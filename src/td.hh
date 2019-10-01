#pragma once

#include "container/task.hh"
#include "future.hh"
#include "scheduler.hh"
#include "sync.hh"

namespace td
{
template <class F>
void launch(td::scheduler_config_t config, F&& func)
{
    if (td::scheduler::inside_scheduler())
        return;

    td::scheduler scheduler(config);
    container::Task mainTask;
    mainTask.lambda(std::forward<F>(func));
    scheduler.start(mainTask);
}

template <class F>
void launch(F&& func)
{
    if (td::scheduler::inside_scheduler())
        return;

    td::scheduler scheduler;
    container::Task mainTask;
    mainTask.lambda(std::forward<F>(func));
    scheduler.start(mainTask);
}

template <class F>
td::sync submit(F&& func)
{
    td::sync res;
    container::Task dispatch(std::forward<F>(func));
    td::scheduler::current().run_jobs(&dispatch, 1, res);
    return res;
}

template <class F>
void submit(td::sync& sync, F&& func)
{
    container::Task dispatch(std::forward<F>(func));
    td::scheduler::current().run_jobs(&dispatch, 1, sync);
}

template <class F>
void submit_n(td::sync& sync, F&& func, unsigned n)
{
    auto tasks = new td::container::Task[n];
    for (auto i = 0u; i < n; ++i)
    {
        tasks[i].lambda([i, func]() { func(i); });
    }
    td::scheduler::current().run_jobs(tasks, n, sync);
}

// TODO
// inline void wait_for(td::sync& sync) {

//}

inline void wait_for_unpinned(td::sync& sync) { td::scheduler::current().wait(sync); }

}
