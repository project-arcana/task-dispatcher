#include "scheduler.hh"

#include <cstdint>
#include <cstdio>

#include <immintrin.h>

#include <clean-core/array.hh>
#include <clean-core/assert.hh>
#include <clean-core/macros.hh>
#include <clean-core/spin_lock.hh>
#include <clean-core/utility.hh>
#include <clean-core/vector.hh>

#ifdef TD_HAS_RICH_LOG
#include <rich-log/StdOutLogger.hh>
#endif

#include "container/mpsc_queue.hh"
#include "container/spmc_queue.hh"
#include "native/fiber.hh"
#include "native/thread.hh"
#include "native/util.hh"

namespace
{
// Configure task distribution strategy
// If true:
//  use per-thread dynamically growing Chase-Lev SPMC Workstealing Queues/Deques
//      located in tls_t::chase_lev_worker and chase_lev_stealers
//  push submitted tasks to local queue
//  try to pop local queue, otherwise steal from a different one
//      in tls_t::get_task
// If false:
//  use a single, fixed size MPMC queue
//      in Scheduler::mTasks
constexpr bool const gc_use_workstealing = false;

// If true, never wait for events and leave worker threads always spinning (minimized latency, cores locked to 100%)
constexpr bool const gc_never_wait
#ifdef TD_NO_WAITS
    = true;
#else
    = false;
#endif

// If true, print a warning to stderr if waiting on the work event times out (usually not an error)
constexpr bool const gc_warn_timeouts = false;

// If true, print a warning to stderr if a deadlock is heuristically detected
constexpr bool const gc_warn_deadlocks = true;

thread_local td::Scheduler* sCurrentScheduler = nullptr;
}


namespace td
{
using resumable_fiber_mpsc_queue = container::FIFOQueue<Scheduler::fiber_index_t, 32>;

struct Scheduler::atomic_counter_t
{
    struct waiting_fiber_t
    {
        int counter_target = 0;                              // the counter value that this fiber is waiting for
        fiber_index_t fiber_index = invalid_fiber;           // index of the waiting fiber
        thread_index_t pinned_thread_index = invalid_thread; // index of the thread this fiber is pinned to, invalid_thread if unpinned
        std::atomic_bool in_use{true};                       // whether this slot in the array is currently being processed
    };

    std::atomic<int> count; // The value of this counter

    static constexpr uint32_t max_waiting = 32;
    cc::array<waiting_fiber_t, max_waiting> waiting_fibers = {};
    cc::array<std::atomic_bool, max_waiting> free_waiting_slots = {};

    // Resets this counter for re-use
    void reset()
    {
        count.store(0, std::memory_order_release);

        for (auto i = 0u; i < max_waiting; ++i)
            free_waiting_slots[i].store(true);
    }

    friend td::Scheduler;
};

enum class Scheduler::fiber_destination_e : uint8_t
{
    none,
    waiting,
    pool
};

struct Scheduler::worker_thread_t
{
    native::thread_t native = {};

    // queue containing fibers that are pinned to this thread are ready to resume
    // same restrictions as for _resumable_fibers apply (worker_fiber_t::is_waiting_cleaned_up)
    resumable_fiber_mpsc_queue pinned_resumable_fibers = {};
    // note that this queue uses a spinlock instead of being lock free (TODO)
    cc::spin_lock pinned_resumable_fibers_lock = {};
};

struct Scheduler::worker_fiber_t
{
    native::fiber_t native = {};

    // True if this fiber is currently waiting (called yield_to_fiber with destination waiting)
    // and has been cleaned up by the fiber it yielded to (via clean_up_prev_fiber)
    std::atomic_bool is_waiting_cleaned_up{false};
};

struct Scheduler::tls_t
{
    native::fiber_t thread_fiber = {}; // thread fiber, not part of scheduler::mFibers

    fiber_index_t current_fiber_index = invalid_fiber;
    fiber_index_t previous_fiber_index = invalid_fiber;

    fiber_destination_e previous_fiber_dest = fiber_destination_e::none;

    thread_index_t thread_index = invalid_thread; // index of this thread in the scheduler::mThreads
    bool is_thread_waiting = false;

    container::spmc::Worker<container::task> chase_lev_worker;
    cc::vector<container::spmc::Stealer<container::task>> chase_lev_stealers;
    thread_index_t last_steal_target = invalid_thread;

    void reset()
    {
        thread_fiber = native::fiber_t{};
        current_fiber_index = invalid_fiber;
        previous_fiber_index = invalid_fiber;
        previous_fiber_dest = fiber_destination_e::none;
        thread_index = invalid_thread;
        is_thread_waiting = false;
        chase_lev_worker.setDeque(nullptr);
        chase_lev_stealers.clear();
        last_steal_target = invalid_thread;
    }

    void prepare_chase_lev(cc::vector<std::shared_ptr<container::spmc::Deque<container::task>>> const& deques, thread_index_t index)
    {
        // The chase lev deque this thread owns
        auto const& own_deque = deques[index];
        // create worker for it
        chase_lev_worker.setDeque(own_deque);

        // Create stealers for the remaining chase lev deques
        chase_lev_stealers.reserve(deques.size() - 1);
        for (auto t_i = 0u; t_i < deques.size(); ++t_i)
        {
            if (t_i != index)
                chase_lev_stealers.emplace_back(deques[t_i]);
        }

        last_steal_target = index + 1;
    }

    bool get_task(container::task& out_ref)
    {
        if (chase_lev_worker.pop(out_ref))
            return true;

        for (auto i = 0u; i < chase_lev_stealers.size(); ++i)
        {
            thread_index_t const target_i = (last_steal_target + i) % static_cast<thread_index_t>(chase_lev_stealers.size());
            if (chase_lev_stealers[target_i].steal(out_ref))
            {
                last_steal_target = target_i;
                return true;
            }
        }

        return false;
    }
};

}

namespace
{
thread_local td::Scheduler::tls_t s_tls;
}

namespace td
{
struct Scheduler::callback_funcs
{
    struct primary_fiber_arg_t
    {
        Scheduler* owning_scheduler;
        container::task main_task;
    };

    struct worker_arg_t
    {
        thread_index_t index;
        td::Scheduler* owning_scheduler;
        cc::function_ptr<void(uint32_t, bool, void*)> thread_startstop_func;
        void* thread_startstop_userdata;

        cc::vector<std::shared_ptr<container::spmc::Deque<container::task>>> chase_lev_deques;
    };

    static TD_NATIVE_THREAD_FUNC_DECL worker_func(void* arg_void)
    {
        worker_arg_t* const worker_arg = static_cast<worker_arg_t*>(arg_void);

        // Register thread local current scheduler variable
        Scheduler* const scheduler = worker_arg->owning_scheduler;
        sCurrentScheduler = scheduler;

        s_tls.reset();
        s_tls.thread_index = worker_arg->index;

        // worker thread startup tasks
#ifdef TD_HAS_RICH_LOG
        // set the rich-log thread name (shown as a prefix)
        rlog::setCurrentThreadName("td#%02u", worker_arg->index);
#endif
        // set the thead name for debuggers
        native::set_current_thread_debug_name(int(worker_arg->index));

        // optionally call user provided startup function
        if (worker_arg->thread_startstop_func)
        {
            worker_arg->thread_startstop_func(uint32_t(worker_arg->index), true, worker_arg->thread_startstop_userdata);
        }

        // Set up chase lev deques
        if constexpr (gc_use_workstealing)
            s_tls.prepare_chase_lev(worker_arg->chase_lev_deques, worker_arg->index);

        // Set up thread fiber
        native::create_main_fiber(s_tls.thread_fiber);

        // ------
        // main work, on main worker fiber
        {
            s_tls.current_fiber_index = scheduler->acquireFreeFiber();
            auto& fiber = scheduler->mFibers[s_tls.current_fiber_index].native;

            native::switch_to_fiber(fiber, s_tls.thread_fiber);
        }
        // returned, shutdown worker thread
        // ------

        // optionally call user provided shutdown function
        if (worker_arg->thread_startstop_func)
        {
            worker_arg->thread_startstop_func(uint32_t(worker_arg->index), false, worker_arg->thread_startstop_userdata);
        }

        // Clean up allocated argument
        delete worker_arg;

        native::delete_main_fiber(s_tls.thread_fiber);
        native::end_current_thread();


        TD_NATIVE_THREAD_FUNC_END;
    }

    static void fiber_func(void* arg_void)
    {
        Scheduler* const scheduler = static_cast<class td::Scheduler*>(arg_void);

#ifdef CC_OS_WINDOWS
        __try
#endif
        {
            scheduler->cleanUpPrevFiber();

            constexpr uint32_t lc_max_backoff_pauses = 1 << 10;
            constexpr uint32_t lc_min_backoff_pauses = 1;
            uint32_t backoff_num_pauses = lc_min_backoff_pauses;

            container::task task;
            while (!scheduler->mIsShuttingDown.load(std::memory_order_relaxed))
            {
                if (scheduler->getNextTask(task))
                {
                    // work available, reset backoff
                    backoff_num_pauses = lc_min_backoff_pauses;

                    // Received a task, execute it
                    task.execute_and_cleanup();

                    // The task returned, decrement the counter
                    scheduler->counterIncrement(scheduler->mCounters[task.get_metadata()], -1);
                }
                else
                {
                    // No tasks available

                    if constexpr (gc_never_wait)
                    {
                        // Immediately retry

                        // SSE2 pause instruction
                        // hints the CPU that this is a spin-wait, improving power usage
                        // and post-loop wakeup time (falls back to nop on pre-SSE2)
                        // (not at all OS scheduler related, locks cores at 100%)
                        _mm_pause();
                    }
                    else
                    {
                        // only perform OS wait if backoff is at maximum and this thread is not waiting
                        if (backoff_num_pauses == lc_max_backoff_pauses && !s_tls.is_thread_waiting)
                        {
                            // reached max backoff, wait for global event

                            // wait until the global event is signalled, with timeout
                            bool signalled = native::wait_for_event(*scheduler->mEventWorkAvailable, 10);

                            if constexpr (gc_warn_timeouts)
                            {
                                if (!signalled)
                                {
                                    std::fprintf(stderr, "[td] Scheduler warning: Work event wait timed out\n");
                                }
                            }
                            else
                            {
                                (void)signalled;
                            }
                        }
                        else
                        {
                            // increase backoff pauses exponentially
                            backoff_num_pauses = cc::min(lc_max_backoff_pauses, backoff_num_pauses << 1);

                            // perform pauses
                            for (auto _ = 0u; _ < backoff_num_pauses; ++_)
                            {
                                _mm_pause();
                            }
                        }
                    }
                }
            }

            // Switch back to thread fiber of the current thread
            native::switch_to_fiber(s_tls.thread_fiber, scheduler->mFibers[s_tls.current_fiber_index].native);

            CC_RUNTIME_ASSERT(false && "Reached end of fiber_func");
        }
#ifdef CC_OS_WINDOWS
        __except (scheduler->mConfig.fiber_seh_filter(GetExceptionInformation()))
        {
        }
#endif // CC_OS_WINDOWS
    }

    static void primary_fiber_func(void* arg_void)
    {
        primary_fiber_arg_t& arg = *static_cast<primary_fiber_arg_t*>(arg_void);

        // Run main task
        arg.main_task.execute_and_cleanup();

        // Shut down
        arg.owning_scheduler->mIsShuttingDown.store(true, std::memory_order_release);

        // Return to main thread fiber
        native::switch_to_fiber(s_tls.thread_fiber, arg.owning_scheduler->mFibers[s_tls.current_fiber_index].native);

        CC_RUNTIME_ASSERT(false && "Reached end of primary_fiber_func");
    }
};
}

td::Scheduler::fiber_index_t td::Scheduler::acquireFreeFiber()
{
    fiber_index_t res;
    for (auto attempt = 0;; ++attempt)
    {
        if (mIdleFibers.dequeue(res))
            return res;

        if constexpr (gc_warn_deadlocks)
        {
            if (attempt > 10)
            {
                std::fprintf(stderr, "[td] Scheduler warning: Failing to find free fiber, possibly deadlocked\n");
            }
        }
    }
}

td::handle::counter td::Scheduler::acquireCounterHandle()
{
    counter_index_t free_counter;
    auto success = mFreeCounters.dequeue(free_counter);
    CC_RUNTIME_ASSERT(success && "No free counters available, consider increasing config.max_num_counters");
    mCounters[free_counter].reset();

    auto const res = mCounterHandles.acquire();
    mCounterHandles.get(res).counterIndex = free_counter;
    return td::handle::counter{res};
}

int td::Scheduler::releaseCounter(td::handle::counter c)
{
    counter_index_t const freed_counter = mCounterHandles.get(c._value).counterIndex;
    int const last_state = mCounters[freed_counter].count.load(std::memory_order_acquire);

    mCounterHandles.release(c._value);
    bool success = mFreeCounters.enqueue(freed_counter);
    CC_ASSERT(success);

    return last_state;
}

bool td::Scheduler::releaseCounterIfOnTarget(td::handle::counter c, int target)
{
    counter_index_t const freed_counter = mCounterHandles.get(c._value).counterIndex;
    int const last_state = mCounters[freed_counter].count.load(std::memory_order_acquire);
    if (last_state != target)
    {
        return false;
    }

    mCounterHandles.release(c._value);
    bool success = mFreeCounters.enqueue(freed_counter);
    CC_ASSERT(success);

    return true;
}

void td::Scheduler::yieldToFiber(td::Scheduler::fiber_index_t target_fiber, td::Scheduler::fiber_destination_e own_destination)
{
    s_tls.previous_fiber_index = s_tls.current_fiber_index;
    s_tls.previous_fiber_dest = own_destination;
    s_tls.current_fiber_index = target_fiber;

    CC_ASSERT(s_tls.previous_fiber_index != invalid_fiber && s_tls.current_fiber_index != invalid_fiber);

    native::switch_to_fiber(mFibers[s_tls.current_fiber_index].native, mFibers[s_tls.previous_fiber_index].native);
    cleanUpPrevFiber();
}

void td::Scheduler::cleanUpPrevFiber()
{
    switch (s_tls.previous_fiber_dest)
    {
    case fiber_destination_e::none:
        return;
    case fiber_destination_e::pool:
        // The fiber is being pooled, add it to the idle fibers
        {
            bool success = mIdleFibers.enqueue(s_tls.previous_fiber_index);
            CC_ASSERT(success);
        }
        break;
    case fiber_destination_e::waiting:
        // The fiber is waiting for a dependency, and can be safely resumed from now on
        // KW_LOG_DIAG("[clean_up_prev_fiber] Waiting fiber " << s_tls.previous_fiber_index << " cleaned up");
        mFibers[s_tls.previous_fiber_index].is_waiting_cleaned_up.store(true, std::memory_order_relaxed);
        break;
    }

    s_tls.previous_fiber_index = invalid_fiber;
    s_tls.previous_fiber_dest = fiber_destination_e::none;
}

bool td::Scheduler::getNextTask(td::container::task& task)
{
    // Sleeping fibers with tasks that had their dependencies resolved in the meantime
    // have the highest priority

    // Locally pinned fibers first
    {
        auto& local_thread = mThreads[s_tls.thread_index];
        fiber_index_t resumable_fiber_index;
        bool got_resumable;
        {
            auto lg = cc::lock_guard(local_thread.pinned_resumable_fibers_lock);
            got_resumable = local_thread.pinned_resumable_fibers.dequeue(resumable_fiber_index);
        }

        if (got_resumable)
        {
            if (tryResumeFiber(resumable_fiber_index))
            {
                // Successfully resumed (and returned)
            }
            else
            {
                // Received fiber is not cleaned up yet, re-enqueue (very rare)
                {
                    auto lg = cc::lock_guard(local_thread.pinned_resumable_fibers_lock);
                    local_thread.pinned_resumable_fibers.enqueue(resumable_fiber_index);
                }

                // signal the global event
                native::signal_event(*mEventWorkAvailable);
            }
            // Fallthrough to global resumables
        }
    }

    // Global resumable fibers
    {
        fiber_index_t resumable_fiber_index;
        if (mResumableFibers.dequeue(resumable_fiber_index))
        {
            if (tryResumeFiber(resumable_fiber_index))
            {
                // Successfully resumed (and returned)
            }
            else
            {
                // Received fiber is not cleaned up yet, re-enqueue (very rare)
                // This should only happen if mResumableFibers is almost empty, and
                // the latency impact is low in those cases
                bool success = mResumableFibers.enqueue(resumable_fiber_index);
                CC_ASSERT(success);

                // signal the global event
                native::signal_event(*mEventWorkAvailable);
            }
            // Fallthrough to global pending Chase-Lev / MPMC
        }
    }

    // Pending tasks
    if constexpr (gc_use_workstealing)
        return s_tls.get_task(task);
    else
        return mTasks.dequeue(task);
}

bool td::Scheduler::tryResumeFiber(td::Scheduler::fiber_index_t fiber)
{
    bool expected = true;
    auto const cas_success = std::atomic_compare_exchange_strong_explicit(&mFibers[fiber].is_waiting_cleaned_up, &expected, false, //
                                                                          std::memory_order_seq_cst, std::memory_order_relaxed);
    if (CC_LIKELY(cas_success))
    {
        // is_waiting_cleaned_up was true, and is now exchanged to false
        // The resumable fiber is properly cleaned up and can be switched to

        // KW_LOG_DIAG("[get_next_task] Acquired resumable fiber " << int(fiber) << " which is cleaned up, yielding");
        yieldToFiber(fiber, fiber_destination_e::pool);

        // returned, resume was successful
        return true;
    }
    else
    {
        // The resumable fiber is not yet cleaned up
        return false;
    }
}

bool td::Scheduler::counterAddWaitingFiber(td::Scheduler::atomic_counter_t& counter, fiber_index_t fiber_index, thread_index_t pinned_thread_index, int counter_target, int& out_counter_val)
{
    for (auto i = 0u; i < atomic_counter_t::max_waiting; ++i)
    {
        // Acquire free waiting slot
        bool expected = true;
        if (!std::atomic_compare_exchange_strong_explicit(&counter.free_waiting_slots[i], &expected, false, //
                                                          std::memory_order_seq_cst, std::memory_order_relaxed))
            continue;

        atomic_counter_t::waiting_fiber_t& slot = counter.waiting_fibers[i];
        slot.fiber_index = fiber_index;
        slot.counter_target = counter_target;
        slot.pinned_thread_index = pinned_thread_index;
        slot.in_use.store(false);

        // Check if already done
        auto const counter_val = counter.count.load(std::memory_order_relaxed);
        out_counter_val = counter_val;

        if (slot.in_use.load(std::memory_order_acquire))
            return false;

        if (slot.counter_target == counter_val)
        {
            expected = false;
            if (!std::atomic_compare_exchange_strong_explicit(&slot.in_use, &expected, true, //
                                                              std::memory_order_seq_cst, std::memory_order_relaxed))
                return false;

            counter.free_waiting_slots[i].store(true, std::memory_order_release);
            return true;
        }

        return false;
    }

    // Panic if there is no space left in counter waiting_slots
    CC_RUNTIME_ASSERT(false && "Counter waiting slots are full");
    return false;
}

void td::Scheduler::counterCheckWaitingFibers(td::Scheduler::atomic_counter_t& counter, int value)
{
    // Go over each waiting fiber slot
    for (auto i = 0u; i < atomic_counter_t::max_waiting; ++i)
    {
        // Skip free slots
        if (counter.free_waiting_slots[i].load(std::memory_order_acquire))
            continue;

        auto& slot = counter.waiting_fibers[i];

        // Skip the slot if it is in use already
        if (slot.in_use.load(std::memory_order_acquire))
            continue;

        // If this slot's dependency is met
        if (slot.counter_target == value)
        {
            // Lock the slot to be used by this thread
            bool expected = false;
            if (!std::atomic_compare_exchange_strong_explicit(&slot.in_use, &expected, true, //
                                                              std::memory_order_seq_cst, std::memory_order_relaxed))
            {
                // Failed to lock, this slot is already being handled on a different thread (which stole it right between the two checks)
                continue;
            }

            // KW_LOG_DIAG("[cnst_check_waiting_fibers] Counter reached " << value << ", making waiting fiber " << slot.fiber_index << " resumable");


            // The waiting fiber is ready, and locked by this thread

            if (slot.pinned_thread_index == invalid_thread)
            {
                // The waiting fiber is not pinned to any thread, store it in the global _resumable fibers
                bool success = mResumableFibers.enqueue(slot.fiber_index);

                // This should never fail, the container is large enough for all fibers in the system
                CC_ASSERT(success);
            }
            else
            {
                // The waiting fiber is pinned to a certain thread, store it there
                auto& pinned_thread = mThreads[slot.pinned_thread_index];


                auto lg = cc::lock_guard(pinned_thread.pinned_resumable_fibers_lock);
                pinned_thread.pinned_resumable_fibers.enqueue(slot.fiber_index);
            }

            // signal the global event
            native::signal_event(*mEventWorkAvailable);

            // Free the slot
            counter.free_waiting_slots[i].store(true, std::memory_order_release);
        }
    }
}

int td::Scheduler::counterIncrement(td::Scheduler::atomic_counter_t& counter, int amount)
{
    CC_ASSERT(amount != 0 && "invalid counter increment value");
    auto previous = counter.count.fetch_add(amount);
    auto const new_val = previous + amount;
    counterCheckWaitingFibers(counter, new_val);
    return new_val;
}

td::Scheduler::Scheduler(scheduler_config const& config)
  : mConfig(config), //
    mTasks(config.max_num_tasks, config.static_alloc),
    mIdleFibers(config.num_fibers, config.static_alloc),
    mResumableFibers(config.num_fibers, config.static_alloc),
    mFreeCounters(config.max_num_counters, config.static_alloc)
{
    CC_ASSERT(config.is_valid() && "Scheduler config invalid, use scheduler_config_t::validate()");
    CC_ASSERT((config.num_threads <= system::num_logical_cores()) && "More threads than physical cores configured");

    mEventWorkAvailable = config.static_alloc->new_t<native::event_t>();
}

td::Scheduler::~Scheduler()
{
    mConfig.static_alloc->delete_t(mEventWorkAvailable);
    mEventWorkAvailable = nullptr;
}

void td::Scheduler::submitTasks(td::container::task* tasks, uint32_t num_tasks, handle::counter c)
{
    counter_index_t const counter_index = mCounterHandles.get(c._value).counterIndex;
    counterIncrement(mCounters[counter_index], int(num_tasks));

    // TODO: Multi-enqueue
    bool success = true;
    for (auto i = 0u; i < num_tasks; ++i)
    {
        tasks[i].set_metadata(counter_index);

        if constexpr (gc_use_workstealing)
            s_tls.chase_lev_worker.push(tasks[i]);
        else
            success &= mTasks.enqueue(tasks[i]);
    }

    // signal the global event
    native::signal_event(*mEventWorkAvailable);

    CC_RUNTIME_ASSERT(success && "Task queue is full, consider increasing config.max_num_tasks");
}

int td::Scheduler::wait(handle::counter c, bool pinnned, int target)
{
    CC_ASSERT(target >= 0 && "sync counter target must not be negative");
    CC_ASSERT(mCounterHandles.is_alive(c._value) && "waited on expired counter handle");

    auto const counter_index = mCounterHandles.get(c._value).counterIndex;

    // The current fiber is now waiting, but not yet cleaned up
    mFibers[s_tls.current_fiber_index].is_waiting_cleaned_up.store(false, std::memory_order_release);

    s_tls.is_thread_waiting = true;

    int counter_value_before_wait = -1;
    if (counterAddWaitingFiber(mCounters[counter_index], s_tls.current_fiber_index, pinnned ? s_tls.thread_index : invalid_thread, target, counter_value_before_wait))
    {
        // Already done
        //        KW_LOG_DIAG("[wait] Wait for counter " << int(counter_index) << " is over early, resuming immediately");
    }
    else
    {
        // Not already done, prepare to yield
        //        KW_LOG_DIAG("[wait] Waiting for counter " << int(counter_index) << ", yielding");
        yieldToFiber(acquireFreeFiber(), fiber_destination_e::waiting);
    }

    s_tls.is_thread_waiting = false;

    // Either the counter was already on target, or this fiber has been awakened because it is now on target,
    // return execution

    return counter_value_before_wait;
}

int td::Scheduler::incrementCounter(td::handle::counter c, uint32_t amount)
{
    auto const counter_index = mCounterHandles.get(c._value).counterIndex;
    return counterIncrement(mCounters[counter_index], int(amount));
}

int td::Scheduler::decrementCounter(td::handle::counter c, uint32_t amount)
{
    auto const counter_index = mCounterHandles.get(c._value).counterIndex;
    return counterIncrement(mCounters[counter_index], int(amount) * -1);
}

td::Scheduler& td::Scheduler::Current() { return *sCurrentScheduler; }

bool td::Scheduler::IsInsideScheduler() { return sCurrentScheduler != nullptr; }

uint32_t td::Scheduler::CurrentThreadIndex() { return s_tls.thread_index; }
uint32_t td::Scheduler::CurrentFiberIndex() { return s_tls.current_fiber_index; }

void td::Scheduler::start(td::container::task main_task)
{
    // Re-default all arrays, as multiple starts are possible
    mThreads.reset(mConfig.static_alloc, mConfig.num_threads);
    mFibers.reset(mConfig.static_alloc, mConfig.num_fibers);
    mCounters.reset(mConfig.static_alloc, mConfig.max_num_counters);

    mCounterHandles.initialize(mConfig.max_num_counters, mConfig.static_alloc);

    if (!mConfig.fiber_seh_filter)
    {
        // use an empty SEH handler if none is specified
        mConfig.fiber_seh_filter = [](void*) -> int32_t { return 0; };
    }

    mIsShuttingDown.store(false, std::memory_order_seq_cst);

    native::create_event(mEventWorkAvailable);

#ifdef CC_OS_WINDOWS
    // attempt to make the win32 scheduler as granular as possible for faster Sleep(1)
    bool applied_win32_sched_change = false;
    if (native::win32_init_utils())
    {
        applied_win32_sched_change = native::win32_enable_scheduler_granular();
    }
#endif


    // Initialize main thread variables, create the thread fiber
    // The main thread is thread 0 by convention
    auto& main_thread = mThreads[0];
    {
        main_thread.native = native::get_current_thread();

        if (mConfig.pin_threads_to_cores)
        {
            // lock main thread to core N
            // (core 0 is conventionally reserved for OS operations and driver interrupts, poor fit for the main thread)
            native::set_current_thread_affinity(mThreads.size() - 1);
        }

        s_tls.reset();
        sCurrentScheduler = this;

        // Create main fiber on this thread
        native::create_main_fiber(s_tls.thread_fiber);

#ifdef TD_HAS_RICH_LOG
        rlog::setCurrentThreadName("td#00");
#endif
    }

    // Populate fiber pool
    for (fiber_index_t i = 0; i < mFibers.size(); ++i)
    {
        native::create_fiber(mFibers[i].native, callback_funcs::fiber_func, this, mConfig.fiber_stack_size);
        bool success = mIdleFibers.enqueue(i);
        CC_ASSERT(success);
    }

    // Populate free counter queue
    for (counter_index_t i = 0; i < mCounters.size(); ++i)
    {
        mFreeCounters.enqueue(i);
    }

    // Launch worker threads, starting at 1
    {
        cc::vector<std::shared_ptr<container::spmc::Deque<container::task>>> thread_deques;

        s_tls.thread_index = 0;

        if constexpr (gc_use_workstealing)
        {
            thread_deques.reserve(mThreads.size());
            for (auto i = 0u; i < mThreads.size(); ++i)
                thread_deques.push_back(std::make_shared<container::spmc::Deque<container::task>>(8));

            s_tls.prepare_chase_lev(thread_deques, 0);
        }

        for (thread_index_t i = 1; i < mThreads.size(); ++i)
        {
            worker_thread_t& thread = mThreads[i];

            // TODO: Adjust this stack size
            // On Win 10 1803 and Linux 4.18 this seems to be entirely irrelevant
            auto constexpr thread_stack_overhead_safety = sizeof(void*) * 16;

            // Prepare worker arg
            callback_funcs::worker_arg_t* const worker_arg = new callback_funcs::worker_arg_t();
            worker_arg->index = i;
            worker_arg->owning_scheduler = this;
            worker_arg->thread_startstop_func = mConfig.worker_thread_startstop_function;
            worker_arg->thread_startstop_userdata = mConfig.worker_thread_startstop_userdata;
            worker_arg->chase_lev_deques = thread_deques;

            bool success = false;
            if (mConfig.pin_threads_to_cores)
            {
                // Create the thread, pinned to core (i - 1), the main thread occupies core N
                uint32_t const pinned_core_index = i - 1;
                success = native::create_thread(mConfig.fiber_stack_size + thread_stack_overhead_safety, callback_funcs::worker_func, worker_arg,
                                                pinned_core_index, &thread.native);
            }
            else
            {
                success = native::create_thread(mConfig.fiber_stack_size + thread_stack_overhead_safety, callback_funcs::worker_func, worker_arg,
                                                &thread.native);
            }
            CC_ASSERT(success && "Failed to create worker thread");
        }
    }

    // Prepare the primary fiber
    {
        // Prepare the args for the primary fiber
        callback_funcs::primary_fiber_arg_t primary_fiber_arg;
        primary_fiber_arg.owning_scheduler = this;
        primary_fiber_arg.main_task = main_task;

        s_tls.current_fiber_index = acquireFreeFiber();
        auto& initial_fiber = mFibers[s_tls.current_fiber_index];

        // reset the fiber, creating the primary fiber
        native::delete_fiber(initial_fiber.native);
        native::create_fiber(initial_fiber.native, callback_funcs::primary_fiber_func, &primary_fiber_arg, mConfig.fiber_stack_size);

        // Launch the primary fiber
        native::switch_to_fiber(initial_fiber.native, s_tls.thread_fiber);
    }

    // The primary fiber has returned, begin shutdown
    {
        // Spin until shutdown has propagated
        while (mIsShuttingDown.load(std::memory_order_seq_cst) != true)
        {
            _mm_pause();
        }

        // wake up all threads
        native::signal_event(*mEventWorkAvailable);

        // Delete the main fiber
        native::delete_main_fiber(s_tls.thread_fiber);

        // Join worker threads, starting at 1
        for (auto i = 1u; i < mThreads.size(); ++i)
        {
            // re-signal before joining each thread
            native::signal_event(*mEventWorkAvailable);
            native::join_thread(mThreads[i].native);
        }

        // Clean up
        {
            for (auto& fib : mFibers)
                native::delete_fiber(fib.native);

            // Empty queues
            container::task task_dump;
            fiber_index_t fiber_dump;
            counter_index_t counter_dump;
            while (mTasks.dequeue(task_dump))
            {
                // Spin
            }

            while (mIdleFibers.dequeue(fiber_dump))
            {
                // Spin
            }

            while (mResumableFibers.dequeue(fiber_dump))
            {
                // Spin
            }

            while (mFreeCounters.dequeue(counter_dump))
            {
                // Spin
            }

            // Reset counter handles
            mCounterHandles.destroy();

            // Clear sCurrentScheduler
            sCurrentScheduler = nullptr;
        }
    }

#ifdef CC_OS_WINDOWS
    // undo the changes made to the win32 scheduler
    if (applied_win32_sched_change)
        native::win32_disable_scheduler_granular();
    native::win32_shutdown_utils();
#endif

    native::destroy_event(*mEventWorkAvailable);
}
