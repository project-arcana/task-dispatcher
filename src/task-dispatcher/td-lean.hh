#pragma once

#include <type_traits>

#include <clean-core/assert.hh>
#include <clean-core/enable_if.hh>
#include <clean-core/forward.hh>
#include <clean-core/span.hh>
#include <clean-core/utility.hh>

#include <task-dispatcher/container/task.hh>
#include <task-dispatcher/scheduler.hh>
#include <task-dispatcher/sync.hh>

// td-lean.hh
// basic task-dispatcher API
// td::launch, td::wait_for, td::submit
//
// intended to be a (more) lightweight header, excluding <tuple> etc.

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
void launch(scheduler_config config, F&& func)
{
    static_assert(std::is_invocable_v<F>, "function must be invocable without arguments");
    static_assert(std::is_same_v<std::invoke_result_t<F>, void>, "return must be void");
    if (isInsideScheduler())
    {
        // if launch is nested, simply call the lambda without re-init
        cc::forward<F>(func).operator()();
        return;
    }

    container::task mainTask;
    mainTask.lambda(cc::forward<F>(func));
    td::launchScheduler(config, mainTask);
}

// launches a scheduler with default config, and calls the lambda as its main task
template <class F>
void launch(F&& func)
{
    return launch(scheduler_config{}, cc::forward<F>(func));
}

// launches a scheduler restricted to a single thread, and calls the lambda as its main task
template <class F>
void launch_singlethreaded(F&& func)
{
    scheduler_config config;
    config.num_threads = 1;
    return launch(config, cc::forward<F>(func));
}


// ==========
// Submit Tasks

// submit multiple pre-constructed tasks
inline void submit_raw(sync& sync, cc::span<td::container::task> tasks)
{
    CC_ASSERT(isInsideScheduler() && "attempted submit outside of live scheduler");

    if (!sync.handle.is_valid())
    {
        sync.handle = acquireCounter();
    }

    submitTasks(sync.handle, tasks);
}

// construct and submit a task
// based on a single "void f()" lambda or function pointer
template <class F, cc::enable_if<std::is_invocable_r_v<void, F>> = true>
void submit(sync& sync, F&& func)
{
    static_assert(std::is_invocable_v<F>, "function must be invocable without arguments");
    static_assert(std::is_invocable_r_v<void, F>, "return must be void");

    container::task dispatch;
    if constexpr (std::is_class_v<F>)
        dispatch.lambda(cc::forward<F>(func));
    else
        dispatch.lambda([=] { func(); });
    submit_raw(sync, cc::span{dispatch});
}

// ==========
// Submit Tasks - sync return variants

// submit multiple pre-constructed tasks and receive an associated new sync object
[[nodiscard]] inline sync submit_raw(cc::span<container::task> tasks)
{
    td::sync res;
    submit_raw(res, tasks);
    return res;
}

namespace detail
{
inline int32_t single_wait_for(sync& sync, bool pinned)
{
    if (!sync.handle.is_valid())
    {
        // return immediately for uninitialized syncs
        return 0;
    }

    // perform real wait
    int32_t const res = td::waitForCounter(sync.handle, pinned);

    // free the sync if it reached 0
    if (td::releaseCounterIfOnZero(sync.handle))
    {
        // mark as uninitialized
        sync.handle = handle::null_counter;
    }

    return res;
}
}

// ==========
// Wait on sync objects

/// waits on a sync object, returns it's value before the call
inline int32_t wait_for(sync& sync) { return detail::single_wait_for(sync, true); }

/// waits on a sync object, returns it's value before the call
/// unpinned: can resume execution on a different thread than the calling one
inline int32_t wait_for_unpinned(sync& sync) { return detail::single_wait_for(sync, false); }

template <class... STs>
[[deprecated("multi-wait overloads will be removed in a future version")]] void wait_for(STs&... syncs)
{
    (detail::single_wait_for(syncs, true), ...);
}

template <class... STs>
[[deprecated("multi-wait overloads will be removed in a future version")]] void wait_for_unpinned(STs&... syncs)
{
    (detail::single_wait_for(syncs, false), ...);
}

// ==========
// Directly manage counters
// this is a lower-level way of using task dispatcher
// counters are the internals of td::sync and must be created and destroyed
// they can also be incremented and decremented directly

[[deprecated("renamed to td::acquireCounter()")]] inline handle::counter acquire_counter() { return td::acquireCounter(); }
[[deprecated("renamed to td::releaseCounter()")]] inline int32_t release_counter(handle::counter handle) { return td::releaseCounter(handle); }

[[deprecated("renamed to td::releaseCounterIfOnZero()")]] inline bool release_counter_if_on_target(handle::counter handle, int32_t target)
{
    CC_ASSERT(target == 0 && "Non-zero counter counter targets are no longer supported");
    return td::releaseCounterIfOnZero(handle);
}

[[deprecated("renamed to td::incrementCounter")]] inline int32_t increment_counter(handle::counter handle, uint32_t amount = 1)
{
    return td::incrementCounter(handle, amount);
}
[[deprecated("renamed to td::incrementCounter")]] inline void decrement_counter(handle::counter handle, uint32_t amount = 1)
{
    td::decrementCounter(handle, amount);
}

/// waits until the counter reaches the target value
/// pinnned: if true, the task entering the wait can only resume on the OS thread this was called from
/// returns the value before the wait
[[deprecated("renamed to td::waitForCounter()")]] inline int32_t wait_for_counter(handle::counter handle, bool pinned, int32_t target = 0)
{
    CC_ASSERT(target == 0 && "Non-zero counter targets are no longer supported");
    return td::waitForCounter(handle, pinned);
}

[[deprecated("use td::submitTasks()")]] inline void submit_on_counter(handle::counter handle, container::task* tasks, uint32_t num)
{
    td::submitTasks(handle, cc::span(tasks, num));
}

template <class F, cc::enable_if<std::is_invocable_r_v<void, F>> = true>
void submit_on_counter(handle::counter handle, F&& func)
{
    static_assert(std::is_invocable_v<F>, "function must be invocable without arguments");

    container::task dispatch;

    if constexpr (std::is_class_v<F>)
        dispatch.lambda(cc::forward<F>(func));
    else
        dispatch.lambda([=] { func(); });

    submitTasks(handle, cc::span{dispatch});
}

template <class F>
uint32_t submit_batched_on_counter(handle::counter handle, F&& func, uint32_t num_elements, uint32_t max_num_batches, cc::allocator* scratch = cc::system_allocator)
{
    static_assert(std::is_invocable_v<F, uint32_t, uint32_t, uint32_t>, "function must be invocable with element start, end, and index argument");

    auto const batch_size = cc::int_div_ceil(num_elements, max_num_batches);
    auto const num_batches = cc::int_div_ceil(num_elements, batch_size);
    CC_ASSERT(num_batches <= max_num_batches && "programmer error");

    auto tasks = cc::alloc_array<td::container::task>::uninitialized(num_batches, scratch);

    for (uint32_t batch = 0u, start = 0u, end = cc::min(batch_size, num_elements); //
         batch < num_batches;                                                      //
         ++batch, start = batch * batch_size, end = cc::min((batch + 1) * batch_size, num_elements))
    {
        tasks[batch].lambda([=] { func(start, end, batch); });
    }

    submit_on_counter(handle, tasks.data(), num_batches);
    return num_batches;
}

}
