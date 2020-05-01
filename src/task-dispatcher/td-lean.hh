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

inline void wait_for(sync& sync) { Scheduler::current().wait(sync, true, 0); }
inline void wait_for_unpinned(sync& sync) { Scheduler::current().wait(sync, false, 0); }

template <class... STs>
void wait_for(STs&... syncs)
{
    (Scheduler::current().wait(syncs, true, 0), ...);
}

template <class... STs>
void wait_for_unpinned(STs&... syncs)
{
    (Scheduler::current().wait(syncs, false, 0), ...);
}

// ==========
// Getter / Miscellaneous

/// returns true if the call is being made from within a scheduler
[[nodiscard]] inline bool is_scheduler_alive() { return Scheduler::isInsideScheduler(); }

/// returns the amount of threads the current scheduler has, only call if is_scheduler_alive() == true
[[nodiscard]] inline unsigned get_current_num_threads() { return Scheduler::current().getNumThreads(); }


// ==========
// Launch

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

template <class F>
void launch(F&& func)
{
    return launch(scheduler_config{}, cc::forward<F>(func));
}

template <class F>
void launch_singlethreaded(F&& func)
{
    scheduler_config config;
    config.num_threads = 1;
    return launch(config, cc::forward<F>(func));
}


// ==========
// Submit

// Raw submit from constructed Task types
inline void submit_raw(sync& sync, container::task* tasks, unsigned num) { td::Scheduler::current().submitTasks(tasks, num, sync); }
inline void submit_raw(sync& sync, cc::span<container::task> tasks) { submit_raw(sync, tasks.data(), unsigned(tasks.size())); }


// Single lambda
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

[[nodiscard]] inline sync submit_raw(cc::span<container::task> tasks)
{
    td::sync res;
    submit_raw(res, tasks.data(), unsigned(tasks.size()));
    return res;
}

[[nodiscard]] inline sync submit_raw(container::task* tasks, unsigned num)
{
    td::sync res;
    submit_raw(res, tasks, num);
    return res;
}
}
