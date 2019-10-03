#pragma once

#include "container/task.hh"
#include "future.hh"
#include "scheduler.hh"
#include "sync.hh"

namespace td
{
// == launch ==

template <class F>
void launch(scheduler_config config, F&& func)
{
    if (Scheduler::isInsideScheduler())
        return;

    Scheduler scheduler(config);
    container::Task mainTask;
    mainTask.lambda(std::forward<F>(func));
    scheduler.start(mainTask);
}

template <class F>
void launch(F&& func)
{
    if (Scheduler::isInsideScheduler())
        return;

    Scheduler scheduler;
    container::Task mainTask;
    mainTask.lambda(std::forward<F>(func));
    scheduler.start(mainTask);
}

// == submit ==

template <class F, class... Args>
[[nodiscard]] auto submit(F&& fun, Args&&... args)
{
    static_assert(std::is_invocable_v<F, Args...>, "function must be invocable with the given args");
    using R = std::decay_t<std::invoke_result_t<F, Args...>>;
    if constexpr (std::is_same_v<R, void>)
    {
        sync res;
        container::Task dispatch([=] { fun(args...); });
        Scheduler::current().submitTasks(&dispatch, 1, res);
        return res;
    }
    else
    {
        static_assert(sizeof(R) == 0, "Unimplemented");
        return future<R>{};
    }
}
template <class F, class... Args>
void submit(sync& s, F&& fun, Args&&... args)
{
    static_assert(std::is_invocable_v<F, Args...>, "function must be invocable with the given args");
    static_assert(std::is_same_v<std::invoke_result_t<F, Args...>, void>, "return must be void");
    container::Task dispatch([=] { fun(args...); });
    Scheduler::current().submitTasks(&dispatch, 1, s);
}


template <class F>
void submit(sync& sync, F&& func)
{
    container::Task dispatch(std::forward<F>(func));
    Scheduler::current().submitTasks(&dispatch, 1, sync);
}

template <class F>
void submit_n(sync& sync, F&& func, unsigned n)
{
    auto tasks = new container::Task[n];
    for (auto i = 0u; i < n; ++i)
    {
        tasks[i].lambda([i, func]() { func(i); });
    }
    Scheduler::current().submitTasks(tasks, n, sync);
}

template <class F>
[[nodiscard]] sync submit(F&& func)
{
    sync res;
    container::Task dispatch(std::forward<F>(func));
    Scheduler::current().submitTasks(&dispatch, 1, res);
    return res;
}

template <class F>
[[nodiscard]] sync submit_n(F&& func, unsigned n)
{
    sync res;
    auto tasks = new container::Task[n];
    for (auto i = 0u; i < n; ++i)
        tasks[i].lambda([=]() { func(i); });
    Scheduler::current().submitTasks(tasks, n, res);
    return res;
}

// == wait ==

// TODO
// inline void wait_for(sync& sync) {}
inline void wait_for_unpinned(sync& sync) { Scheduler::current().wait(sync); }

template <class... STs>
void wait_for_unpinned(sync& s, sync& peek, STs&...tail)
{
    wait_for_unpinned(s);
    wait_for_unpinned(peek, tail...);
}


// == getter / misc ==

inline bool scheduler_alive() { return Scheduler::isInsideScheduler(); }

}
