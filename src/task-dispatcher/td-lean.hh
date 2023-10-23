#pragma once

#include <clean-core/assert.hh>

#include <task-dispatcher/AutoCounter.hh>
#include <task-dispatcher/Scheduler.hh>
#include <task-dispatcher/SchedulerConfig.hh>
#include <task-dispatcher/container/Task.hh>

#include <task-dispatcher/LambdaSubmission.hh>

// td-lean.hh
// sink header excluding some heavyweight additional features

namespace td
{
// ==========
// Info

[[deprecated("renamed to td::isInsideScheduler()")]] inline bool is_scheduler_alive() { return isInsideScheduler(); }
[[deprecated("renamed to td::getNumThreadsInScheduler()")]] inline uint32_t get_current_num_threads() { return getNumThreadsInScheduler(); }
[[deprecated("renamed to td::getCurrentThreadIndex()")]] inline uint32_t current_thread_id() { return getCurrentThreadIndex(); }
[[deprecated("renamed to td::getCurrentFiberIndex()")]] inline uint32_t current_fiber_id() { return getCurrentFiberIndex(); }

// ==========
// Launch

// launches a scheduler with the given config, and calls the lambda as its main task
template <class F>
void launch(SchedulerConfig const& config, F&& func)
{
    static_assert(std::is_invocable_v<F>, "function must be invocable without arguments");
    static_assert(std::is_same_v<std::invoke_result_t<F>, void>, "return must be void");

    Task mainTask;
    mainTask.initWithLambda(cc::forward<F>(func));
    td::launchScheduler(config, mainTask);
}

// launches a scheduler with default config, and calls the lambda as its main task
template <class F>
void launch(F&& func)
{
    return launch(SchedulerConfig{}, cc::forward<F>(func));
}

// launches a scheduler restricted to a single thread, and calls the lambda as its main task
template <class F>
void launchSinglethreaded(F&& func)
{
    SchedulerConfig config;
    config.numThreads = 1;
    return launch(config, cc::forward<F>(func));
}

[[deprecated("renamed to td::acquireCounter()")]] inline CounterHandle acquire_counter() { return td::acquireCounter(); }
[[deprecated("renamed to td::releaseCounter()")]] inline int32_t release_counter(CounterHandle handle) { return td::releaseCounter(handle); }

[[deprecated("renamed to td::releaseCounterIfOnZero()")]] inline bool release_counter_if_on_target(CounterHandle handle, int32_t target)
{
    CC_ASSERT(target == 0 && "Non-zero counter counter targets are no longer supported");
    return td::releaseCounterIfOnZero(handle);
}

[[deprecated("renamed to td::incrementCounter")]] inline int32_t increment_counter(CounterHandle handle, uint32_t amount = 1)
{
    return td::incrementCounter(handle, amount);
}
[[deprecated("renamed to td::incrementCounter")]] inline void decrement_counter(CounterHandle handle, uint32_t amount = 1)
{
    td::decrementCounter(handle, amount);
}

/// waits until the counter reaches the target value
/// pinnned: if true, the task entering the wait can only resume on the OS thread this was called from
/// returns the value before the wait
[[deprecated("renamed to td::waitForCounter()")]] inline int32_t wait_for_counter(CounterHandle handle, bool pinned, int32_t target = 0)
{
    CC_ASSERT(target == 0 && "Non-zero counter targets are no longer supported");
    return td::waitForCounter(handle, pinned);
}

[[deprecated("use td::submitTasks()")]] inline void submit_on_counter(CounterHandle handle, Task* tasks, uint32_t num)
{
    td::submitTasks(handle, cc::span(tasks, num));
}

template <class F, cc::enable_if<std::is_invocable_r_v<void, F>> = true>
[[deprecated("renamed to td::submitLambda()")]] void submit_on_counter(CounterHandle handle, F&& func)
{
    td::submitLambda<F>(handle, cc::forward<F>(func));
}

template <class F>
[[deprecated("renamed to td::submitBatched()")]] uint32_t submit_batched_on_counter(
    CounterHandle handle, F&& func, uint32_t num_elements, uint32_t max_num_batches, cc::allocator* scratch = cc::system_allocator)
    = delete;
}
