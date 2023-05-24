#include "Scheduler.hh"

#include <stdio.h>

#include <immintrin.h>

#include <atomic>

#include <clean-core/alloc_array.hh>
#include <clean-core/array.hh>
#include <clean-core/assert.hh>
#include <clean-core/atomic_linked_pool.hh>
#include <clean-core/lock_guard.hh>
#include <clean-core/macros.hh>
#include <clean-core/spin_lock.hh>
#include <clean-core/utility.hh>
#include <clean-core/vector.hh>

#ifdef CC_OS_WINDOWS
#include <clean-core/native/win32_sanitized.hh>
#endif

#ifdef TD_HAS_RICH_LOG
#include <rich-log/logger.hh>
#endif

#include <task-dispatcher/SchedulerConfig.hh>
#include <task-dispatcher/common/system_info.hh>
#include <task-dispatcher/container/ChaseLevQueue.hh>
#include <task-dispatcher/container/FIFOQueue.hh>
#include <task-dispatcher/container/Task.hh>
#include <task-dispatcher/native/Fiber.hh>
#include <task-dispatcher/native/Threading.hh>
#include <task-dispatcher/native/util.hh>

#define TD_USE_MOODYCAMEL true

#if TD_USE_MOODYCAMEL
#include <task-dispatcher/container/ConcurrentQueue.hh>
#else
#include <clean-core/experimental/mpmc_queue.hh>
#endif

// Warn if waiting on the work event times out (usually not an error)
#define TD_WARN_ON_WAITING_TIMEOUTS false

// Warn if a deadlock is likely
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
static_assert(sizeof(Task) == NumBytesFixedTask, "task type is unexpectedly large");
static_assert(sizeof(Task[8]) == NumBytesFixedTask * 8, "task arrays are unexpectedly large, overaligned?");
static_assert(alignof(Task) >= NumBytesL1Cacheline, "task type risks misalignment");
static_assert(std::is_trivial_v<Task>, "task is not trivial");

namespace
{
//
// types

enum class EFiberDestination : uint8_t
{
    None = 0,
    Waiting,
    Pool
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

struct CounterNode
{
    // The value of this counter
    std::atomic<int32_t> count;

    cc::spin_lock spinLockWaitingVector;
    WaitingFiberVector waitingVector;

    // Resets this counter for re-use
    void reset()
    {
        count.store(0, std::memory_order_release);

        waitingVector.numElements.store(0);
    }

    friend td::Scheduler;
};

struct WorkerThreadNode
{
    native::thread_t native = {};

    // queue containing fibers that are pinned to this thread are ready to resume
    // same restrictions as for _resumable_fibers apply (WorkerFiberNode::bIsWaitingCleanedUp)
    FIFOQueue<fiber_index_t> pinnedResumableFibers = {};

    // note that this queue uses a spinlock instead of being lock free (TODO)
    cc::spin_lock pinnedResumableFibersLock = {};
};

struct WorkerFiberNode
{
    native::fiber_t native = {};

    // True if this fiber is currently waiting (called yield_to_fiber with destination waiting)
    // and has been cleaned up by the fiber it yielded to (via clean_up_prev_fiber)
    std::atomic_bool bIsWaitingCleanedUp = {false};

    // index of the thread this fiber is pinned to, invalid_thread if unpinned
    std::atomic<thread_index_t> pinnedThreadIndex = {invalid_thread};

    std::atomic<ETaskPriority> currentTaskPriority = {ETaskPriority::NUM_PRIORITIES};
};

struct ThreadLocalGlobals
{
    native::fiber_t threadFiber = {}; // thread fiber, not part of scheduler::mFibers

    fiber_index_t currentFiberIdx = invalid_fiber;
    fiber_index_t previousFiberIdx = invalid_fiber;

    EFiberDestination previousFiberDest = EFiberDestination::None;

    thread_index_t threadIdx = invalid_thread; // index of this thread in the scheduler::mThreads
    bool bIsThreadWaiting = false;

    void reset()
    {
        threadFiber = native::fiber_t{};
        currentFiberIdx = invalid_fiber;
        previousFiberIdx = invalid_fiber;
        previousFiberDest = EFiberDestination::None;
        threadIdx = invalid_thread;
        bIsThreadWaiting = false;
    }
};

//
// globals

thread_local Scheduler* gSchedulerOnThread = nullptr;
Scheduler* gScheduler = nullptr;

thread_local ThreadLocalGlobals gTLS;
} // namespace

#if TD_USE_MOODYCAMEL
template <class T>
using UsedMPMCQueue = td::ConcurrentQueue<T>;
#else
template <class T>
using UsedMPMCQueue = cc::mpmc_queue<T>;
#endif

template <class T>
struct PrioritizedMPMC
{
    UsedMPMCQueue<T> queues[(uint32_t)ETaskPriority::NUM_PRIORITIES];

    bool Dequeue(T* pOutType, ETaskPriority* pOutPriority)
    {
        // go from 0 (highest) to slower prios
        for (uint32_t i = 0; i < (uint32_t)ETaskPriority::NUM_PRIORITIES; ++i)
        {
            if (queues[i].try_dequeue(*pOutType))
            {
                *pOutPriority = ETaskPriority(i);
                return true;
            }
        }

        return false;
    }

    void Enqueue(T const& type, ETaskPriority prio)
    {
        CC_ASSERT((uint32_t)prio < (uint32_t)ETaskPriority::NUM_PRIORITIES);
        bool bSuccess = queues[(uint32_t)prio].enqueue(type);
        CC_ASSERT(bSuccess);
    }

    void EnqueueBatch(cc::span<T const> spValues, ETaskPriority prio)
    {
        CC_ASSERT((uint32_t)prio < (uint32_t)ETaskPriority::NUM_PRIORITIES);
        bool bSuccess = queues[(uint32_t)prio].enqueue_bulk(spValues.data(), spValues.size());
        CC_ASSERT(bSuccess);
    }
};

struct Scheduler
{
    /// Launch the scheduler with the given main task
    void start(Task const& mainTask);

    std::atomic_bool mIsShuttingDown = {false};
    SchedulerConfig mConfig;

    // Arrays
    cc::alloc_array<WorkerThreadNode> mThreads;
    cc::alloc_array<WorkerFiberNode> mFibers;
    cc::alloc_array<CounterNode> mCounters;


    // Queues
    PrioritizedMPMC<Task> mTasks;
    UsedMPMCQueue<fiber_index_t> mIdleFibers;
    PrioritizedMPMC<fiber_index_t> mResumableFibers;
    UsedMPMCQueue<counter_index_t> mFreeCounters;

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

    void yieldToFiber(fiber_index_t target_fiber, EFiberDestination own_destination);
    void cleanUpPrevFiber();

    bool getNextTask(Task& task, ETaskPriority& outPrio);
    bool tryResumeFiber(fiber_index_t fiber);

    bool counterAddWaitingFiber(CounterNode& counter, WaitingElement newElement, thread_index_t pinnedThreadIndex, int* pOutCounterVal);
    void counterCheckWaitingFibers(CounterNode& counter, int value);

    int32_t counterIncrement(CounterNode& counter, int32_t amount = 1);

    int32_t counterCompareAndSwap(CounterNode& counter, int32_t comparand, int32_t exchange);
};

namespace
{
struct PrimaryFiberLaunchArgs
{
    Scheduler* pOwningScheduler;
    Task mainTask;
};

struct WorkerThreadLaunchArgs
{
    thread_index_t index;
    Scheduler* pOwningScheduler;
    cc::function_ptr<void(uint32_t, bool, void*)> pThreadStartstopFunc;
    void* pThreadStartstopFunc_Userdata;
};

TD_NATIVE_THREAD_FUNC_DECL entrypointWorkerThread(void* pArgVoid)
{
    WorkerThreadLaunchArgs* const pWorkerArg = static_cast<WorkerThreadLaunchArgs*>(pArgVoid);

    // Register thread local current scheduler variable
    Scheduler* const scheduler = pWorkerArg->pOwningScheduler;
    gSchedulerOnThread = scheduler;

    gTLS.reset();
    gTLS.threadIdx = pWorkerArg->index;

    // worker thread startup tasks
    {
#ifdef TD_HAS_RICH_LOG
        // set the rich-log thread name (shown as a prefix)
        rlog::set_current_thread_name("td#%02u", pWorkerArg->index);
#endif

        // set the thead name for debuggers
        char buf[256];
        std::snprintf(buf, sizeof(buf), "td worker thread #%02d", int(pWorkerArg->index));
        td::native::setCurrentThreadDebugName(buf);

        // optionally call user provided startup function
        if (pWorkerArg->pThreadStartstopFunc)
        {
            pWorkerArg->pThreadStartstopFunc(uint32_t(pWorkerArg->index), true, pWorkerArg->pThreadStartstopFunc_Userdata);
        }
    }

    // Set up thread fiber
    td::native::createMainFiber(&gTLS.threadFiber);

    // ------
    // main work, on main worker fiber
    {
        gTLS.currentFiberIdx = scheduler->acquireFreeFiber();
        auto& fiber = scheduler->mFibers[gTLS.currentFiberIdx].native;

        td::native::switchToFiber(fiber, gTLS.threadFiber);
    }
    // returned, shutdown worker thread
    // ------

    // optionally call user provided shutdown function
    if (pWorkerArg->pThreadStartstopFunc)
    {
        pWorkerArg->pThreadStartstopFunc(uint32_t(pWorkerArg->index), false, pWorkerArg->pThreadStartstopFunc_Userdata);
    }

    // Clean up allocated argument
    delete pWorkerArg;

    td::native::deleteMainFiber(gTLS.threadFiber);
    td::native::endCurrentThread();


    TD_NATIVE_THREAD_FUNC_END;
}

static void entrypointFiber(void* pArgVoid)
{
    Scheduler* const scheduler = static_cast<Scheduler*>(pArgVoid);

#ifdef CC_OS_WINDOWS
    __try
#endif // CC_OS_WINDOWS
    {
        scheduler->cleanUpPrevFiber();

        constexpr uint32_t lc_max_backoff_pauses = 1 << 10;
        constexpr uint32_t lc_min_backoff_pauses = 1;
        uint32_t backoff_num_pauses = lc_min_backoff_pauses;

        Task task;
        while (!scheduler->mIsShuttingDown.load(std::memory_order_relaxed))
        {
            ETaskPriority retrievedPrio;
            if (scheduler->getNextTask(task, retrievedPrio))
            {
                // work available, reset backoff
                backoff_num_pauses = lc_min_backoff_pauses;

                // store current task priority
                scheduler->mFibers[gTLS.currentFiberIdx].currentTaskPriority.store(retrievedPrio);

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
                if (backoff_num_pauses == lc_max_backoff_pauses && !gTLS.bIsThreadWaiting)
                {
                    // reached max backoff, wait for global event

                    // wait until the global event is signalled, with timeout
                    bool signalled = td::native::waitForEvent(scheduler->mEventWorkAvailable, 10);

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
        td::native::switchToFiber(gTLS.threadFiber, scheduler->mFibers[gTLS.currentFiberIdx].native);

        CC_RUNTIME_ASSERT(false && "Reached end of entrypointFiber");
    }
#ifdef CC_OS_WINDOWS
    __except (scheduler->mConfig.fiberSEHFilter(GetExceptionInformation()))
    {
    }
#endif // CC_OS_WINDOWS
}

static void entrypointPrimaryFiber(void* pArgVoid)
{
    PrimaryFiberLaunchArgs& arg = *static_cast<PrimaryFiberLaunchArgs*>(pArgVoid);

    // Run main task
    arg.mainTask.runTask();

    // Shut down
    arg.pOwningScheduler->mIsShuttingDown.store(true, std::memory_order_release);

    // Return to main thread fiber
    td::native::switchToFiber(gTLS.threadFiber, arg.pOwningScheduler->mFibers[gTLS.currentFiberIdx].native);

    CC_RUNTIME_ASSERT(false && "Reached end of entrypointPrimaryFiber");
}
}
} // namespace td

td::fiber_index_t td::Scheduler::acquireFreeFiber()
{
    fiber_index_t res;
    for (int attempt = 0;; ++attempt)
    {
        if (mIdleFibers.try_dequeue(res))
            return res;

#if TD_WARN_ON_DEADLOCKS
        if (attempt > 10)
        {
            fprintf(stderr, "[td] Scheduler warning: Failing to find free fiber, possibly deadlocked (attempt %d)\n", attempt);
        }
#endif
    }
}

void td::Scheduler::yieldToFiber(fiber_index_t targetFiberIdx, EFiberDestination ownFiberDestination)
{
    gTLS.previousFiberIdx = gTLS.currentFiberIdx;
    gTLS.previousFiberDest = ownFiberDestination;
    gTLS.currentFiberIdx = targetFiberIdx;

    CC_ASSERT(gTLS.previousFiberIdx != invalid_fiber && gTLS.currentFiberIdx != invalid_fiber);

    native::switchToFiber(mFibers[gTLS.currentFiberIdx].native, mFibers[gTLS.previousFiberIdx].native);
    cleanUpPrevFiber();
}

void td::Scheduler::cleanUpPrevFiber()
{
    switch (gTLS.previousFiberDest)
    {
    case EFiberDestination::None:
        break;
    case EFiberDestination::Pool:
        // The fiber is being pooled, add it to the idle fibers
        {
            bool bSuccess = mIdleFibers.enqueue(gTLS.previousFiberIdx);
            CC_ASSERT(bSuccess);
        }
        break;
    case EFiberDestination::Waiting:
        // The fiber is waiting for a dependency, and can be safely resumed from now on
        // KW_LOG_DIAG("[clean_up_prev_fiber] Waiting fiber " << gTLS.previousFiberIdx << " cleaned up");
        mFibers[gTLS.previousFiberIdx].bIsWaitingCleanedUp.store(true, std::memory_order_relaxed);
        break;
    }

    gTLS.previousFiberIdx = invalid_fiber;
    gTLS.previousFiberDest = EFiberDestination::None;
}

bool td::Scheduler::getNextTask(td::Task& task, ETaskPriority& outPrio)
{
    // Locally pinned, resumable fibers first
    while (true)
    {
        auto& localThread = mThreads[gTLS.threadIdx];
        fiber_index_t resumableFiberIdx;
        bool bGotResumable;
        {
            auto lg = cc::lock_guard(localThread.pinnedResumableFibersLock);
            bGotResumable = localThread.pinnedResumableFibers.dequeue(resumableFiberIdx);
        }

        // got no local pinned fibers, fall through to global resumable fibers
        if (!bGotResumable)
            break;

        if (!tryResumeFiber(resumableFiberIdx))
        {
            // Received fiber is not cleaned up yet, re-enqueue (very rare)
            {
                auto lg = cc::lock_guard(localThread.pinnedResumableFibersLock);
                localThread.pinnedResumableFibers.enqueue(resumableFiberIdx);
            }

            // signal the global event
            native::signalEvent(mEventWorkAvailable);
            break;
        }

        // Successfully resumed (and returned)
        // stay in while-loop to retry for local pinned resumables
    }

    // Global resumable fibers
    while (true)
    {
        fiber_index_t resumableFiberIdx;
        ETaskPriority fiberPriority;
        if (!mResumableFibers.Dequeue(&resumableFiberIdx, &fiberPriority))
        {
            // got no  global resumable fibers, fall through to global pending tasks
            break;
        }

        // in case other things were done with this fiber, re-set the priority
        mFibers[resumableFiberIdx].currentTaskPriority.store(fiberPriority);

        if (!tryResumeFiber(resumableFiberIdx))
        {
            // Received fiber is not cleaned up yet, re-enqueue (very rare)
            // This should only happen if mResumableFibers is almost empty, and
            // the latency impact is low in those cases
            mResumableFibers.Enqueue(resumableFiberIdx, fiberPriority);

            // signal the global event
            native::signalEvent(mEventWorkAvailable);
            break;
        }

        // Successfully resumed (and returned)
        // stay in while-loop to retry for global resumables
    }

    // New pending tasks (prioritized)
    return mTasks.Dequeue(&task, &outPrio);
}

bool td::Scheduler::tryResumeFiber(fiber_index_t fiber)
{
    bool bExpected = true;
    bool const bCASSuccess = std::atomic_compare_exchange_strong_explicit(&mFibers[fiber].bIsWaitingCleanedUp, &bExpected, false, //
                                                                          std::memory_order_seq_cst, std::memory_order_relaxed);
    if CC_CONDITION_LIKELY (bCASSuccess)
    {
        // bIsWaitingCleanedUp was true, and is now exchanged to false
        // The resumable fiber is properly cleaned up and can be switched to

        // KW_LOG_DIAG("[get_next_task] Acquired resumable fiber " << int(fiber) << " which is cleaned up, yielding");
        yieldToFiber(fiber, EFiberDestination::Pool);

        // returned, resume was successful
        return true;
    }
    else
    {
        // The resumable fiber is not yet cleaned up
        return false;
    }
}

bool td::Scheduler::counterAddWaitingFiber(CounterNode& counter, WaitingElement newElement, thread_index_t pinnedThreadIdx, int* pOutCounterVal)
{
    auto lg = cc::lock_guard(counter.spinLockWaitingVector);

    // Check if already done
    int const counterVal = counter.count.load(std::memory_order_relaxed);
    *pOutCounterVal = counterVal;

    if (counterVal == 0)
    {
        // already done
        return true;
    }

    if (newElement.fiber != invalid_fiber)
    {
        // a new waiting fiber

        thread_index_t const prevPinnedThreadIdx = mFibers[newElement.fiber].pinnedThreadIndex.exchange(pinnedThreadIdx);
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
}

void td::Scheduler::counterCheckWaitingFibers(CounterNode& counter, int value)
{
    CC_ASSERT(value == 0 && "non-zero targets no longer supported");

    WaitingElement receivedWaitingElems[128];
    uint32_t numReceivedFibers = 0;

    {
        auto lg = cc::lock_guard(counter.spinLockWaitingVector);

        // clear the previous
        numReceivedFibers = counter.waitingVector.numElements.exchange(0);
        CC_RUNTIME_ASSERT(numReceivedFibers <= CC_COUNTOF(receivedWaitingElems) && "Too many waiting fibers on a single counter");

        if (numReceivedFibers > 0)
        {
            memcpy(receivedWaitingElems, counter.waitingVector.elements, numReceivedFibers * sizeof(receivedWaitingElems[0]));
        }
    }

    for (auto i = 0u; i < numReceivedFibers; ++i)
    {
        WaitingElement const waitingElem = receivedWaitingElems[i];

        if (waitingElem.fiber != invalid_fiber)
        {
            // this element represents a waiting fiber
            auto const fiberIdx = waitingElem.fiber;
            WorkerFiberNode& fiberNode = mFibers[fiberIdx];

            ETaskPriority const currentTaskPrio = fiberNode.currentTaskPriority.load();
            thread_index_t const pinnedThreadIdx = fiberNode.pinnedThreadIndex.exchange(invalid_thread);

            if (pinnedThreadIdx == invalid_thread)
            {
                // The waiting fiber is not pinned to any thread, store it in the global resumable fibers
                mResumableFibers.Enqueue(fiberIdx, currentTaskPrio);
            }
            else
            {
                // The waiting fiber is pinned to a certain thread, store it there
                auto& pinned_thread = mThreads[pinnedThreadIdx];

                auto lg = cc::lock_guard(pinned_thread.pinnedResumableFibersLock);
                pinned_thread.pinnedResumableFibers.enqueue(fiberIdx);
            }
        }
        else
        {
            // this element represents a waiting counter, caused by createCounterDependency
            CC_ASSERT(waitingElem.counter != invalid_counter && "unexpected entry in waiting elements");

            // decrement the counter
            CounterNode& counter = mCounters[waitingElem.counter];

            // NOTE(JK): counterIncrement can in turn call this function!
            counterIncrement(counter, -1);
        }
    }

    native::signalEvent(mEventWorkAvailable);
}

int32_t td::Scheduler::counterIncrement(CounterNode& counter, int32_t amount)
{
    CC_ASSERT(amount != 0 && "Must not increment counters by zero");

    int const valuePrev = counter.count.fetch_add(amount);
    int const valueCurr = valuePrev + amount;

    CC_ASSERT(valuePrev != ECounterVal_Released && "Increment on counter that was already released");

    if (valueCurr == 0)
    {
        // amount must be != 0, meaning this is the only thread with this result
        counterCheckWaitingFibers(counter, 0);
    }

    return valueCurr;
}

int32_t td::Scheduler::counterCompareAndSwap(CounterNode& counter, int32_t comparand, int32_t exchange)
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

void td::Scheduler::start(td::Task const& mainTask)
{
    for (CounterNode& counter : mCounters)
    {
        counter.waitingVector.numElements.store(0);
        counter.waitingVector.elements = mConfig.staticAlloc->new_array_sized<WaitingElement>(mConfig.maxNumWaitingFibersPerCounter);
    }

    mIsShuttingDown.store(false, std::memory_order_seq_cst);

    native::createEvent(&mEventWorkAvailable);

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
        main_thread.native = native::getCurrentThread();

        if (mConfig.pinThreadsToCores)
        {
            // lock main thread to core N
            // (core 0 is conventionally reserved for OS operations and driver interrupts, poor fit for the main thread)
            native::setCurrentThreadCoreAffinity(mThreads.size() - 1);
        }

        gTLS.reset();
        gSchedulerOnThread = this;

        // Create main fiber on this thread
        native::createMainFiber(&gTLS.threadFiber);

#ifdef TD_HAS_RICH_LOG
        rlog::set_current_thread_name("td#00");
#endif
    }

    // Populate fiber pool
    for (fiber_index_t i = 0; i < mFibers.size(); ++i)
    {
        native::createFiber(&mFibers[i].native, entrypointFiber, this, mConfig.fiberStackSizeBytes, mConfig.staticAlloc);
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
        gTLS.threadIdx = 0;

        for (thread_index_t i = 1; i < mThreads.size(); ++i)
        {
            WorkerThreadNode& thread = mThreads[i];

            // TODO: Adjust this stack size
            // On Win 10 1803 and Linux 4.18 this seems to be entirely irrelevant
            auto constexpr thread_stack_overhead_safety = sizeof(void*) * 16;

            // Prepare worker arg
            WorkerThreadLaunchArgs* const worker_arg = new WorkerThreadLaunchArgs();
            worker_arg->index = i;
            worker_arg->pOwningScheduler = this;
            worker_arg->pThreadStartstopFunc = mConfig.workerThreadStartStopFunction;
            worker_arg->pThreadStartstopFunc_Userdata = mConfig.workerThreadStartStopUserdata;

            bool success = false;
            if (mConfig.pinThreadsToCores)
            {
                // Create the thread, pinned to core (i - 1), the main thread occupies core N
                uint32_t const pinned_core_index = i - 1;
                success = native::createThread(mConfig.fiberStackSizeBytes + thread_stack_overhead_safety, entrypointWorkerThread, worker_arg,
                                               pinned_core_index, &thread.native);
            }
            else
            {
                success = native::createThread(mConfig.fiberStackSizeBytes + thread_stack_overhead_safety, entrypointWorkerThread, worker_arg, &thread.native);
            }
            CC_ASSERT(success && "Failed to create worker thread");
        }
    }

    // Prepare the primary fiber
    {
        // Prepare the args for the primary fiber
        PrimaryFiberLaunchArgs primary_fiber_arg;
        primary_fiber_arg.pOwningScheduler = this;
        primary_fiber_arg.mainTask = mainTask;

        gTLS.currentFiberIdx = acquireFreeFiber();
        auto& initial_fiber = mFibers[gTLS.currentFiberIdx];

        // reset the fiber, creating the primary fiber
        native::deleteFiber(initial_fiber.native, mConfig.staticAlloc);
        native::createFiber(&initial_fiber.native, entrypointPrimaryFiber, &primary_fiber_arg, mConfig.fiberStackSizeBytes, mConfig.staticAlloc);

        // Launch the primary fiber
        native::switchToFiber(initial_fiber.native, gTLS.threadFiber);
    }

    // The primary fiber has returned, begin shutdown
    {
        // Spin until shutdown has propagated
        while (mIsShuttingDown.load(std::memory_order_seq_cst) != true)
        {
            _mm_pause();
        }

        // wake up all threads
        native::signalEvent(mEventWorkAvailable);

        // Delete the main fiber
        native::deleteMainFiber(gTLS.threadFiber);

        // Join worker threads, starting at 1
        for (auto i = 1u; i < mThreads.size(); ++i)
        {
            // re-signal before joining each thread
            native::signalEvent(mEventWorkAvailable);

            native::joinThread(mThreads[i].native);
        }

        // destroy OS fibers
        for (auto& fib : mFibers)
        {
            native::deleteFiber(fib.native, mConfig.staticAlloc);
        }

        // Clear sCurrentScheduler
        gSchedulerOnThread = nullptr;
    }

    for (CounterNode& counter : mCounters)
    {
        mConfig.staticAlloc->delete_array_sized(counter.waitingVector.elements, mConfig.maxNumWaitingFibersPerCounter);
    }

#ifdef CC_OS_WINDOWS
    // undo the changes made to the win32 scheduler
    if (applied_win32_sched_change)
        native::win32_disable_scheduler_granular();
    native::win32_shutdown_utils();
#endif

    native::destroyEvent(mEventWorkAvailable);
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

    for (WorkerThreadNode& thread : sched->mThreads)
    {
        // allocate enough to never overflow
        thread.pinnedResumableFibers.initialize(config.numFibers, config.staticAlloc);
    }

    sched->mFibers.reset(config.staticAlloc, config.numFibers);
    sched->mCounters.reset(config.staticAlloc, config.maxNumCounters);

#if !TD_USE_MOODYCAMEL
    sched->mTasks.initialize(config.maxNumTasks, config.staticAlloc);
    sched->mIdleFibers.initialize(config.numFibers, config.staticAlloc);
    sched->mResumableFibers.initialize(config.numFibers, config.staticAlloc);
    sched->mFreeCounters.initialize(config.maxNumCounters, config.staticAlloc);
#endif

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
    auto success = sched->mFreeCounters.try_dequeue(free_counter);
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

bool td::releaseCounterIfOnZero(CounterHandle hCounter)
{
    Scheduler* const sched = gSchedulerOnThread;
    CC_ASSERT(sched != nullptr && "Called from outside scheduler, use td::launchScheduler() first");

    counter_index_t const releasedCounterIdx = sched->mCounterHandles.get(hCounter._value).counterIndex;

    int releasedCounterVal = 0;
    if (!sched->mCounters[releasedCounterIdx].count.compare_exchange_strong(releasedCounterVal, ECounterVal_Released))
    {
        // current value must be either ECounterVal_Released, meaning someone else won the race on this function,
        // or > 0 because the counter didn't reach zero yet
        CC_ASSERT(releasedCounterVal > 0 || releasedCounterVal == ECounterVal_Released && "Counter contains negative value");
        return false;
    }

    sched->mCounterHandles.release(hCounter._value);
    bool const bSuccess = sched->mFreeCounters.enqueue(releasedCounterIdx);
    CC_ASSERT(bSuccess && "Unexpected error, free counter MPMC queue should always have sufficient capacity");

    return true;
}

void td::submitTasks(CounterHandle hCounter, cc::span<Task> tasks, ETaskPriority priority)
{
    if (tasks.empty())
        return;

    Scheduler* const sched = gSchedulerOnThread;
    CC_ASSERT(sched != nullptr && "Called from outside scheduler, use td::launchScheduler() first");
    CC_ASSERT(hCounter.isValid() && "td::submitTasks() called with invalid counter handle");

    size_t const numTasks = tasks.size();

    counter_index_t const counter_index = sched->mCounterHandles.get(hCounter._value).counterIndex;
    sched->counterIncrement(sched->mCounters[counter_index], int(numTasks));

#if TD_USE_MOODYCAMEL
    for (auto i = 0u; i < numTasks; ++i)
    {
        tasks[i].mMetadata = counter_index;
    }

    // batch enqueue
    sched->mTasks.EnqueueBatch(tasks, priority);

#else
    // TODO: Multi-enqueue
    bool bSuccess = true;
    for (auto i = 0u; i < numTasks; ++i)
    {
        tasks[i].mMetadata = counter_index;

        bSuccess &= sched->mTasks.enqueue(tasks[i]);
    }
    CC_RUNTIME_ASSERT(bSuccess && "Task queue is full, consider increasing config.maxNumTasks");
#endif

    // signal the global event
    native::signalEvent(sched->mEventWorkAvailable);
}

int32_t td::waitForCounter(CounterHandle hCounter, bool bPinned)
{
    Scheduler* const sched = gSchedulerOnThread;
    CC_ASSERT(sched != nullptr && "Called from outside scheduler, use td::launchScheduler() first");
    CC_ASSERT(sched->mCounterHandles.is_alive(hCounter._value) && "waited on expired counter handle");

    auto const counterIdx = sched->mCounterHandles.get(hCounter._value).counterIndex;

    // The current fiber is now waiting, but not yet cleaned up
    sched->mFibers[gTLS.currentFiberIdx].bIsWaitingCleanedUp.store(false, std::memory_order_release);

    gTLS.bIsThreadWaiting = true;

    int counterValBeforeWait = -1;

    WaitingElement waitElem = {};
    waitElem.fiber = gTLS.currentFiberIdx;

    if (sched->counterAddWaitingFiber(sched->mCounters[counterIdx], waitElem, bPinned ? gTLS.threadIdx : invalid_thread, &counterValBeforeWait))
    {
        // Already done - resume immediately
    }
    else
    {
        // Not done yet, prepare to yield
        sched->yieldToFiber(sched->acquireFreeFiber(), EFiberDestination::Waiting);
    }

    gTLS.bIsThreadWaiting = false;

    // Either the counter was already on target, or this fiber has been awakened because it is now on target,
    // return execution
    return counterValBeforeWait;
}

int32_t td::incrementCounter(CounterHandle hCounter, uint32_t amount)
{
    Scheduler* const sched = gSchedulerOnThread;
    CC_ASSERT(sched != nullptr && "Called from outside scheduler, use td::launchScheduler() first");
    CC_ASSERT(hCounter.isValid() && "td::incrementCounter() called with invalid counter handle");

    auto const counter_index = sched->mCounterHandles.get(hCounter._value).counterIndex;
    return sched->counterIncrement(sched->mCounters[counter_index], (int32_t)amount);
}

int32_t td::decrementCounter(CounterHandle hCounter, uint32_t amount)
{
    Scheduler* const sched = gSchedulerOnThread;
    CC_ASSERT(sched != nullptr && "Called from outside scheduler, use td::launchScheduler() first");
    CC_ASSERT(hCounter.isValid() && "td::decrementCounter() called with invalid counter handle");

    auto const counter_index = sched->mCounterHandles.get(hCounter._value).counterIndex;
    return sched->counterIncrement(sched->mCounters[counter_index], (int32_t)amount * -1);
}

int32_t td::compareAndSwapCounter(CounterHandle hCounter, int32_t comparand, int32_t exchange)
{
    Scheduler* const sched = gSchedulerOnThread;
    CC_ASSERT(sched != nullptr && "Called from outside scheduler, use td::launchScheduler() first");
    CC_ASSERT(hCounter.isValid() && "td::compareAndSwapCounter() called with invalid counter handle");

    auto const counter_index = sched->mCounterHandles.get(hCounter._value).counterIndex;
    return sched->counterCompareAndSwap(sched->mCounters[counter_index], comparand, exchange);
}

int32_t td::createCounterDependency(CounterHandle hCounterToModify, CounterHandle hCounterToDependOn)
{
    Scheduler* const sched = gSchedulerOnThread;
    CC_ASSERT(sched != nullptr && "Called from outside scheduler, use td::launchScheduler() first");
    CC_ASSERT(hCounterToModify.isValid() && hCounterToDependOn.isValid() && "td::createCounterDependency() called with invalid counter handle");


    // immediately increment hCounterToModify by one
    auto const counterToModifyIdx = sched->mCounterHandles.get(hCounterToModify._value).counterIndex;
    sched->counterIncrement(sched->mCounters[counterToModifyIdx], 1);

    // enter hCounterToModify as a waiting element into hCounterToDependOn
    auto const counterToDependUponIdx = sched->mCounterHandles.get(hCounterToDependOn._value).counterIndex;
    int counterToDependUponValueBeforeWait = -1;

    WaitingElement waitElem = {};
    waitElem.counter = counterToModifyIdx;

    if (sched->counterAddWaitingFiber(sched->mCounters[counterToDependUponIdx], waitElem, invalid_thread, &counterToDependUponValueBeforeWait))
    {
        // immediately done, decrement hCounterToModify again
        sched->counterIncrement(sched->mCounters[counterToModifyIdx], -1);
    }

    return counterToDependUponValueBeforeWait;
}

int32_t td::getApproximateCounterValue(CounterHandle hCounter)
{
    Scheduler* const sched = gSchedulerOnThread;
    CC_ASSERT(sched != nullptr && "Called from outside scheduler, use td::launchScheduler() first");
    CC_ASSERT(hCounter.isValid() && "td::getApproximateCounterValue() called with invalid counter handle");

    auto const counter_index = sched->mCounterHandles.get(hCounter._value).counterIndex;
    return sched->mCounters[counter_index].count.load(std::memory_order_relaxed);
}

uint32_t td::getNumThreadsInScheduler()
{
    Scheduler* const sched = gSchedulerOnThread;
    CC_ASSERT(sched != nullptr && "Called from outside scheduler, use td::launchScheduler() first");

    return (uint32_t)sched->mThreads.size();
}

bool td::isInsideScheduler() { return gSchedulerOnThread != nullptr; }

uint32_t td::getCurrentThreadIndex() { return gTLS.threadIdx; }

uint32_t td::getCurrentFiberIndex() { return gTLS.currentFiberIdx; }
