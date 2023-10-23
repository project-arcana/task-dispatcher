#pragma once

#include <stdint.h>

#include <clean-core/fwd.hh>

#include <task-dispatcher/common/api.hh>
#include <task-dispatcher/fwd.hh>

namespace td
{
TD_API void launchScheduler(SchedulerConfig const& config, Task const& mainTask);

// create a counter handle
// used to associate submitted tasks with, and then wait upon completion
TD_API [[nodiscard]] CounterHandle acquireCounter();

// release a counter handle, returns last counter value
// must not be called concurrently for the same counter
TD_API int32_t releaseCounter(CounterHandle hCounter);

// release a counter handle if it reached zero
// can be called concurrently for the same counter (only one caller will win)
// returns true if the counter was released
TD_API bool releaseCounterIfOnZero(CounterHandle hCounter);

enum class ETaskPriority : uint32_t
{
    Realtime = 0,
    Default,

    NUM_PRIORITIES
};

// submit multiple tasks to the scheduler for immediate execution
// the counter is incremented by the amount of tasks and decremented once per completed task
// bRealtime: high priority task
TD_API void submitTasks(CounterHandle hCounter, cc::span<Task> tasks, ETaskPriority priority = ETaskPriority::Default);

// waits until the counter reaches zero
// bPinned: if true, the task entering the wait can only resume on the same OS thread
// returns the counter value before the wait
TD_API int32_t waitForCounter(CounterHandle hCounter, bool bPinned = true);

// manually increment a counter, preventing waits on it to resolve
// normally, a counter is incremented by 1 for every task submitted on it
// WARNING: without subsequent calls to decrementCounter, this will deadlock wait-calls on the sync
// returns the new counter value
TD_API int32_t incrementCounter(CounterHandle hCounter, uint32_t amount = 1);

// manually decrement a counter, potentially causing waits on it to resolve
// normally, a sync is decremented once a task submitted on it is finished
// WARNING: without previous calls to increment_sync, this will cause wait-calls to resolve before all tasks have finished
// returns the new counter value
TD_API int32_t decrementCounter(CounterHandle hCounter, uint32_t amount = 1);

// manually apply a compare-and-swap operation on a counter
// potentially causing waits on it to resolve like decrementCounter
// or preventing waits to resolve like incrementCounter
// returns the original value of the counter
// after this call, the counter will have the value of 'exchange' if the original value is equal to 'comparand'
TD_API int32_t compareAndSwapCounter(CounterHandle hCounter, int32_t comparand, int32_t exchange);

// creates a manual dependency between two counters
// hCounterToModify is incremented by one, and decremented again once hCounterToDependOn reaches zero
TD_API int32_t createCounterDependency(CounterHandle hCounterToModify, CounterHandle hCounterToDependOn);

// returns the approximate current counter value
// NOTE: value can be immediately out of date, should not be used to make threading decisions
TD_API int32_t getApproximateCounterValue(CounterHandle hCounter);

//
// Info
//

// returns the amount of threads the current scheduler has
TD_API uint32_t getNumThreadsInScheduler();

// returns true if the call is being made from within a scheduler
TD_API bool isInsideScheduler();

// returns the index of the current thread, or uint32_t(-1) on unowned threads
TD_API uint32_t getCurrentThreadIndex();

// returns the index of the current fiber, or uint32_t(-1) on unowned threads
TD_API uint32_t getCurrentFiberIndex();

//
// AutoCounter
//

// waits until the counter reaches zero
// pinnned: if true, the task entering the wait can only resume on the same OS thread
// returns the counter value before the wait
// WARNING: Do not call concurrently for the same AutoCounter - for advanced usage prefer explicit CounterHandle
TD_API int32_t waitForCounter(AutoCounter& autoCounter, bool bPinned = true);

// waiting on an AutoCounter requires writing access
/*TD_API*/ int32_t waitForCounter(AutoCounter const&, bool) = delete;

// AutoCounters must not be explicitly released
/*TD_API*/ int32_t releaseCounter(AutoCounter&) = delete;
/*TD_API*/ int32_t releaseCounter(AutoCounter const&) = delete;
/*TD_API*/ bool releaseCounterIfOnZero(AutoCounter&) = delete;
/*TD_API*/ bool releaseCounterIfOnZero(AutoCounter const&) = delete;
}
