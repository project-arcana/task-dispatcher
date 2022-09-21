#include "Scheduler.hh"

#include <stdio.h>

#include <immintrin.h>

#include <atomic>

#include <clean-core/alloc_array.hh>
#include <clean-core/array.hh>
#include <clean-core/assert.hh>
#include <clean-core/atomic_linked_pool.hh>
#include <clean-core/macros.hh>
#include <clean-core/spin_lock.hh>
#include <clean-core/lock_guard.hh>
#include <clean-core/utility.hh>
#include <clean-core/vector.hh>

#ifdef TD_HAS_RICH_LOG
#include <rich-log/StdOutLogger.hh>
#endif

#include <task-dispatcher/SchedulerConfig.hh>
#include <task-dispatcher/common/system_info.hh>
#include <task-dispatcher/container/ChaseLevQueue.hh>
#include <task-dispatcher/container/FIFOQueue.hh>
#include <task-dispatcher/container/MPMCQueue.hh>
#include <task-dispatcher/container/Task.hh>
#include <task-dispatcher/native/Fiber.hh>
#include <task-dispatcher/native/Threading.hh>
#include <task-dispatcher/native/util.hh>


// new waiting mechanism
// currently broken, WIP
// it is developed with the goal of increasing the maximum waiting fibers on a single counter
#define TD_NEW_WAITING_FIBER_MECHANISM true

// If true, print a warning to stderr if waiting on the work event times out (usually not an error)
#define TD_WARN_ON_WAITING_TIMEOUTS false

// If true, print a warning to stderr if a deadlock is heuristically detected
#define TD_WARN_ON_DEADLOCKS true

// If true, never wait for events and leave worker threads always spinning (minimized latency, cores locked to 100%)
#ifdef TD_NO_WAITS
#define TD_ALWAYS_SPINWAIT true
#else
#define TD_ALWAYS_SPINWAIT false
#endif

// forward declarations
namespace td
{
struct Scheduler;

// checks about td::Task
static_assert(sizeof(td::Task) == td::l1_cacheline_size * TD_FIXED_TASK_SIZE, "task type is unexpectedly large");
static_assert(sizeof(td::Task[8]) == td::l1_cacheline_size * TD_FIXED_TASK_SIZE * 8, "task arrays are unexpectedly large, overaligned?");
static_assert(alignof(td::Task) >= td::l1_cacheline_size, "task type risks misalignment");
static_assert(std::is_trivial_v<td::Task>, "task is not trivial");
}

namespace
{
//
// types

enum class fiber_destination_e : uint8_t
{
    none,
    waiting,
    pool
};

using fiber_index_t = uint32_t;
using thread_index_t = uint32_t;
using counter_index_t = uint16_t; // Must fit into task metadata

enum EInvalidIndices : uint32_t
{
    invalid_fiber = uint32_t(-1),
    invalid_thread = uint32_t(-1),
};
enum EInvalidIndices2 : uint16_t
{
    invalid_counter = uint16_t(-1),
};

enum ESpecialCounterValues : int32_t
{
    ECounterVal_Released = -99
};

using resumable_fiber_mpsc_queue = td::FIFOQueue<fiber_index_t>;

struct WaitingElement
{
    fiber_index_t fiber = invalid_fiber;
    counter_index_t counter = invalid_counter;
};

struct WaitingFiberVector
{
    std::atomic_uint32_t numElements;
    WaitingElement* elements;
};

struct atomic_counter_t
{
    struct waiting_fiber_t
    {
        int counter_target = 0;                              // the counter value that this fiber is waiting for
        fiber_index_t fiber_index = invalid_fiber;           // index of the waiting fiber
        thread_index_t pinned_thread_index = invalid_thread; // index of the thread this fiber is pinned to, invalid_thread if unpinned
        std::atomic_bool in_use{true};                       // whether this slot in the array is currently being processed
    };

    // The value of this counter
    std::atomic<int32_t> count;

#if TD_NEW_WAITING_FIBER_MECHANISM
    cc::spin_lock spinLockWaitingVector;
    WaitingFiberVector waitingVector;
#else
    static constexpr uint32_t max_waiting = 32;
    cc::array<waiting_fiber_t, max_waiting> waiting_fibers = {};
    cc::array<std::atomic_bool, max_waiting> free_waiting_slots = {};
#endif

    // Resets this counter for re-use
    void reset()
    {
        count.store(0, std::memory_order_release);

#if TD_NEW_WAITING_FIBER_MECHANISM
        waitingVector.numElements.store(0);
#else
        for (auto i = 0u; i < max_waiting; ++i)
        {
            free_waiting_slots[i].store(true);
        }
#endif
    }

    friend td::Scheduler;
};

struct worker_thread_t
{
    td::native::thread_t native = {};

    // queue containing fibers that are pinned to this thread are ready to resume
    // same restrictions as for _resumable_fibers apply (worker_fiber_t::is_waiting_cleaned_up)
    resumable_fiber_mpsc_queue pinned_resumable_fibers = {};
    // note that this queue uses a spinlock instead of being lock free (TODO)
    cc::spin_lock pinned_resumable_fibers_lock = {};
};

struct worker_fiber_t
{
    td::native::fiber_t native = {};

    // True if this fiber is currently waiting (called yield_to_fiber with destination waiting)
    // and has been cleaned up by the fiber it yielded to (via clean_up_prev_fiber)
    std::atomic_bool is_waiting_cleaned_up = {false};

#if TD_NEW_WAITING_FIBER_MECHANISM
    // index of the thread this fiber is pinned to, invalid_thread if unpinned
    std::atomic<thread_index_t> pinned_thread_index = {invalid_thread};
#endif
};

struct tls_t
{
    td::native::fiber_t thread_fiber = {}; // thread fiber, not part of scheduler::mFibers

    fiber_index_t current_fiber_index = invalid_fiber;
    fiber_index_t previous_fiber_index = invalid_fiber;

    fiber_destination_e previous_fiber_dest = fiber_destination_e::none;

    thread_index_t thread_index = invalid_thread; // index of this thread in the scheduler::mThreads
    bool is_thread_waiting = false;

    void reset()
    {
        thread_fiber = td::native::fiber_t{};
        current_fiber_index = invalid_fiber;
        previous_fiber_index = invalid_fiber;
        previous_fiber_dest = fiber_destination_e::none;
        thread_index = invalid_thread;
        is_thread_waiting = false;
    }
};

//
// globals

thread_local td::Scheduler* gSchedulerOnThread = nullptr;
td::Scheduler* gScheduler = nullptr;

thread_local tls_t gTLS;
}


namespace td
{
struct Scheduler
{
    /// Launch the scheduler with the given main task
    void start(Task const& main_task);

    std::atomic_bool mIsShuttingDown = {false};
    SchedulerConfig mConfig;

    // Arrays
    cc::alloc_array<worker_thread_t> mThreads;
    cc::alloc_array<worker_fiber_t> mFibers;
    cc::alloc_array<atomic_counter_t> mCounters;

    // Queues
    MPMCQueue<Task> mTasks;
    MPMCQueue<fiber_index_t> mIdleFibers;
    MPMCQueue<fiber_index_t> mResumableFibers;
    MPMCQueue<counter_index_t> mFreeCounters;

    struct AtomicCounterHandleContent
    {
        counter_index_t counterIndex = invalid_counter;
        uint32_t pad = 0; // the type must be 8 byte for the linked pool to work
    };

    cc::atomic_linked_pool<AtomicCounterHandleContent, true> mCounterHandles;

    // Worker wakeup event
    native::event_t mEventWorkAvailable;

    // private:
    fiber_index_t acquireFreeFiber();

    void yieldToFiber(fiber_index_t target_fiber, fiber_destination_e own_destination);
    void cleanUpPrevFiber();

    bool getNextTask(Task& task);
    bool tryResumeFiber(fiber_index_t fiber);

    bool counterAddWaitingFiber(atomic_counter_t& counter, WaitingElement newElement, thread_index_t pinned_thread_index, int counter_target, int& out_counter_val);
    void counterCheckWaitingFibers(atomic_counter_t& counter, int value);

    int32_t counterIncrement(atomic_counter_t& counter, int32_t amount = 1);

    int32_t counterCompareAndSwap(atomic_counter_t& counter, int32_t comparand, int32_t exchange);

    bool enqueueTasks(td::Task* tasks, uint32_t num_tasks, CounterHandle counter);
};
}

namespace
{
struct PrimaryFiberLaunchArgs
{
    td::Scheduler* owning_scheduler;
    td::Task main_task;
};

struct WorkerThreadLaunchArgs
{
    thread_index_t index;
    td::Scheduler* owning_scheduler;
    cc::function_ptr<void(uint32_t, bool, void*)> thread_startstop_func;
    void* thread_startstop_userdata;
};

TD_NATIVE_THREAD_FUNC_DECL entrypointWorkerThread(void* arg_void)
{
    WorkerThreadLaunchArgs* const worker_arg = static_cast<WorkerThreadLaunchArgs*>(arg_void);

    // Register thread local current scheduler variable
    td::Scheduler* const scheduler = worker_arg->owning_scheduler;
    gSchedulerOnThread = scheduler;

    gTLS.reset();
    gTLS.thread_index = worker_arg->index;

    // worker thread startup tasks
#ifdef TD_HAS_RICH_LOG
    // set the rich-log thread name (shown as a prefix)
    rlog::setCurrentThreadName("td#%02u", worker_arg->index);
#endif
    // set the thead name for debuggers
    td::native::set_current_thread_debug_name(int(worker_arg->index));

    // optionally call user provided startup function
    if (worker_arg->thread_startstop_func)
    {
        worker_arg->thread_startstop_func(uint32_t(worker_arg->index), true, worker_arg->thread_startstop_userdata);
    }

    // Set up thread fiber
    td::native::create_main_fiber(gTLS.thread_fiber);

    // ------
    // main work, on main worker fiber
    {
        gTLS.current_fiber_index = scheduler->acquireFreeFiber();
        auto& fiber = scheduler->mFibers[gTLS.current_fiber_index].native;

        td::native::switch_to_fiber(fiber, gTLS.thread_fiber);
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

    td::native::delete_main_fiber(gTLS.thread_fiber);
    td::native::end_current_thread();


    TD_NATIVE_THREAD_FUNC_END;
}

static void entrypointFiber(void* arg_void)
{
    td::Scheduler* const scheduler = static_cast<td::Scheduler*>(arg_void);

#ifdef CC_OS_WINDOWS
    __try
#endif // CC_OS_WINDOWS
    {
        scheduler->cleanUpPrevFiber();

        constexpr uint32_t lc_max_backoff_pauses = 1 << 10;
        constexpr uint32_t lc_min_backoff_pauses = 1;
        uint32_t backoff_num_pauses = lc_min_backoff_pauses;

        td::Task task;
        while (!scheduler->mIsShuttingDown.load(std::memory_order_relaxed))
        {
            if (scheduler->getNextTask(task))
            {
                // work available, reset backoff
                backoff_num_pauses = lc_min_backoff_pauses;

                // Received a task, execute it
                task.runTask();

                // The task returned, decrement the counter
                auto const relevantCounterIdx = task.mMetadata;
                scheduler->counterIncrement(scheduler->mCounters[relevantCounterIdx], -1);
            }
            else
            {
                // No tasks available

#if TD_ALWAYS_SPINWAIT
                // Immediately retry

                // SSE2 pause instruction
                // hints the CPU that this is a spin-wait, improving power usage
                // and post-loop wakeup time (falls back to nop on pre-SSE2)
                // (not at all OS scheduler related, locks cores at 100%)
                _mm_pause();
#else // !TD_ALWAYS_SPINWAIT
      // only perform OS wait if backoff is at maximum and this thread is not waiting
                if (backoff_num_pauses == lc_max_backoff_pauses && !gTLS.is_thread_waiting)
                {
                    // reached max backoff, wait for global event

                    // wait until the global event is signalled, with timeout
                    bool signalled = td::native::wait_for_event(scheduler->mEventWorkAvailable, 10);

#if TD_WARN_ON_WAITING_TIMEOUTS
                    if (!signalled)
                    {
                        fprintf(stderr, "[td] Scheduler warning: Work event wait timed out\n");
                    }
#else  // !TD_WARN_ON_WAITING_TIMEOUTS
                    (void)signalled;
#endif // TD_WARN_ON_WAITING_TIMEOUTS
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
#endif // TD_ALWAYS_SPINWAIT
            }
        }

        // Switch back to thread fiber of the current thread
        td::native::switch_to_fiber(gTLS.thread_fiber, scheduler->mFibers[gTLS.current_fiber_index].native);

        CC_RUNTIME_ASSERT(false && "Reached end of entrypointFiber");
    }
#ifdef CC_OS_WINDOWS
    __except (scheduler->mConfig.fiberSEHFilter(GetExceptionInformation()))
    {
    }
#endif // CC_OS_WINDOWS
}

static void entrypointPrimaryFiber(void* arg_void)
{
    PrimaryFiberLaunchArgs& arg = *static_cast<PrimaryFiberLaunchArgs*>(arg_void);

    // Run main task
    arg.main_task.runTask();

    // Shut down
    arg.owning_scheduler->mIsShuttingDown.store(true, std::memory_order_release);

    // Return to main thread fiber
    td::native::switch_to_fiber(gTLS.thread_fiber, arg.owning_scheduler->mFibers[gTLS.current_fiber_index].native);

    CC_RUNTIME_ASSERT(false && "Reached end of entrypointPrimaryFiber");
}
}

fiber_index_t td::Scheduler::acquireFreeFiber()
{
    fiber_index_t res;
    for (int attempt = 0;; ++attempt)
    {
        if (mIdleFibers.dequeue(res))
            return res;

#if TD_WARN_ON_DEADLOCKS
        if (attempt > 10)
        {
            fprintf(stderr, "[td] Scheduler warning: Failing to find free fiber, possibly deadlocked (attempt %d)\n", attempt);
        }
#endif
    }
}

void td::Scheduler::yieldToFiber(fiber_index_t target_fiber, fiber_destination_e own_destination)
{
    gTLS.previous_fiber_index = gTLS.current_fiber_index;
    gTLS.previous_fiber_dest = own_destination;
    gTLS.current_fiber_index = target_fiber;

    CC_ASSERT(gTLS.previous_fiber_index != invalid_fiber && gTLS.current_fiber_index != invalid_fiber);

    native::switch_to_fiber(mFibers[gTLS.current_fiber_index].native, mFibers[gTLS.previous_fiber_index].native);
    cleanUpPrevFiber();
}

void td::Scheduler::cleanUpPrevFiber()
{
    switch (gTLS.previous_fiber_dest)
    {
    case fiber_destination_e::none:
        return;
    case fiber_destination_e::pool:
        // The fiber is being pooled, add it to the idle fibers
        {
            bool success = mIdleFibers.enqueue(gTLS.previous_fiber_index);
            CC_ASSERT(success);
        }
        break;
    case fiber_destination_e::waiting:
        // The fiber is waiting for a dependency, and can be safely resumed from now on
        // KW_LOG_DIAG("[clean_up_prev_fiber] Waiting fiber " << gTLS.previous_fiber_index << " cleaned up");
        mFibers[gTLS.previous_fiber_index].is_waiting_cleaned_up.store(true, std::memory_order_relaxed);
        break;
    }

    gTLS.previous_fiber_index = invalid_fiber;
    gTLS.previous_fiber_dest = fiber_destination_e::none;
}

bool td::Scheduler::getNextTask(td::Task& task)
{
    // Sleeping fibers with tasks that had their dependencies resolved in the meantime
    // have the highest priority

    // Locally pinned fibers first
    {
        auto& local_thread = mThreads[gTLS.thread_index];
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
                native::signal_event(mEventWorkAvailable);
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
                native::signal_event(mEventWorkAvailable);
            }
            // Fallthrough to global pending Chase-Lev / MPMC
        }
    }

    // Pending tasks
    return mTasks.dequeue(task);
}

bool td::Scheduler::tryResumeFiber(fiber_index_t fiber)
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

bool td::Scheduler::counterAddWaitingFiber(atomic_counter_t& counter, WaitingElement newElement, thread_index_t pinned_thread_index, int counter_target, int& out_counter_val)
{
    CC_ASSERT(counter_target == 0 && "Non-zero waiting targets unimplemented");

#if TD_NEW_WAITING_FIBER_MECHANISM

    auto lg = cc::lock_guard(counter.spinLockWaitingVector);

    // Check if already done
    int const counter_val = counter.count.load(std::memory_order_relaxed);
    out_counter_val = counter_val;

    if (counter_target == counter_val)
    {
        // already done
        return true;
    }

    if (newElement.fiber != invalid_fiber)
    {
        // a new waiting fiber

        thread_index_t const prevPinnedThreadIdx = mFibers[newElement.fiber].pinned_thread_index.exchange(pinned_thread_index);
        CC_ASSERT(prevPinnedThreadIdx == invalid_thread && "unexpected / race");
    }
    else
    {
        // a new waiting counter, nothing special to do
    }

    uint32_t const newElemIdx = counter.waitingVector.numElements.fetch_add(1);
    CC_ASSERT(newElemIdx < mConfig.maxNumWaitingFibersPerCounter && "Too many fibers waiting on a single counter");

    counter.waitingVector.elements[newElemIdx] = newElement;

    return false;

#else

    // WARNING: this is very sensitive code, think 5 times before changing anything
    // consider interleavings of this function and counterCheckWaitingFibers

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
#endif
}

void td::Scheduler::counterCheckWaitingFibers(atomic_counter_t& counter, int value)
{
    CC_ASSERT(value == 0 && "non-zero targets no longer supported");

#if TD_NEW_WAITING_FIBER_MECHANISM

    WaitingElement* pReceivedWaitingElems = nullptr;
    uint32_t numReceivedFibers = 0;

    {
        auto lg = cc::lock_guard(counter.spinLockWaitingVector);

        // clear the previous
        uint32_t const prevVectorSize = counter.waitingVector.numElements.exchange(0);

        numReceivedFibers = prevVectorSize;
        if (numReceivedFibers > 0)
        {
            pReceivedWaitingElems = reinterpret_cast<WaitingElement*>(alloca(sizeof(pReceivedWaitingElems[0]) * prevVectorSize));
            memcpy(pReceivedWaitingElems, counter.waitingVector.elements, numReceivedFibers * sizeof(pReceivedWaitingElems[0]));
        }
    }

    for (auto i = 0u; i < numReceivedFibers; ++i)
    {
        WaitingElement const waitingElem = pReceivedWaitingElems[i];

        if (waitingElem.fiber != invalid_fiber)
        {
            // this element represents a waiting fiber
            auto const fiberIdx = waitingElem.fiber;

            thread_index_t const pinnedThreadIdx = mFibers[fiberIdx].pinned_thread_index.exchange(invalid_thread);

            if (pinnedThreadIdx == invalid_thread)
            {
                // The waiting fiber is not pinned to any thread, store it in the global resumable fibers
                bool success = mResumableFibers.enqueue(fiberIdx);

                // This should never fail, the container is large enough for all fibers in the system
                CC_ASSERT(success);
            }
            else
            {
                // The waiting fiber is pinned to a certain thread, store it there
                auto& pinned_thread = mThreads[pinnedThreadIdx];

                auto lg = cc::lock_guard(pinned_thread.pinned_resumable_fibers_lock);
                pinned_thread.pinned_resumable_fibers.enqueue(fiberIdx);
            }
        }
        else
        {
            // this element represents a waiting counter, caused by createCounterDependency
            CC_ASSERT(waitingElem.counter != invalid_counter && "unexpected entry in waiting elements");

            // decrement the counter
            atomic_counter_t& counter = mCounters[waitingElem.counter];

            // NOTE(JK): counterIncrement can in turn call this function!
            // this might be a problem wrt. alloca, however no deadlocks possible
            counterIncrement(counter, -1);
        }
    }

    native::signal_event(mEventWorkAvailable);

#else

    // WARNING: this is very sensitive code, think 5 times before changing anything
    // consider interleavings of this function and counterAddWaitingFiber

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
            native::signal_event(mEventWorkAvailable);

            // Free the slot
            counter.free_waiting_slots[i].store(true, std::memory_order_release);
        }
    }
#endif
}

int32_t td::Scheduler::counterIncrement(atomic_counter_t& counter, int32_t amount)
{
    CC_ASSERT(amount != 0 && "Must not increment counters by zero");

    int previous = counter.count.fetch_add(amount);
    int const new_val = previous + amount;

    CC_ASSERT(previous != ECounterVal_Released && "Increment on counter that was already released");

    if (new_val == 0)
    {
        // amount must be != 0, meaning this is the only thread with this result
        counterCheckWaitingFibers(counter, 0);
    }

    return new_val;
}

int32_t td::Scheduler::counterCompareAndSwap(atomic_counter_t& counter, int32_t comparand, int32_t exchange)
{
    CC_ASSERT(exchange != ECounterVal_Released && exchange >= 0 && "Must not CAS counters to below zero");

    int32_t expected = comparand;
    bool const success = counter.count.compare_exchange_strong(expected, exchange);

    // 'expected' holds the initial value of the counter
    CC_ASSERT(expected != ECounterVal_Released && "CAS on counter that was already released");

    if (success && exchange == 0 && comparand != 0)
    {
        // CAS succeeded AND zero was written AND the previous value was non-zero
        // this is equivalent to a decrement towards zero
        counterCheckWaitingFibers(counter, 0);
    }

    return expected;
}

void td::Scheduler::start(td::Task const& main_task)
{
#if TD_NEW_WAITING_FIBER_MECHANISM
    for (atomic_counter_t& counter : mCounters)
    {
        counter.waitingVector.numElements.store(0);
        counter.waitingVector.elements = mConfig.staticAlloc->new_array_sized<WaitingElement>(mConfig.maxNumWaitingFibersPerCounter);
    }
#endif

    mIsShuttingDown.store(false, std::memory_order_seq_cst);

    native::create_event(&mEventWorkAvailable);

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

        if (mConfig.pinThreadsToCores)
        {
            // lock main thread to core N
            // (core 0 is conventionally reserved for OS operations and driver interrupts, poor fit for the main thread)
            native::set_current_thread_affinity(mThreads.size() - 1);
        }

        gTLS.reset();
        gSchedulerOnThread = this;

        // Create main fiber on this thread
        native::create_main_fiber(gTLS.thread_fiber);

#ifdef TD_HAS_RICH_LOG
        rlog::setCurrentThreadName("td#00");
#endif
    }

    // Populate fiber pool
    for (fiber_index_t i = 0; i < mFibers.size(); ++i)
    {
        native::create_fiber(mFibers[i].native, entrypointFiber, this, mConfig.fiberStackSizeBytes, mConfig.staticAlloc);
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
        gTLS.thread_index = 0;

        for (thread_index_t i = 1; i < mThreads.size(); ++i)
        {
            worker_thread_t& thread = mThreads[i];

            // TODO: Adjust this stack size
            // On Win 10 1803 and Linux 4.18 this seems to be entirely irrelevant
            auto constexpr thread_stack_overhead_safety = sizeof(void*) * 16;

            // Prepare worker arg
            WorkerThreadLaunchArgs* const worker_arg = new WorkerThreadLaunchArgs();
            worker_arg->index = i;
            worker_arg->owning_scheduler = this;
            worker_arg->thread_startstop_func = mConfig.workerThreadStartStopFunction;
            worker_arg->thread_startstop_userdata = mConfig.workerThreadStartStopUserdata;

            bool success = false;
            if (mConfig.pinThreadsToCores)
            {
                // Create the thread, pinned to core (i - 1), the main thread occupies core N
                uint32_t const pinned_core_index = i - 1;
                success = native::create_thread(mConfig.fiberStackSizeBytes + thread_stack_overhead_safety, entrypointWorkerThread, worker_arg,
                                                pinned_core_index, &thread.native);
            }
            else
            {
                success
                    = native::create_thread(mConfig.fiberStackSizeBytes + thread_stack_overhead_safety, entrypointWorkerThread, worker_arg, &thread.native);
            }
            CC_ASSERT(success && "Failed to create worker thread");
        }
    }

    // Prepare the primary fiber
    {
        // Prepare the args for the primary fiber
        PrimaryFiberLaunchArgs primary_fiber_arg;
        primary_fiber_arg.owning_scheduler = this;
        primary_fiber_arg.main_task = main_task;

        gTLS.current_fiber_index = acquireFreeFiber();
        auto& initial_fiber = mFibers[gTLS.current_fiber_index];

        // reset the fiber, creating the primary fiber
        native::delete_fiber(initial_fiber.native, mConfig.staticAlloc);
        native::create_fiber(initial_fiber.native, entrypointPrimaryFiber, &primary_fiber_arg, mConfig.fiberStackSizeBytes, mConfig.staticAlloc);

        // Launch the primary fiber
        native::switch_to_fiber(initial_fiber.native, gTLS.thread_fiber);
    }

    // The primary fiber has returned, begin shutdown
    {
        // Spin until shutdown has propagated
        while (mIsShuttingDown.load(std::memory_order_seq_cst) != true)
        {
            _mm_pause();
        }

        // wake up all threads
        native::signal_event(mEventWorkAvailable);

        // Delete the main fiber
        native::delete_main_fiber(gTLS.thread_fiber);

        // Join worker threads, starting at 1
        for (auto i = 1u; i < mThreads.size(); ++i)
        {
            // re-signal before joining each thread
            native::signal_event(mEventWorkAvailable);
            native::join_thread(mThreads[i].native);
        }

        // destroy OS fibers
        for (auto& fib : mFibers)
        {
            native::delete_fiber(fib.native, mConfig.staticAlloc);
        }

        // Clear sCurrentScheduler
        gSchedulerOnThread = nullptr;
    }

#if TD_NEW_WAITING_FIBER_MECHANISM
    for (atomic_counter_t& counter : mCounters)
    {
        mConfig.staticAlloc->delete_array_sized(counter.waitingVector.elements, mConfig.maxNumWaitingFibersPerCounter);
    }
#endif

#ifdef CC_OS_WINDOWS
    // undo the changes made to the win32 scheduler
    if (applied_win32_sched_change)
        native::win32_disable_scheduler_granular();
    native::win32_shutdown_utils();
#endif

    native::destroy_event(mEventWorkAvailable);
}

void td::launchScheduler(SchedulerConfig const& configArg, Task const& mainTask)
{
    // init
    CC_ASSERT(gScheduler == nullptr && "More than one scheduler running at once");

    SchedulerConfig config = configArg; // copy
    config.ceilValuesToPowerOfTwo();

    CC_ASSERT(config.isValid() && "Scheduler config invalid, check SchedulerConfig::isValid()");

    if (config.pinThreadsToCores)
    {
        CC_ASSERT(config.numThreads <= getNumLogicalCPUCores() && "More threads than logical CPU cores configured while thread pinning is enabled");
    }

    if (!config.fiberSEHFilter)
    {
        // use an empty SEH handler if none is specified
        config.fiberSEHFilter = [](void*) -> int32_t { return 0; };
    }

    Scheduler* const sched = config.staticAlloc->new_t<td::Scheduler>();
    gScheduler = sched;

    sched->mConfig = config;

    sched->mThreads.reset(config.staticAlloc, config.numThreads);

    for (worker_thread_t& thread : sched->mThreads)
    {
        // allocate enough to never overflow
        thread.pinned_resumable_fibers.initialize(config.numFibers, config.staticAlloc);
    }

    sched->mFibers.reset(config.staticAlloc, config.numFibers);
    sched->mCounters.reset(config.staticAlloc, config.maxNumCounters);

    sched->mTasks.initialize(config.maxNumTasks, config.staticAlloc);
    sched->mIdleFibers.initialize(config.numFibers, config.staticAlloc);
    sched->mResumableFibers.initialize(config.numFibers, config.staticAlloc);
    sched->mFreeCounters.initialize(config.maxNumCounters, config.staticAlloc);

    sched->mCounterHandles.initialize(config.maxNumCounters, config.staticAlloc);

    // launch
    sched->start(mainTask);
    // -- main task returned

    // shutdown
    CC_ASSERT(gScheduler != nullptr && "Uninitialized scheduler");
    CC_ASSERT(gScheduler == sched && "unexpected");
    CC_ASSERT(!isInsideScheduler() && "Must not destroy the scheduler from inside scheduler threads");

    sched->mCounterHandles.destroy(); // might want to do a leak check here

    auto* const staticAlloc = sched->mConfig.staticAlloc;
    staticAlloc->delete_t(sched);
    gScheduler = nullptr;
}

td::CounterHandle td::acquireCounter()
{
    Scheduler* const sched = gSchedulerOnThread;
    CC_ASSERT(sched != nullptr && "Called from outside scheduler, use td::launchScheduler() first");

    counter_index_t free_counter;
    auto success = sched->mFreeCounters.dequeue(free_counter);
    CC_RUNTIME_ASSERT(success && "No free counters available, consider increasing config.maxNumCounters");

    sched->mCounters[free_counter].reset();
    auto const res = sched->mCounterHandles.acquire();
    sched->mCounterHandles.get(res).counterIndex = free_counter;

    return CounterHandle{res};
}

int32_t td::releaseCounter(CounterHandle c)
{
    Scheduler* const sched = gSchedulerOnThread;
    CC_ASSERT(sched != nullptr && "Called from outside scheduler, use td::launchScheduler() first");

    counter_index_t const freed_counter = sched->mCounterHandles.get(c._value).counterIndex;
    int const previousState = sched->mCounters[freed_counter].count.exchange(ECounterVal_Released);
    CC_ASSERT(previousState != ECounterVal_Released && "Raced on td::releaseCounter! Consider releaseCounterIfOnZero");

    sched->mCounterHandles.release(c._value);
    bool success = sched->mFreeCounters.enqueue(freed_counter);
    CC_ASSERT(success && "Unexpected error, free counter MPMC queue should always have sufficient capacity");

    return previousState;
}

bool td::releaseCounterIfOnZero(CounterHandle c)
{
    Scheduler* const sched = gSchedulerOnThread;
    CC_ASSERT(sched != nullptr && "Called from outside scheduler, use td::launchScheduler() first");

    counter_index_t const freed_counter = sched->mCounterHandles.get(c._value).counterIndex;

    int contained = 0;
    if (!sched->mCounters[freed_counter].count.compare_exchange_strong(contained, ECounterVal_Released))
    {
        // current value must be either ECounterVal_Released, meaning someone else won the race on this function,
        // or > 0 because the counter didn't reach zero yet
        CC_ASSERT(contained > 0 || contained == ECounterVal_Released && "Counter contains negative value");
        return false;
    }

    sched->mCounterHandles.release(c._value);
    bool success = sched->mFreeCounters.enqueue(freed_counter);
    CC_ASSERT(success && "Unexpected error, free counter MPMC queue should always have sufficient capacity");

    return true;
}

void td::submitTasks(CounterHandle c, cc::span<Task> tasks)
{
    Scheduler* const sched = gSchedulerOnThread;
    CC_ASSERT(sched != nullptr && "Called from outside scheduler, use td::launchScheduler() first");
    CC_ASSERT(c.isValid() && "td::sumitTasks() called with invalid counter handle");

    size_t const numTasks = tasks.size();

    counter_index_t const counter_index = sched->mCounterHandles.get(c._value).counterIndex;
    sched->counterIncrement(sched->mCounters[counter_index], int(numTasks));

    // TODO: Multi-enqueue
    bool success = true;
    for (auto i = 0u; i < numTasks; ++i)
    {
        tasks[i].mMetadata = counter_index;

        success &= sched->mTasks.enqueue(tasks[i]);
    }

    // signal the global event
    native::signal_event(sched->mEventWorkAvailable);

    CC_RUNTIME_ASSERT(success && "Task queue is full, consider increasing config.maxNumTasks");
}

int32_t td::waitForCounter(CounterHandle c, bool pinned)
{
    Scheduler* const sched = gSchedulerOnThread;
    CC_ASSERT(sched != nullptr && "Called from outside scheduler, use td::launchScheduler() first");
    CC_ASSERT(sched->mCounterHandles.is_alive(c._value) && "waited on expired counter handle");

    auto const counter_index = sched->mCounterHandles.get(c._value).counterIndex;

    // The current fiber is now waiting, but not yet cleaned up
    sched->mFibers[gTLS.current_fiber_index].is_waiting_cleaned_up.store(false, std::memory_order_release);

    gTLS.is_thread_waiting = true;

    int counter_value_before_wait = -1;

    WaitingElement waitElem = {};
    waitElem.fiber = gTLS.current_fiber_index;

    if (sched->counterAddWaitingFiber(sched->mCounters[counter_index], waitElem, pinned ? gTLS.thread_index : invalid_thread, 0, counter_value_before_wait))
    {
        // Already done
        //        KW_LOG_DIAG("[wait] Wait for counter " << int(counter_index) << " is over early, resuming immediately");
    }
    else
    {
        // Not already done, prepare to yield
        //        KW_LOG_DIAG("[wait] Waiting for counter " << int(counter_index) << ", yielding");
        sched->yieldToFiber(sched->acquireFreeFiber(), fiber_destination_e::waiting);
    }

    gTLS.is_thread_waiting = false;

    // Either the counter was already on target, or this fiber has been awakened because it is now on target,
    // return execution

    return counter_value_before_wait;
}

int32_t td::incrementCounter(CounterHandle c, uint32_t amount)
{
    Scheduler* const sched = gSchedulerOnThread;
    CC_ASSERT(sched != nullptr && "Called from outside scheduler, use td::launchScheduler() first");
    CC_ASSERT(c.isValid() && "td::incrementCounter() called with invalid counter handle");

    auto const counter_index = sched->mCounterHandles.get(c._value).counterIndex;
    return sched->counterIncrement(sched->mCounters[counter_index], (int32_t)amount);
}

int32_t td::decrementCounter(CounterHandle c, uint32_t amount)
{
    Scheduler* const sched = gSchedulerOnThread;
    CC_ASSERT(sched != nullptr && "Called from outside scheduler, use td::launchScheduler() first");
    CC_ASSERT(c.isValid() && "td::decrementCounter() called with invalid counter handle");

    auto const counter_index = sched->mCounterHandles.get(c._value).counterIndex;
    return sched->counterIncrement(sched->mCounters[counter_index], (int32_t)amount * -1);
}

int32_t td::compareAndSwapCounter(CounterHandle c, int32_t comparand, int32_t exchange)
{
    Scheduler* const sched = gSchedulerOnThread;
    CC_ASSERT(sched != nullptr && "Called from outside scheduler, use td::launchScheduler() first");
    CC_ASSERT(c.isValid() && "td::compareAndSwapCounter() called with invalid counter handle");

    auto const counter_index = sched->mCounterHandles.get(c._value).counterIndex;
    return sched->counterCompareAndSwap(sched->mCounters[counter_index], comparand, exchange);
}

int32_t td::createCounterDependency(CounterHandle counterToModify, CounterHandle counterToDependUpon)
{
    Scheduler* const sched = gSchedulerOnThread;
    CC_ASSERT(sched != nullptr && "Called from outside scheduler, use td::launchScheduler() first");
    CC_ASSERT(counterToModify.isValid() && counterToDependUpon.isValid() && "td::createCounterDependency() called with invalid counter handle");


    // immediately increment counterToModify by one
    auto const counterToModifyIdx = sched->mCounterHandles.get(counterToModify._value).counterIndex;
    sched->counterIncrement(sched->mCounters[counterToModifyIdx], 1);

    // enter counterToModify as a waiting element into counterToDependUpon
    auto const counterToDependUponIdx = sched->mCounterHandles.get(counterToDependUpon._value).counterIndex;
    int counterToDependUponValueBeforeWait = -1;

    WaitingElement waitElem = {};
    waitElem.counter = counterToModifyIdx;

    if (sched->counterAddWaitingFiber(sched->mCounters[counterToDependUponIdx], waitElem, invalid_thread, 0, counterToDependUponValueBeforeWait))
    {
        // immediately done, decrement counterToModify again
        sched->counterIncrement(sched->mCounters[counterToModifyIdx], -1);
    }

    return counterToDependUponValueBeforeWait;
}

int32_t td::getApproximateCounterValue(CounterHandle c)
{
    Scheduler* const sched = gSchedulerOnThread;
    CC_ASSERT(sched != nullptr && "Called from outside scheduler, use td::launchScheduler() first");
    CC_ASSERT(c.isValid() && "td::getApproximateCounterValue() called with invalid counter handle");

    auto const counter_index = sched->mCounterHandles.get(c._value).counterIndex;
    return sched->mCounters[counter_index].count.load(std::memory_order_relaxed);
}

uint32_t td::getNumThreadsInScheduler()
{
    Scheduler* const sched = gSchedulerOnThread;
    CC_ASSERT(sched != nullptr && "Called from outside scheduler, use td::launchScheduler() first");

    return (uint32_t)sched->mThreads.size();
}

bool td::isInsideScheduler() { return gSchedulerOnThread != nullptr; }

uint32_t td::getCurrentThreadIndex() { return gTLS.thread_index; }

uint32_t td::getCurrentFiberIndex() { return gTLS.current_fiber_index; }
