#pragma once

#include <tuple> // TODO: Replace with cc::tuple

#include <clean-core/array.hh>
#include <clean-core/assert.hh>
#include <clean-core/forward.hh>
#include <clean-core/move.hh>
#include <clean-core/span.hh>
#include <clean-core/unique_ptr.hh>

#include "container/task.hh"
#include "scheduler.hh"
#include "sync.hh"

namespace td
{
namespace detail
{
// Divide ints and round up
// a > 0, b > 0
template <class T = int>
constexpr T int_div_ceil(T a, T b)
{
    return 1 + ((a - 1) / b);
}

template <class T = int>
constexpr T min(T a, T b)
{
    return a < b ? a : b;
}
}

// ==========
// Wait

inline void wait_for(sync& sync) { Scheduler::current().wait(sync, true, 0); }
inline void wait_for_unpinned(sync& sync) { Scheduler::current().wait(sync, false, 0); }

template <class... STs>
void wait_for(STs&... syncs)
{
    using firstST = typename std::tuple_element<0, std::tuple<STs...>>::type;
    static_assert(std::is_same_v<firstST, sync>, "td::wait_for: wrong argument type - check if the sync objects are not const");
    (wait_for(syncs), ...);
}

template <class... STs>
void wait_for_unpinned(STs&... syncs)
{
    using firstST = typename std::tuple_element<0, std::tuple<STs...>>::type;
    static_assert(std::is_same_v<firstST, sync>, "td::wait_for_unpinned: wrong argument type - check if the sync objects are not const");
    (wait_for_unpinned(syncs), ...);
}


// ==========
// Getter / Miscellaneous

[[nodiscard]] inline bool is_scheduler_alive() { return Scheduler::isInsideScheduler(); }

// Future, move only
// Can be obtained when submitting invocables with return values
template <class T>
struct future
{
public:
    future() : _value(cc::make_unique<T>()) {}
    ~future()
    {
        // Enforce sync guarantee
        if (_sync.initialized)
            ::td::wait_for(_sync);
    }

    [[nodiscard]] T const& get()
    {
        ::td::wait_for(_sync);
        return *_value;
    }

    [[nodiscard]] T const& get_unpinned()
    {
        ::td::wait_for_unpinned(_sync);
        return *_value;
    }

    [[nodiscard]] T* get_raw_pointer() const { return _value.get(); }

    void set_sync(sync s) { _sync = s; }

    future(future&) = delete;
    future& operator=(future&) = delete;

    future(future&& rhs) noexcept : _sync(rhs._sync), _value(cc::move(rhs._value)) { rhs._sync.initialized = false; }
    future& operator=(future&& rhs) noexcept
    {
        _sync = rhs._sync;
        _value = cc::move(rhs._value);
        rhs._sync.initialized = false;
    }

private:
    sync _sync;
    cc::unique_ptr<T> _value;
};

// ==========
// Launch

template <class F>
void launch(scheduler_config config, F&& func)
{
    static_assert(std::is_invocable_v<F>, "function must be invocable without arguments");
    static_assert(std::is_same_v<std::invoke_result_t<F>, void>, "return must be void");
    if (is_scheduler_alive())
        return;

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
    static_assert(std::is_invocable_v<F>, "function must be invocable without arguments");
    static_assert(std::is_same_v<std::invoke_result_t<F>, void>, "return must be void");
    if (is_scheduler_alive())
        return;

    Scheduler scheduler;
    container::task mainTask;
    mainTask.lambda(cc::forward<F>(func));
    scheduler.start(mainTask);
}

// ==========
// Submit

// Raw submit from constructed Task types
inline void submit_raw(sync& sync, container::task* tasks, unsigned num) { td::Scheduler::current().submitTasks(tasks, num, sync); }
inline void submit_raw(sync& sync, cc::span<container::task> tasks) { submit_raw(sync, tasks.data(), unsigned(tasks.size())); }

// TODO Single pointer to member with arguments
// template <class F, class FObj, class... Args>
// void submit(sync& s, F&& fun, FObj& inst, Args&&... args)
//{
//    using FuncT = decltype (inst.*fun);
//    static_assert(std::is_invocable_v<FuncT, Args...>, "function must be invocable with the given args");
//    static_assert(std::is_same_v<std::invoke_result_t<FuncT, Args...>, void>, "return must be void");
//    container::task dispatch([fun, args..., &inst] { (inst.*fun)(args...); });
//    Scheduler::current().submitTasks(&dispatch, 1, s);
//}

// Single lambda
template <class F, std::enable_if_t<std::is_invocable_r_v<void, F>, int> = 0>
void submit(sync& sync, F&& func)
{
    static_assert(std::is_invocable_v<F>, "function must be invocable without arguments");
    static_assert(std::is_invocable_r_v<void, F>, "return must be void");

    container::task dispatch;
    dispatch.lambda(cc::forward<F>(func));
    submit_raw(sync, &dispatch, 1);
}

// Single lambda with arguments
template <class F, class... Args>
void submit(sync& s, F&& fun, Args&&... args)
{
    static_assert(std::is_invocable_v<F, Args...>, "function must be invocable with the given args");
    static_assert(std::is_same_v<std::invoke_result_t<F, Args...>, void>, "return must be void");
    // A lambda callung fun(args...), but moving the args instead of copying them into the lambda
    container::task dispatch(
        [fun, tup = std::make_tuple(cc::move(args)...)] { std::apply([&fun](auto&&... args) { fun(decltype(args)(args)...); }, tup); });
    submit_raw(s, &dispatch, 1);
}

// Pointer to member function with arguments - sync return variant, with optional return type
template <class F, class FObj, class... Args, std::enable_if_t<std::is_member_function_pointer_v<F>, int> = 0>
void submit(sync& s, F func, FObj& inst, Args&&... args)
{
    static_assert(std::is_invocable_v<F, FObj, Args...>, "function must be invocable with the given args");
    static_assert(std::is_same_v<std::invoke_result_t<F, FObj, Args...>, void>, "return must be void");
    // A lambda calling fun(args...), but moving the args instead of copying them into the lambda
    container::task dispatch([func, inst_ptr = &inst, tup = std::make_tuple(cc::move(args)...)] {
        std::apply([&func, &inst_ptr](auto&&... args) { (inst_ptr->*func)(decltype(args)(args)...); }, tup);
    });

    submit_raw(s, &dispatch, 1);
}

// Lambda called n times with index argument
template <class F>
void submit_n(sync& sync, F&& func, unsigned n)
{
    static_assert(std::is_invocable_v<F, unsigned>, "function must be invocable with index argument");
    static_assert(std::is_same_v<std::invoke_result_t<F, unsigned>, void>, "return must be void");

    auto tasks = cc::array<td::container::task>::uninitialized(n);
    for (auto i = 0u; i < n; ++i)
        tasks[i].lambda([=] { func(i); });
    submit_raw(sync, tasks.data(), n);
}

// Lambda called for each element with element reference
template <class T, class F>
void submit_each_ref(sync& sync, F&& func, cc::span<T> vals)
{
    static_assert(std::is_invocable_v<F, T&>, "function must be invocable with element reference");
    static_assert(std::is_same_v<std::invoke_result_t<F, T&>, void>, "return must be void");

    auto tasks = cc::array<td::container::task>::uninitialized(vals.size());
    for (auto i = 0u; i < vals.size(); ++i)
        tasks[i].lambda([func, val_ptr = vals.data() + i] { func(*val_ptr); });

    submit_raw(sync, tasks.data(), unsigned(vals.size()));
}

// Lambda called for each element with element copy
template <class T, class F>
void submit_each_copy(sync& sync, F&& func, cc::span<T> vals)
{
    static_assert(std::is_invocable_v<F, T>, "function must be invocable with element copy");
    static_assert(std::is_same_v<std::invoke_result_t<F, T>, void>, "return must be void");

    auto tasks = cc::array<td::container::task>::uninitialized(vals.size());
    for (auto i = 0u; i < vals.size(); ++i)
        tasks[i].lambda([func, val_copy = vals[i]] { func(val_copy); });

    submit_raw(sync, tasks.data(), unsigned(vals.size()));
}

// Lambda called for each batch, with batch start and end
template <class F>
void submit_batched(sync& sync, F&& func, unsigned n, unsigned num_batches_max = td::system::hardware_concurrency * 4)
{
    static_assert(std::is_invocable_v<F, unsigned, unsigned>, "function must be invocable with batch start and end argument");
    static_assert(std::is_same_v<std::invoke_result_t<F, unsigned, unsigned>, void>, "return must be void");

    auto batch_size = detail::int_div_ceil(n, num_batches_max);
    auto num_batches = detail::int_div_ceil(n, batch_size);

    CC_RUNTIME_ASSERT(num_batches <= num_batches_max && "programmer error");

    auto tasks = cc::array<td::container::task>::uninitialized(num_batches);

    for (auto batch = 0u, batchStart = 0u, batchEnd = detail::min(batch_size, n); batch < num_batches;
         ++batch, batchStart = batch * batch_size, batchEnd = detail::min((batch + 1) * batch_size, n))
        tasks[batch].lambda([=] { func(batchStart, batchEnd); });

    submit_raw(sync, tasks.data(), num_batches);
}

// Lambda called for each batch, with batch start, end, and batch index
template <class F>
void submit_batched_n(sync& sync, F&& func, unsigned n, unsigned num_batches_max = td::system::hardware_concurrency * 4)
{
    static_assert(std::is_invocable_v<F, unsigned, unsigned, unsigned>, "function must be invocable with batch start, end, and index argument");
    static_assert(std::is_same_v<std::invoke_result_t<F, unsigned, unsigned, unsigned>, void>, "return must be void");

    auto batch_size = detail::int_div_ceil(n, num_batches_max);
    auto num_batches = detail::int_div_ceil(n, batch_size);

    CC_RUNTIME_ASSERT(num_batches <= num_batches_max && "programmer error");

    auto tasks = cc::array<td::container::task>::uninitialized(num_batches);

    for (auto batch = 0u, batchStart = 0u, batchEnd = detail::min(batch_size, n); batch < num_batches;
         ++batch, batchStart = batch * batch_size, batchEnd = detail::min((batch + 1) * batch_size, n))
        tasks[batch].lambda([=] { func(batchStart, batchEnd, batch); });

    submit_raw(sync, tasks.data(), num_batches);
}

// ==========
// Lambdas with return values

// Lambda - sync return variant, with optional return type
template <class F>
[[nodiscard]] auto submit(F&& fun)
{
    static_assert(std::is_invocable_v<F>, "function must be invocable without arguments");
    using R = std::decay_t<std::invoke_result_t<F>>;
    if constexpr (std::is_same_v<R, void>)
    {
        sync res;
        container::task dispatch([=] { fun(); });
        submit_raw(res, &dispatch, 1);
        return res;
    }
    else
    {
        sync s;
        future<R> res;
        R* const result_ptr = res.get_raw_pointer();
        container::task dispatch([=] { *result_ptr = fun(); });
        submit_raw(s, &dispatch, 1);
        res.set_sync(s);
        return res;
    }
}

// Pointer to member function with arguments - sync return variant, with optional return type
template <class F, class FObj, class... Args, std::enable_if_t<std::is_member_function_pointer_v<F>, int> = 0>
[[nodiscard]] auto submit(F func, FObj& inst, Args&&... args)
{
    static_assert(std::is_invocable_v<F, FObj, Args...>, "function must be invocable with the given args");
    using R = std::decay_t<std::invoke_result_t<F, FObj, Args...>>;
    if constexpr (std::is_same_v<R, void>)
    {
        sync res;

        // A lambda calling fun(args...), but moving the args instead of copying them into the lambda
        container::task dispatch([func, inst_ptr = &inst, tup = std::make_tuple(cc::move(args)...)] {
            std::apply([&func, &inst_ptr](auto&&... args) { (inst_ptr->*func)(decltype(args)(args)...); }, tup);
        });

        submit_raw(res, &dispatch, 1);
        return res;
    }
    else
    {
        sync s;
        future<R> res;
        R* const result_ptr = res.get_raw_pointer();

        // A lambda calling fun(args...), but moving the args instead of copying them into the lambda
        container::task dispatch([func, inst_ptr = &inst, result_ptr, tup = std::make_tuple(cc::move(args)...)] {
            std::apply([&func, &inst_ptr, &result_ptr](auto&&... args) { *result_ptr = (inst_ptr->*func)(decltype(args)(args)...); }, tup);
        });

        submit_raw(s, &dispatch, 1);
        res.set_sync(s);
        return res;
    }
}

// Lambda with arguments - sync return variant, with optional return type
template <class F, class... Args, std::enable_if_t<std::is_invocable_v<F, Args...> && !std::is_member_function_pointer_v<F>, int> = 0>
[[nodiscard]] auto submit(F&& fun, Args&&... args)
{
    static_assert(std::is_invocable_v<F, Args...>, "function must be invocable with the given args");
    static_assert(!std::is_member_function_pointer_v<F>, "function must not be a function pointer");
    using R = std::decay_t<std::invoke_result_t<F, Args...>>;
    if constexpr (std::is_same_v<R, void>)
    {
        sync res;

        // A lambda callung fun(args...), but moving the args instead of copying them into the lambda
        container::task dispatch(
            [fun, tup = std::make_tuple(cc::move(args)...)] { std::apply([&fun](auto&&... args) { fun(decltype(args)(args)...); }, tup); });

        submit_raw(res, &dispatch, 1);
        return res;
    }
    else
    {
        sync s;
        future<R> res;
        R* const result_ptr = res.get_raw_pointer();

        // A lambda callung fun(args...), but moving the args instead of copying them into the lambda
        container::task dispatch([fun, result_ptr, tup = std::make_tuple(cc::move(args)...)] {
            std::apply([&fun, &result_ptr](auto&&... args) { *result_ptr = fun(decltype(args)(args)...); }, tup);
        });

        submit_raw(s, &dispatch, 1);
        res.set_sync(s);
        return res;
    }
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

template <class F>
[[nodiscard]] sync submit_n(F&& func, unsigned n)
{
    static_assert(std::is_invocable_v<F, unsigned>, "function must be invocable with index argument");
    static_assert(std::is_same_v<std::invoke_result_t<F, unsigned>, void>, "return must be void");
    sync res;
    submit_n<F>(res, cc::forward<F>(func), n);
    return res;
}

template <class T, class F>
[[nodiscard]] sync submit_each_ref(F&& func, cc::span<T> vals)
{
    static_assert(std::is_invocable_v<F, T&>, "function must be invocable with element reference");
    static_assert(std::is_same_v<std::invoke_result_t<F, T&>, void>, "return must be void");
    sync res;
    submit_each_ref<T, F>(res, cc::forward<F>(func), vals);
    return res;
}

template <class T, class F>
[[nodiscard]] sync submit_each_copy(F&& func, cc::span<T> vals)
{
    static_assert(std::is_invocable_v<F, T>, "function must be invocable with element value");
    static_assert(std::is_same_v<std::invoke_result_t<F, T>, void>, "return must be void");
    sync res;
    submit_each_copy<T, F>(res, cc::forward<F>(func), vals);
    return res;
}

template <class F>
[[nodiscard]] sync submit_batched(F&& func, unsigned n, unsigned num_batches_max = td::system::hardware_concurrency * 4)
{
    static_assert(std::is_invocable_v<F, unsigned, unsigned>, "function must be invocable with batch start and end argument");
    static_assert(std::is_same_v<std::invoke_result_t<F, unsigned, unsigned>, void>, "return must be void");
    sync res;
    submit_batched<F>(res, cc::forward<F>(func), n, num_batches_max);
    return res;
}

template <class F>
[[nodiscard]] sync submit_batched_n(F&& func, unsigned n, unsigned num_batches_max = td::system::hardware_concurrency * 4)
{
    static_assert(std::is_invocable_v<F, unsigned, unsigned, unsigned>, "function must be invocable with batch start, end, and index argument");
    static_assert(std::is_same_v<std::invoke_result_t<F, unsigned, unsigned, unsigned>, void>, "return must be void");
    sync res;
    submit_batched_n<F>(res, cc::forward<F>(func), n, num_batches_max);
    return res;
}
}
