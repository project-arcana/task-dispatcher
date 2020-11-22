#include "scheduler.hh"

#include <cstdio>

#include <immintrin.h>

#include <clean-core/allocate.hh>
#include <clean-core/array.hh>
#include <clean-core/assert.hh>
#include <clean-core/macros.hh>
#include <clean-core/utility.hh>
#include <clean-core/vector.hh>


#ifdef TD_HAS_RICH_LOG
#include <rich-log/logger.hh>
#endif

#include "common/spin_lock.hh"
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

enum class WaitingSlotState : int32_t
{
    Empty,
    Locked,
    Full
};
}

thread_local td::Scheduler* td::Scheduler::sCurrentScheduler = nullptr;

namespace td
{
using resumable_fiber_mpsc_queue = container::FIFOQueue<Scheduler::fiber_index_t, 32>;

struct alignas(64) Scheduler::atomic_counter_t
{
    struct waiting_fiber_t
    {
        int32_t counter_target = 0;                          // the counter value that this fiber is waiting for
        fiber_index_t fiber_index = invalid_fiber;           // index of the waiting fiber
        thread_index_t pinned_thread_index = invalid_thread; // index of the thread this fiber is pinned to, invalid_thread if unpinned

        bool is_empty() { return state.load(std::memory_order_relaxed) == WaitingSlotState::Empty; }

        bool try_lock_from_empty()
        {
            WaitingSlotState expected = WaitingSlotState::Empty;
            return std::atomic_compare_exchange_strong(&state, &expected, WaitingSlotState::Locked);
        }

        bool try_lock_from_full()
        {
            WaitingSlotState expected = WaitingSlotState::Full;
            return std::atomic_compare_exchange_strong(&state, &expected, WaitingSlotState::Locked);
        }

        bool cas_full_to_empty()
        {
            WaitingSlotState expected = WaitingSlotState::Full;
            return std::atomic_compare_exchange_strong(&state, &expected, WaitingSlotState::Empty);
        }

        void unlock_to_empty()
        {
            auto const prevState = state.exchange(WaitingSlotState::Empty);
            CC_ASSERT(prevState == WaitingSlotState::Locked && "unexpected race on slot state");
        }

        void unlock_to_full()
        {
            auto const prevState = state.exchange(WaitingSlotState::Full);
            CC_ASSERT(prevState == WaitingSlotState::Locked && "unexpected race on slot state");
        }

    private:
        std::atomic<WaitingSlotState> state = {WaitingSlotState::Empty};
    };

    std::atomic_int count; // The value of this counter

    static constexpr unsigned max_waiting = 16;
    cc::array<waiting_fiber_t, max_waiting> waiting_fibers = {};

    friend td::Scheduler;
};

enum class Scheduler::fiber_destination_e : cc::uint8
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
    SpinLock pinned_resumable_fibers_lock = {};
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
        thread_index_t const index;
        td::Scheduler* const owning_scheduler;
        cc::vector<std::shared_ptr<container::spmc::Deque<container::task>>> const chase_lev_deques;
    };

    static TD_NATIVE_THREAD_FUNC_DECL worker_func(void* arg_void)
    {
        worker_arg_t const* const worker_arg = static_cast<worker_arg_t*>(arg_void);

        // Register thread local current scheduler variable
        Scheduler* const scheduler = worker_arg->owning_scheduler;
        scheduler->sCurrentScheduler = scheduler;

        s_tls.reset();
        s_tls.thread_index = worker_arg->index;

#ifdef TD_HAS_RICH_LOG
        rlog::set_current_thread_name("td#%.2u", worker_arg->index);
#endif

        // Set up chase lev deques
        if constexpr (gc_use_workstealing)
            s_tls.prepare_chase_lev(worker_arg->chase_lev_deques, worker_arg->index);

        // Clean up allocated argument
        cc::free(worker_arg);

        // Set up thread fiber
        native::create_main_fiber(s_tls.thread_fiber);

        {
            s_tls.current_fiber_index = scheduler->acquireFreeFiber();
            auto& fiber = scheduler->mFibers[s_tls.current_fiber_index].native;

            native::switch_to_fiber(fiber, s_tls.thread_fiber);
        }

        native::delete_main_fiber(s_tls.thread_fiber);

        scheduler->sCurrentScheduler = nullptr;
        s_tls.reset();

        native::end_current_thread();

        TD_NATIVE_THREAD_FUNC_END;
    }

    static void fiber_func(void* arg_void)
    {
        Scheduler* scheduler = static_cast<class td::Scheduler*>(arg_void);
        scheduler->cleanUpPrevFiber();

        constexpr unsigned lc_max_backoff_pauses = 1 << 10;
        constexpr unsigned lc_min_backoff_pauses = 1;
        unsigned backoff_num_pauses = lc_min_backoff_pauses;

        container::task task;
        while (!scheduler->mIsShuttingDown.load(std::memory_order_relaxed))
        {
            if (scheduler->getNextTask(task))
            {
                // work available, reset backoff
                backoff_num_pauses = lc_min_backoff_pauses;

                // Received a task, execute it
                task.execute_and_cleanup();

                // The task returned, decrement the counter if present
                auto const associated_counter = handle::counter{task.get_metadata()};
                if (associated_counter.is_valid())
                {
                    scheduler->counterIncrement(scheduler->mCounters.get(associated_counter._value), -1);
                }
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
                    if (backoff_num_pauses == lc_max_backoff_pauses)
                    {
                        // reached max backoff, wait for global event

                        // wait until the global event is signalled, with timeout
                        auto const signalled = native::wait_for_event(*scheduler->mEventWorkAvailable, 50);

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
        mIdleFibers.enqueue(s_tls.previous_fiber_index);
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
        fiber_index_t resumable_fiber_index = invalid_fiber;
        bool got_resumable = false;

        {
            auto lg = LockGuard(local_thread.pinned_resumable_fibers_lock);
            got_resumable = local_thread.pinned_resumable_fibers.dequeue(resumable_fiber_index);
        }

        if (got_resumable)
        {
            CC_ASSERT(resumable_fiber_index < mFibers.size() && "fatal: received invalid fiber index from resumable queue");

            if (tryResumeFiber(resumable_fiber_index))
            {
                // Successfully resumed (and returned)
            }
            else
            {
                // Received fiber is not cleaned up yet, re-enqueue (very rare)
                {
                    auto lg = LockGuard(local_thread.pinned_resumable_fibers_lock);
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
        fiber_index_t resumable_fiber_index = invalid_fiber;
        bool got_resumable = mResumableFibers.dequeue(resumable_fiber_index);

        if (got_resumable)
        {
            CC_ASSERT(resumable_fiber_index < mFibers.size() && "fatal: received invalid fiber index from resumable queue");

            if (tryResumeFiber(resumable_fiber_index))
            {
                // Successfully resumed (and returned)
            }
            else
            {
                // Received fiber is not cleaned up yet, re-enqueue (very rare)
                // This should only happen if mResumableFibers is almost empty, and
                // the latency impact is low in those cases
                bool const re_enqueue_success = mResumableFibers.enqueue(resumable_fiber_index);
                CC_ASSERT(re_enqueue_success && "fatal: fiber queue full"); // this should never happen

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
    bool const cas_success = std::atomic_compare_exchange_strong_explicit(&mFibers[fiber].is_waiting_cleaned_up, &expected, false, //
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

bool td::Scheduler::counterAddWaitingFiber(td::Scheduler::atomic_counter_t& counter, fiber_index_t fiber_index, thread_index_t pinned_thread_index, int counter_target, int& out_current_counter_value)
{
    CC_ASSERT(fiber_index < mFibers.size() && "attempted to register invalid fiber as waiting");

    for (auto i = 0u; i < atomic_counter_t::max_waiting; ++i)
    {
        atomic_counter_t::waiting_fiber_t& slot = counter.waiting_fibers[i];

        // relaxed load to skip nonempty slots
        if (!slot.is_empty())
        {
            continue;
        }

        // try to lock
        if (!slot.try_lock_from_empty())
        {
            continue;
        }

        // locked, write contents
        slot.fiber_index = fiber_index;
        slot.counter_target = counter_target;
        slot.pinned_thread_index = pinned_thread_index;

        out_current_counter_value = counter.count.load(std::memory_order_relaxed);

        // set to full
        slot.unlock_to_full();

        // Check if already done
        if (counter_target == out_current_counter_value)
        {
            if (!slot.cas_full_to_empty())
            {
                // got raced by other thread in processing this state
                return false;
            }

            // slot (instantly) freed
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
        atomic_counter_t::waiting_fiber_t& slot = counter.waiting_fibers[i];

        // relaxed load to skip empty slots
        if (slot.is_empty())
        {
            continue;
        }

        // If this slot's dependency is met
        if (slot.counter_target == value)
        {
            // Lock the slot to be used by this thread
            if (!slot.try_lock_from_full())
            {
                // Failed to lock, this slot is already being handled on a different thread
                // (which stole it right between the two checks)
                continue;
            }

            // The waiting fiber is ready, and locked by this thread
            auto const fiber_index_to_enqueue = slot.fiber_index;
            CC_ASSERT(fiber_index_to_enqueue < mFibers.size() && "fatal: counter waiting slot has invalid associated fiber");

            auto const read_pinned_thread_index = slot.pinned_thread_index;

            // Free the slot
            slot.unlock_to_empty();

            // NOTE: the slot can now be immediately invalid, the reference must be considered dangling
            // right after enqueuing into the MPMC / pinned resumable FIFO

            if (read_pinned_thread_index == invalid_thread)
            {
                // The waiting fiber is not pinned to any thread, store it in the global _resumable fibers
                bool success = mResumableFibers.enqueue(fiber_index_to_enqueue);

                // This should never fail, the container is large enough for all fibers in the system
                CC_ASSERT(success && "fatal: resumable fiber queue is full");
            }
            else
            {
                // The waiting fiber is pinned to a certain thread, store it there
                CC_ASSERT(read_pinned_thread_index < mThreads.size() && "fatal: counter waiting slot has invalid pinned thread index");
                auto& pinned_thread = mThreads[read_pinned_thread_index];

                {
                    auto lg = LockGuard(pinned_thread.pinned_resumable_fibers_lock);
                    pinned_thread.pinned_resumable_fibers.enqueue(fiber_index_to_enqueue);
                }
            }

            // signal the global event
            native::signal_event(*mEventWorkAvailable);
        }
    }
}

int td::Scheduler::counterIncrement(td::Scheduler::atomic_counter_t& counter, int amount)
{
    CC_ASSERT(amount != 0 && "invalid counter increment value");
    auto const previous = counter.count.fetch_add(amount);
    auto const new_val = previous + amount;
    counterCheckWaitingFibers(counter, new_val);
    return new_val;
}

bool td::Scheduler::enqueueTasks(td::container::task* tasks, unsigned num_tasks, handle::counter counter)
{
    bool success = true;
    for (auto i = 0u; i < num_tasks; ++i)
    {
        tasks[i].set_metadata(counter._value);

        if constexpr (gc_use_workstealing)
            s_tls.chase_lev_worker.push(tasks[i]);
        else
            success &= mTasks.enqueue(tasks[i]);
    }

    // signal the global event
    native::signal_event(*mEventWorkAvailable);

    CC_RUNTIME_ASSERT(success && "Task queue is full, consider increasing config.max_num_tasks");
    return success;
}

td::Scheduler::Scheduler(scheduler_config const& config)
  : mFiberStackSize(config.fiber_stack_size),
    mNumCounters(config.max_num_counters),
    mEnablePinThreads(config.pin_threads_to_cores),
    mThreads(cc::fwd_array<worker_thread_t>::defaulted(config.num_threads)),
    mFibers(cc::fwd_array<worker_fiber_t>::defaulted(config.num_fibers)),
    mTasks(config.max_num_tasks),
    mIdleFibers(mFibers.size()),
    mResumableFibers(mFibers.size())
{
    static_assert(std::is_trivially_destructible_v<Scheduler::atomic_counter_t>, "atomic counter type not trivially destructible");

    mEventWorkAvailable = new native::event_t();

    CC_ASSERT(config.is_valid() && "Scheduler config invalid, use scheduler_config_t::validate()");
    CC_ASSERT((config.num_threads <= system::num_logical_cores()) && "More threads than physical cores configured");
}

td::Scheduler::~Scheduler() { delete mEventWorkAvailable; }

td::handle::counter td::Scheduler::acquireCounterHandle()
{
    auto const res = mCounters.acquire();
    return handle::counter{res};
}

int td::Scheduler::releaseCounter(handle::counter c)
{
    int const last_state = mCounters.get(c._value).count.load(std::memory_order_acquire);
    mCounters.release(c._value);

    return last_state;
}

bool td::Scheduler::releaseCounterIfOnTarget(handle::counter c, int target)
{
    int expected = target;
    bool const cas_success = mCounters.get(c._value).count.compare_exchange_strong(expected, 0);

    if (cas_success)
    {
        mCounters.release(c._value);
    }

    return cas_success;
}

void td::Scheduler::submitTasks(td::container::task* tasks, unsigned num_tasks, handle::counter c)
{
    counterIncrement(mCounters.get(c._value), int(num_tasks));
    enqueueTasks(tasks, num_tasks, c);
}

void td::Scheduler::submitTasksWithoutCounter(td::container::task* tasks, unsigned num_tasks)
{
    enqueueTasks(tasks, num_tasks, handle::null_counter);
}

int td::Scheduler::wait(handle::counter c, bool pinnned, int target)
{
    CC_ASSERT(target >= 0 && "sync counter target must not be negative");

    // The current fiber is now waiting, but not yet cleaned up
    mFibers[s_tls.current_fiber_index].is_waiting_cleaned_up.store(false, std::memory_order_release);

    int current_counter_value = 0;

    if (counterAddWaitingFiber(mCounters.get(c._value), s_tls.current_fiber_index, pinnned ? s_tls.thread_index : invalid_thread, target, current_counter_value))
    {
        // Already done
    }
    else
    {
        // Not already done, prepare to yield
        yieldToFiber(acquireFreeFiber(), fiber_destination_e::waiting);
    }

    // Either the counter was already on target, or this fiber has been awakened because it is now on target,
    // return execution

    return current_counter_value;
}

int td::Scheduler::incrementCounter(handle::counter c, unsigned amount) { return counterIncrement(mCounters.get(c._value), int(amount)); }

int td::Scheduler::decrementCounter(handle::counter c, unsigned amount)
{
    // re-enqueue all waiting tasks as resumable
    return counterIncrement(mCounters.get(c._value), -1 * int(amount));
}

unsigned td::Scheduler::CurrentThreadIndex() { return s_tls.thread_index; }
unsigned td::Scheduler::CurrentFiberIndex() { return s_tls.current_fiber_index; }

void td::Scheduler::start(td::container::task main_task)
{
    // Re-default all arrays, as multiple starts are possible
    mThreads = cc::fwd_array<worker_thread_t>::defaulted(mThreads.size());
    mFibers = cc::fwd_array<worker_fiber_t>::defaulted(mFibers.size());
    mCounters.initialize(mNumCounters);

    mIsShuttingDown.store(false, std::memory_order_seq_cst);

    native::create_event(mEventWorkAvailable);

#ifdef CC_OS_WINDOWS
    // attempt to make the win32 scheduler as granular as possible for faster Sleep(1)
    bool applied_win32_sched_change = false;
    if (native::win32_init_utils())
    {
        applied_win32_sched_change = native::win32_enable_scheduler_granular();
    }
#ifdef TD_HAS_RICH_LOG
    // always enable win32 colors for rich-log
    rlog::enable_win32_colors();
#endif
#endif


    // Initialize main thread variables, create the thread fiber
    // The main thread is thread 0 by convention
    auto& main_thread = mThreads[0];
    {
        main_thread.native = native::get_current_thread();

        if (mEnablePinThreads)
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
        rlog::set_current_thread_name("td#%.2u", 0);
#endif
    }

    // Populate fiber pool
    for (fiber_index_t i = 0; i < mFibers.size(); ++i)
    {
        native::create_fiber(mFibers[i].native, callback_funcs::fiber_func, this, mFiberStackSize);
        mIdleFibers.enqueue(i);
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
            callback_funcs::worker_arg_t* const worker_arg = cc::alloc<callback_funcs::worker_arg_t>(callback_funcs::worker_arg_t{i, this, thread_deques});

            bool success = false;
            if (mEnablePinThreads)
            {
                // Create the thread, pinned to core (i - 1), the main thread occupies core N
                unsigned const pinned_core_index = i - 1;
                success = native::create_thread(mFiberStackSize + thread_stack_overhead_safety, callback_funcs::worker_func, worker_arg,
                                                pinned_core_index, &thread.native);
            }
            else
            {
                success = native::create_thread(mFiberStackSize + thread_stack_overhead_safety, callback_funcs::worker_func, worker_arg, &thread.native);
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
        native::create_fiber(initial_fiber.native, callback_funcs::primary_fiber_func, &primary_fiber_arg, mFiberStackSize);

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
            {
                native::delete_fiber(fib.native);
            }

            // Empty Queues
            // task queue
            {
                container::task task_dump;
                unsigned num_outstanding_tasks = 0;
                while (mTasks.dequeue(task_dump))
                {
                    // Spin
                    ++num_outstanding_tasks;
                }

                if (num_outstanding_tasks > 0)
                {
                    // it's unreasonable to execute skipped tasks serially here (because of dependencies and pinned waits),
                    // the user should instead prevent this by waiting on all syncs before shutdown
                    fprintf(stderr, "[task-dispatcher] warning: skipped %u pending tasks with shutdown\n", num_outstanding_tasks);
                }
            }

            // fiber queues
            {
                fiber_index_t fiber_dump;
                while (mIdleFibers.dequeue(fiber_dump))
                {
                    // Spin
                }

                unsigned num_resumable_fibers = 0;
                while (mResumableFibers.dequeue(fiber_dump))
                {
                    // Spin
                    ++num_resumable_fibers;
                }

                if (num_resumable_fibers > 0)
                {
                    fprintf(stderr, "[task-dispatcher] warning: skipped %u resumable fibers with shutdown\n", num_resumable_fibers);
                }
            }

            mCounters.destroy();

            // Clear sCurrentScheduler
            sCurrentScheduler = nullptr;
            s_tls.reset();
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
