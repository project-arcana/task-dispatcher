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
TD_API int32_t releaseCounter(CounterHandle counter);

// release a counter handle if it reached zero
// can be called concurrently for the same counter (only one caller will win)
// returns true if the counter was released
TD_API bool releaseCounterIfOnZero(CounterHandle counter);

// submit multiple tasks to the scheduler for immediate execution
// the counter is incremented by the amount of tasks and decremented once per completed task
TD_API void submitTasks(CounterHandle counter, cc::span<Task> tasks);

// waits until the counter reaches zero
// pinnned: if true, the task entering the wait can only resume on the same OS thread
// returns the counter value before the wait
TD_API int32_t waitForCounter(CounterHandle counter, bool pinned = true);

// manually increment a counter, preventing waits on it to resolve
// normally, a counter is incremented by 1 for every task submitted on it
// WARNING: without subsequent calls to decrementCounter, this will deadlock wait-calls on the sync
// returns the new counter value
TD_API int32_t incrementCounter(CounterHandle c, uint32_t amount = 1);

// manually decrement a sync object, potentially causing waits on it to resolve
// normally, a sync is decremented once a task submitted on it is finished
// WARNING: without previous calls to increment_sync, this will cause wait-calls to resolve before all tasks have finished
// returns the new counter value
TD_API int32_t decrementCounter(CounterHandle c, uint32_t amount = 1);

// returns the amount of threads the current scheduler has
TD_API uint32_t getNumThreadsInScheduler();

// returns true if the call is being made from within a scheduler
TD_API bool isInsideScheduler();

// returns the index of the current thread, or uint32_t(-1) on unowned threads
TD_API uint32_t getCurrentThreadIndex();

// returns the index of the current fiber, or uint32_t(-1) on unowned threads
TD_API uint32_t getCurrentFiberIndex();

}
