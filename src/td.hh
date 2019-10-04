#pragma once

#include <memory>

#include "container/task.hh"
#include "future.hh"
#include "scheduler.hh"
#include "sync.hh"

namespace td
{
// == wait ==

// TODO
// inline void wait_for(sync& sync) {}
inline void wait_for_unpinned(sync& sync) { Scheduler::current().wait(sync); }

template <class... STs>
void wait_for_unpinned(sync& s, sync& peek, STs&... tail)
{
    wait_for_unpinned(s);
    wait_for_unpinned(peek, tail...);
}


// == getter / misc ==

inline bool scheduler_alive() { return Scheduler::isInsideScheduler(); }

// Future - TODO: Rewrite
template <class T>
struct future
{
private:
    sync mSync;
    std::shared_ptr<T> const mValue;

public:
    // TODO
    //    T get()  {
    //        ::td::wait_for(mSync);
    //        return *mValue;
    //    }

    T get_unpinned()
    {
        ::td::wait_for_unpinned(mSync);
        return *mValue;
    }

    T* get_raw_pointer() const { return mValue.get(); }

    future(sync s) : mSync(s), mValue(std::make_shared<T>()) {}
    ~future()
    {
        // Enforce sync guarantee
        ::td::wait_for_unpinned(mSync);
    }

    future(future&) = delete;
    future& operator=(future&) = delete;
    future(future&&) noexcept = default;
    future& operator=(future&&) noexcept = default;
};

// == launch ==

template <class F>
void launch(scheduler_config const& config, F&& func)
{
    static_assert(std::is_invocable_v<F>, "function must be invocable without arguments");
    static_assert(std::is_same_v<std::invoke_result_t<F>, void>, "return must be void");
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
    static_assert(std::is_invocable_v<F>, "function must be invocable without arguments");
    static_assert(std::is_same_v<std::invoke_result_t<F>, void>, "return must be void");
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
        sync s;
        auto res = future<R>{s};
        R* const result_ptr = res.get_raw_pointer();

        container::Task dispatch([=] { *result_ptr = fun(args...); });
        Scheduler::current().submitTasks(&dispatch, 1, s);
        return res;
    }
}

template <class F, class... Args>
auto submit(sync& s, F&& fun, Args&&... args)
{
    static_assert(std::is_invocable_v<F, Args...>, "function must be invocable with the given args");
    static_assert(std::is_same_v<std::invoke_result_t<F, Args...>, void>, "return must be void");
    container::Task dispatch([=] { fun(args...); });
    Scheduler::current().submitTasks(&dispatch, 1, s);
}

// TODO
// template <class F, class FObj, class... Args>
// void submit(sync& s, F&& fun, FObj& inst, Args&&... args)
//{
//    using FuncT = decltype (inst.*fun);
//    static_assert(std::is_invocable_v<FuncT, Args...>, "function must be invocable with the given args");
//    static_assert(std::is_same_v<std::invoke_result_t<FuncT, Args...>, void>, "return must be void");
//    container::Task dispatch([fun, args..., &inst] { (inst.*fun)(args...); });
//    Scheduler::current().submitTasks(&dispatch, 1, s);
//}


template <class F>
void submit(sync& sync, F&& func)
{
    static_assert(std::is_invocable_v<F>, "function must be invocable without arguments");
    static_assert(std::is_same_v<std::invoke_result_t<F>, void>, "return must be void");
    container::Task dispatch(std::forward<F>(func));
    Scheduler::current().submitTasks(&dispatch, 1, sync);
}

template <class F>
void submit_n(sync& sync, F&& func, unsigned n)
{
    static_assert(std::is_invocable_v<F, unsigned>, "function must be invocable with index argument");
    static_assert(std::is_same_v<std::invoke_result_t<F, unsigned>, void>, "return must be void");
    auto tasks = new container::Task[n];
    for (auto i = 0u; i < n; ++i)
        tasks[i].lambda([=] { func(i); });
    Scheduler::current().submitTasks(tasks, n, sync);
    delete[] tasks;
}

[[nodiscard]] sync submit_raw(container::Task* tasks, unsigned num)
{
    td::sync res;
    td::Scheduler::current().submitTasks(tasks, num, res);
    return res;
}

void submit_raw(sync& sync, container::Task* tasks, unsigned num) { td::Scheduler::current().submitTasks(tasks, num, sync); }

template <class F>
[[nodiscard]] auto submit(F&& fun)
{
    static_assert(std::is_invocable_v<F>, "function must be invocable without arguments");
    using R = std::decay_t<std::invoke_result_t<F>>;
    if constexpr (std::is_same_v<R, void>)
    {
        sync res;
        container::Task dispatch([=] { fun(); });
        Scheduler::current().submitTasks(&dispatch, 1, res);
        return res;
    }
    else
    {
        sync s;
        future<R> res{s};
        R* const result_ptr = res.get_raw_pointer();

        container::Task dispatch([=] { *result_ptr = fun(); });
        Scheduler::current().submitTasks(&dispatch, 1, s);

        return res;
    }
}

template <class F>
[[nodiscard]] sync submit_n(F&& func, unsigned n)
{
    static_assert(std::is_invocable_v<F, unsigned>, "function must be invocable with index argument");
    static_assert(std::is_same_v<std::invoke_result_t<F, unsigned>, void>, "return must be void");
    sync res;
    auto tasks = new container::Task[n];
    for (auto i = 0u; i < n; ++i)
        tasks[i].lambda([=] { func(i); });
    Scheduler::current().submitTasks(tasks, n, res);
    delete[] tasks;
    return res;
}

}
