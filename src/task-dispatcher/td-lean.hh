#pragma once

#include <type_traits>

#include <clean-core/assert.hh>
#include <clean-core/enable_if.hh>
#include <clean-core/forward.hh>
#include <clean-core/span.hh>

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
// Launch

/// launches a scheduler with the given config, and calls the lambda as its main task
template <class F>
void launch(scheduler_config config, F&& func)
{
    static_assert(std::is_invocable_v<F>, "function must be invocable without arguments");
    static_assert(std::is_same_v<std::invoke_result_t<F>, void>, "return must be void");
    if (is_scheduler_alive())
    {
        // if launch is nested, simply call the lambda without re-init
        cc::forward<F>(func).operator()();
        return;
    }

    config.ceil_to_pow2();
    CC_ASSERT(config.is_valid() && "Scheduler configuration invalid");
    Scheduler scheduler(config);
    container::task mainTask;
    mainTask.lambda(cc::forward<F>(func));
    scheduler.start(mainTask);
}

/// launches a scheduler with default config, and calls the lambda as its main task
template <class F>
void launch(F&& func)
{
    return launch(scheduler_config{}, cc::forward<F>(func));
}

/// launches a scheduler restricted to a single thread, and calls the lambda as its main task
template <class F>
void launch_singlethreaded(F&& func)
{
    scheduler_config config;
    config.num_threads = 1;
    return launch(config, cc::forward<F>(func));
}

// ==========
// Info

/// returns true if the call is being made from within a scheduler
[[nodiscard]] inline bool is_scheduler_alive() { return Scheduler::IsInsideScheduler(); }

/// returns the amount of threads the current scheduler has, only call if is_scheduler_alive() == true
[[nodiscard]] inline unsigned get_current_num_threads() { return Scheduler::Current().getNumThreads(); }

/// returns the index of the current thread, or unsigned(-1) on unowned threads
[[nodiscard]] inline unsigned current_thread_id() { return Scheduler::CurrentThreadIndex(); }

/// returns the index of the current fiber, or unsigned(-1) on unowned threads
[[nodiscard]] inline unsigned current_fiber_id() { return Scheduler::CurrentFiberIndex(); }


// ==========
// Submit Tasks

/// submit multiple pre-constructed tasks
inline void submit_raw(sync& sync, container::task* tasks, unsigned num)
{
    CC_ASSERT(td::is_scheduler_alive() && "attempted submit outside of live scheduler");

    if (!sync.initialized)
    {
        sync.handle = td::Scheduler::Current().acquireCounterHandle();
        sync.initialized = true;
    }

    td::Scheduler::Current().submitTasks(tasks, num, sync.handle);
}

/// submit multiple pre-constructed tasks
inline void submit_raw(sync& sync, cc::span<container::task> tasks) { submit_raw(sync, tasks.data(), unsigned(tasks.size())); }


/// construct and submit a task based on a single "void f()" lambda or function pointer
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
    submit_raw(sync, &dispatch, 1);
}


// ==========
// Submit Tasks - sync return variants

/// submit multiple pre-constructed tasks and receive an associated new sync object
[[nodiscard]] inline sync submit_raw(cc::span<container::task> tasks)
{
    td::sync res;
    submit_raw(res, tasks.data(), unsigned(tasks.size()));
    return res;
}

/// submit multiple pre-constructed tasks and receive an associated new sync object
[[nodiscard]] inline sync submit_raw(container::task* tasks, unsigned num)
{
    td::sync res;
    submit_raw(res, tasks, num);
    return res;
}

namespace detail
{
inline void single_wait_for(sync& sync, bool pinned)
{
    if (!sync.initialized)
    {
        // return immediately for uninitialized syncs
        return;
    }

    // perform real wait
    Scheduler::Current().wait(sync.handle, pinned, 0);

    // free the sync if it reached 0
    if (Scheduler::Current().releaseCounterIfOnTarget(sync.handle, 0))
    {
        // mark as uninitialized
        sync.initialized = false;
    }
}
}

// ==========
// Wait on sync objects

inline void wait_for(sync& sync) { detail::single_wait_for(sync, true); }
inline void wait_for_unpinned(sync& sync) { detail::single_wait_for(sync, false); }

template <class... STs>
void wait_for(STs&... syncs)
{
    (detail::single_wait_for(syncs, true), ...);
}

template <class... STs>
void wait_for_unpinned(STs&... syncs)
{
    (detail::single_wait_for(syncs, false), ...);
}


// ==========
// Experimental API - operation on counter handles

namespace experimental
{
/// manually create a counter handle
[[nodiscard]] inline counter_handle_t acquire_counter() { return Scheduler::Current().acquireCounterHandle(); }

/// manually release a counter handle, returns last counter
inline int release_counter(counter_handle_t handle) { return Scheduler::Current().releaseCounter(handle); }

/// manually release a counter handle if it reached a set target
[[nodiscard]] inline bool release_counter_if_on_target(counter_handle_t handle, int target)
{
    return Scheduler::Current().releaseCounterIfOnTarget(handle, target);
}

/// experimental: manually increment a sync object, preventing waits on it to resolve
/// normally, a sync is incremented by 1 for every task submitted on it
/// WARNING: without subsequent calls to decrement_sync, this will deadlock wait-calls on the sync
inline void increment_counter(counter_handle_t handle, unsigned amount = 1) { Scheduler::Current().incrementCounter(handle, amount); }


/// experimental: manually decrement a sync object, potentially causing waits on it to resolve
/// normally, a sync is decremented once a task submitted on it is finished
/// WARNING: without previous calls to increment_sync, this will cause wait-calls to resolve before all tasks have finished
inline void decrement_counter(counter_handle_t handle, unsigned amount = 1) { Scheduler::Current().decrementCounter(handle, amount); }

inline void wait_for_counter(counter_handle_t handle, bool pinned, int target = 0) { Scheduler::Current().wait(handle, pinned, target); }
}

}
