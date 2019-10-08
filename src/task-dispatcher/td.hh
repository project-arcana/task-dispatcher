#pragma once

#include <memory>

#include <cc/span.hh>

#include "common/panic.hh"
#include "container/task.hh"
#include "scheduler.hh"
#include "sync.hh"

namespace td
{
// Divide ints and round up
// a > 0, b > 0
template <class T = int>
constexpr T int_div_ceil(T a, T b)
{
    return 1 + ((a - 1) / b);
}

// == wait ==

inline void wait_for(sync& sync) { Scheduler::current().wait(sync, true); }
inline void wait_for_unpinned(sync& sync) { Scheduler::current().wait(sync); }

template <class... STs>
void wait_for(STs&... syncs)
{
    (wait_for(syncs), ...);
}

template <class... STs>
void wait_for_unpinned(STs&... syncs)
{
    (wait_for_unpinned(syncs), ...);
}


// == getter / misc ==

[[nodiscard]] inline bool is_scheduler_alive() { return Scheduler::isInsideScheduler(); }

// Future - TODO: Rewrite
template <class T>
struct future
{
private:
    sync mSync;
    std::shared_ptr<T> const mValue;

public:
    [[nodiscard]] T const& get()
    {
        ::td::wait_for(mSync);
        return *mValue;
    }

    [[nodiscard]] T const& get_unpinned()
    {
        ::td::wait_for_unpinned(mSync);
        return *mValue;
    }

    [[nodiscard]] T* get_raw_pointer() const { return mValue.get(); }

    void set_sync(sync s) { mSync = s; }

    future() : mValue(std::make_shared<T>()) {}
    ~future()
    {
        // Enforce sync guarantee
        if (mSync.initialized)
            ::td::wait_for(mSync);
    }

    future(future&) = delete;
    future& operator=(future&) = delete;
    future(future&&) noexcept = default;
    future& operator=(future&&) noexcept = default;
};

// == launch ==

template <class F>
void launch(scheduler_config& config, F&& func)
{
    static_assert(std::is_invocable_v<F>, "function must be invocable without arguments");
    static_assert(std::is_same_v<std::invoke_result_t<F>, void>, "return must be void");
    if (is_scheduler_alive())
        return;

    config.ceil_to_pow2();
    TD_PANIC_IF(!config.is_valid(), "Scheduler configuration invalid");
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
    if (is_scheduler_alive())
        return;

    Scheduler scheduler;
    container::Task mainTask;
    mainTask.lambda(std::forward<F>(func));
    scheduler.start(mainTask);
}

// == submit ==

void submit_raw(sync& sync, container::Task* tasks, unsigned num) { td::Scheduler::current().submitTasks(tasks, num, sync); }
void submit_raw(sync& sync, cc::span<container::Task> tasks) { submit_raw(sync, tasks.data(), unsigned(tasks.size())); }

[[nodiscard]] sync submit_raw(cc::span<container::Task> tasks)
{
    td::sync res;
    submit_raw(res, tasks.data(), unsigned(tasks.size()));
    return res;
}

[[nodiscard]] sync submit_raw(container::Task* tasks, unsigned num)
{
    td::sync res;
    submit_raw(res, tasks, num);
    return res;
}


template <class F, class... Args>
[[nodiscard]] auto submit(F&& fun, Args&&... args)
{
    static_assert(std::is_invocable_v<F, Args...>, "function must be invocable with the given args");
    using R = std::decay_t<std::invoke_result_t<F, Args...>>;
    if constexpr (std::is_same_v<R, void>)
    {
        sync res;
        container::Task dispatch([=] { fun(args...); });
        submit_raw(res, &dispatch, 1);
        return res;
    }
    else
    {
        sync s;
        future<R> res;
        R* const result_ptr = res.get_raw_pointer();
        container::Task dispatch([=] { *result_ptr = fun(args...); });
        submit_raw(s, &dispatch, 1);
        res.set_sync(s);
        return res;
    }
}

template <class F, class... Args>
[[nodiscard]] auto submit(sync& s, F&& fun, Args&&... args)
{
    static_assert(std::is_invocable_v<F, Args...>, "function must be invocable with the given args");
    static_assert(std::is_same_v<std::invoke_result_t<F, Args...>, void>, "return must be void");
    container::Task dispatch([=] { fun(args...); });
    submit_raw(s, &dispatch, 1);
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
    submit_raw(sync, &dispatch, 1);
}

template <class F>
void submit_n(sync& sync, F&& func, unsigned n)
{
    static_assert(std::is_invocable_v<F, unsigned>, "function must be invocable with index argument");
    static_assert(std::is_same_v<std::invoke_result_t<F, unsigned>, void>, "return must be void");
    auto tasks = new container::Task[n];
    for (auto i = 0u; i < n; ++i)
        tasks[i].lambda([=] { func(i); });
    submit_raw(sync, tasks, n);
    delete[] tasks;
}

template <class F>
void submit_batched(sync& sync, F&& func, unsigned n, unsigned num_batches_target = td::system::hardware_concurrency * 4)
{
    static_assert(std::is_invocable_v<F, unsigned, unsigned>, "function must be invocable with batch start and end argument");
    static_assert(std::is_same_v<std::invoke_result_t<F, unsigned, unsigned>, void>, "return must be void");
    auto batch_size = int_div_ceil(n, num_batches_target);
    auto num_batches = int_div_ceil(n, batch_size);
    auto tasks = new container::Task[num_batches];

    for (auto batch = 0u, batchStart = 0u, batchEnd = min(batch_size, n); batch < num_batches;
         ++batch, batchStart = batch * batch_size, batchEnd = min((batch + 1) * batch_size, n))
        tasks[batch].lambda([=] { func(batchStart, batchEnd); });

    submit_raw(sync, tasks, num_batches);
    delete[] tasks;
}

template <class F>
[[nodiscard]] auto submit(F&& fun)
{
    static_assert(std::is_invocable_v<F>, "function must be invocable without arguments");
    using R = std::decay_t<std::invoke_result_t<F>>;
    if constexpr (std::is_same_v<R, void>)
    {
        sync res;
        container::Task dispatch([=] { fun(); });
        submit_raw(res, &dispatch, 1);
        return res;
    }
    else
    {
        sync s;
        future<R> res;
        R* const result_ptr = res.get_raw_pointer();
        container::Task dispatch([=] { *result_ptr = fun(); });
        submit_raw(s, &dispatch, 1);
        res.set_sync(s);
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
    submit_raw(res, tasks, n);
    delete[] tasks;
    return res;
}

}