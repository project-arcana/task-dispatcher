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
// Wait

inline void wait_for(sync& sync) { Scheduler::Current().wait(sync, true, 0); }
inline void wait_for_unpinned(sync& sync) { Scheduler::Current().wait(sync, false, 0); }

template <class... STs>
void wait_for(STs&... syncs)
{
    (Scheduler::Current().wait(syncs, true, 0), ...);
}

template <class... STs>
void wait_for_unpinned(STs&... syncs)
{
    (Scheduler::Current().wait(syncs, false, 0), ...);
}

// ==========
// Getter / Miscellaneous

/// returns true if the call is being made from within a scheduler
[[nodiscard]] inline bool is_scheduler_alive() { return Scheduler::IsInsideScheduler(); }

/// returns the amount of threads the current scheduler has, only call if is_scheduler_alive() == true
[[nodiscard]] inline unsigned get_current_num_threads() { return Scheduler::Current().getNumThreads(); }

/// returns the index of the current thread, or unsigned(-1) on unowned threads
[[nodiscard]] inline unsigned current_thread_id() { return Scheduler::CurrentThreadIndex(); }

/// returns the index of the current fiber, or unsigned(-1) on unowned threads
[[nodiscard]] inline unsigned current_fiber_id() { return Scheduler::CurrentFiberIndex(); }

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
// Submit

/// submit multiple pre-constructed tasks
inline void submit_raw(sync& sync, container::task* tasks, unsigned num)
{
    CC_ASSERT(td::is_scheduler_alive() && "attempted submit outside of live scheduler");
    td::Scheduler::Current().submitTasks(tasks, num, sync);
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
// Sync return variants

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
}
