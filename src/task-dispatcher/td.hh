#pragma once

#include <clean-core/alloc_array.hh>
#include <clean-core/apply.hh>
#include <clean-core/assert.hh>
#include <clean-core/bits.hh>
#include <clean-core/enable_if.hh>
#include <clean-core/forward.hh>
#include <clean-core/move.hh>
#include <clean-core/span.hh>
#include <clean-core/tuple.hh>
#include <clean-core/unique_ptr.hh>
#include <clean-core/utility.hh>

#include <task-dispatcher/td-lean.hh>

// td.hh
// full task-dispatcher API
//
// contains additional batch-convenience submit variants,
// td::future submit and pointer-to-member-function submit

namespace td
{
// Future, move only
// Can be obtained when submitting invocables with return values
template <class T>
struct future
{
public:
    [[nodiscard]] T const& get()
    {
        td::wait_for(_sync);
        return *_value;
    }

    [[nodiscard]] T const& get_unpinned()
    {
        td::wait_for_unpinned(_sync);
        return *_value;
    }

    [[nodiscard]] T* get_raw_pointer() const { return _value.get(); }

    void set_sync(sync s) { _sync = s; }

public:
    future() : _value(cc::make_unique<T>()) {}
    ~future()
    {
        // Enforce sync guarantee
        td::wait_for(_sync);
    }

    future(future&) = delete;
    future& operator=(future&) = delete;

    future(future&& rhs) noexcept : _sync(rhs._sync), _value(cc::move(rhs._value)) { rhs._sync.handle.invalidate(); }
    future& operator=(future&& rhs) noexcept
    {
        if (this != &rhs)
        {
            td::wait_for(_sync);

            _sync = rhs._sync;
            _value = cc::move(rhs._value);
            rhs._sync.handle.invalidate();
        }
        return *this;
    }

private:
    sync _sync;
    cc::unique_ptr<T> _value;
};

// ==========
// Submit

/// submit a task based on a lambda, with arguments passed to it
/// arguments are moved into the task
template <class F, class... Args>
void submit(sync& s, F&& fun, Args&&... args)
{
    static_assert(std::is_invocable_v<F, Args...>, "function must be invocable with the given args");
    static_assert(std::is_same_v<std::invoke_result_t<F, Args...>, void>, "return must be void");
    // A lambda callung fun(args...), but moving the args instead of copying them into the lambda
    container::task dispatch([fun, tup = cc::tuple(cc::move(args)...)] { cc::apply([&fun](auto&&... args) { fun(decltype(args)(args)...); }, tup); });
    submit_raw(s, &dispatch, 1);
}

/// submit a task based on a member function, with arguments passed to it
/// arguments are moved into the task
template <class F, class FObj, class... Args, cc::enable_if<std::is_member_function_pointer_v<F>> = true>
void submit(sync& s, F func, FObj& inst, Args&&... args)
{
    static_assert(std::is_invocable_v<F, FObj, Args...>, "function must be invocable with the given args");
    static_assert(std::is_same_v<std::invoke_result_t<F, FObj, Args...>, void>, "return must be void");
    // A lambda calling fun(args...), but moving the args instead of copying them into the lambda
    container::task dispatch([func, inst_ptr = &inst, tup = cc::tuple(cc::move(args)...)]
                             { cc::apply([&func, &inst_ptr](auto&&... args) { (inst_ptr->*func)(decltype(args)(args)...); }, tup); });

    submit_raw(s, &dispatch, 1);
}

/// submits tasks calling a lambda "void f(unsigned i)" for each i from 0 to n
template <class F>
void submit_n(sync& sync, F&& func, unsigned n, cc::allocator* scratch_alloc = cc::system_allocator)
{
    static_assert(std::is_invocable_v<F, unsigned>, "function must be invocable with index argument");
    static_assert(std::is_same_v<std::invoke_result_t<F, unsigned>, void>, "return must be void");

    auto tasks = cc::alloc_array<td::container::task>::uninitialized(n, scratch_alloc);
    for (auto i = 0u; i < n; ++i)
        tasks[i].lambda([=] { func(i); });
    submit_raw(sync, tasks.data(), n);
}

/// submits tasks calling a lambda "void f(T& value)" for each element in the span
template <class T, class F>
void submit_each_ref(sync& sync, F&& func, cc::span<T> vals, cc::allocator* scratch_alloc = cc::system_allocator)
{
    static_assert(std::is_invocable_v<F, T&>, "function must be invocable with element reference");
    static_assert(std::is_same_v<std::invoke_result_t<F, T&>, void>, "return must be void");

    auto tasks = cc::alloc_array<td::container::task>::uninitialized(vals.size(), scratch_alloc);
    for (auto i = 0u; i < vals.size(); ++i)
        tasks[i].lambda([func, val_ptr = vals.data() + i] { func(*val_ptr); });

    submit_raw(sync, tasks.data(), unsigned(vals.size()));
}

/// submits tasks calling a lambda "void f(T value)" for each element in the span
template <class T, class F>
void submit_each_copy(sync& sync, F&& func, cc::span<T> vals, cc::allocator* scratch_alloc = cc::system_allocator)
{
    static_assert(std::is_invocable_v<F, T>, "function must be invocable with element copy");
    static_assert(std::is_same_v<std::invoke_result_t<F, T>, void>, "return must be void");

    auto tasks = cc::alloc_array<td::container::task>::uninitialized(vals.size(), scratch_alloc);
    for (auto i = 0u; i < vals.size(); ++i)
        tasks[i].lambda([func, val_copy = vals[i]] { func(val_copy); });

    submit_raw(sync, tasks.data(), unsigned(vals.size()));
}

/// submits tasks calling a lambda "void f(unsigned start, unsigned end)" for multiple batches over the range 0 to n
/// num_batches_max: maximum amount of batches to partition the range into
template <class F>
uint32_t submit_batched(sync& sync, F&& func, uint32_t n, uint32_t num_batches_max = td::system::num_logical_cores() * 4, cc::allocator* scratch_alloc = cc::system_allocator)
{
    static_assert(std::is_invocable_v<F, uint32_t, uint32_t>, "function must be invocable with batch start and end argument");

    auto batch_size = cc::int_div_ceil(n, num_batches_max);
    auto num_batches = cc::int_div_ceil(n, batch_size);

    CC_RUNTIME_ASSERT(num_batches <= num_batches_max && "programmer error");

    auto tasks = cc::alloc_array<td::container::task>::uninitialized(num_batches, scratch_alloc);

    for (unsigned batch = 0u, start = 0u, end = cc::min(batch_size, n); //
         batch < num_batches;                                           //
         ++batch, start = batch * batch_size, end = cc::min((batch + 1) * batch_size, n))
    {
        tasks[batch].lambda([=] { func(start, end); });
    }

    submit_raw(sync, tasks.data(), num_batches);
    return num_batches;
}

/// submits tasks calling a lambda "void f(unsigned start, unsigned end, unsigned batch_i)" for multiple batches over the range 0 to n
/// num_batches_max: maximum amount of batches to partition the range into
template <class F>
uint32_t submit_batched_n(sync& sync, F&& func, uint32_t num_elements, uint32_t max_num_batches = td::system::num_logical_cores() * 4, cc::allocator* scratch_alloc = cc::system_allocator)
{
    CC_ASSERT(is_scheduler_alive() && "scheduler not alive");
    if (!sync.handle.is_valid())
    {
        sync.handle = td::Scheduler::Current().acquireCounterHandle();
    }

    return submit_batched_on_counter<F>(sync.handle, cc::forward<F>(func), n, max_num_batches, scratch_alloc);
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
template <class F, class FObj, class... Args, cc::enable_if<std::is_member_function_pointer_v<F>> = true>
[[nodiscard]] auto submit(F func, FObj& inst, Args&&... args)
{
    static_assert(std::is_invocable_v<F, FObj, Args...>, "function must be invocable with the given args");
    using R = std::decay_t<std::invoke_result_t<F, FObj, Args...>>;
    if constexpr (std::is_same_v<R, void>)
    {
        sync res;

        // A lambda calling fun(args...), but moving the args instead of copying them into the lambda
        container::task dispatch([func, inst_ptr = &inst, tup = cc::tuple(cc::move(args)...)]
                                 { cc::apply([&func, &inst_ptr](auto&&... args) { (inst_ptr->*func)(decltype(args)(args)...); }, tup); });

        submit_raw(res, &dispatch, 1);
        return res;
    }
    else
    {
        sync s;
        future<R> res;
        R* const result_ptr = res.get_raw_pointer();

        // A lambda calling fun(args...), but moving the args instead of copying them into the lambda
        container::task dispatch(
            [func, inst_ptr = &inst, result_ptr, tup = cc::tuple(cc::move(args)...)]
            { cc::apply([&func, &inst_ptr, &result_ptr](auto&&... args) { *result_ptr = (inst_ptr->*func)(decltype(args)(args)...); }, tup); });

        submit_raw(s, &dispatch, 1);
        res.set_sync(s);
        return res;
    }
}

// Lambda with arguments - sync return variant, with optional return type
template <class F, class... Args, cc::enable_if<std::is_invocable_v<F, Args...> && !std::is_member_function_pointer_v<F>> = true>
[[nodiscard]] auto submit(F&& fun, Args&&... args)
{
    static_assert(std::is_invocable_v<F, Args...>, "function must be invocable with the given args");
    static_assert(!std::is_member_function_pointer_v<F>, "function must not be a function pointer");
    using R = std::decay_t<std::invoke_result_t<F, Args...>>;
    if constexpr (std::is_same_v<R, void>)
    {
        sync res;

        // A lambda callung fun(args...), but moving the args instead of copying them into the lambda
        container::task dispatch([fun, tup = cc::tuple(cc::move(args)...)]
                                 { cc::apply([&fun](auto&&... args) { fun(decltype(args)(args)...); }, tup); });

        submit_raw(res, &dispatch, 1);
        return res;
    }
    else
    {
        sync s;
        future<R> res;
        R* const result_ptr = res.get_raw_pointer();

        // A lambda callung fun(args...), but moving the args instead of copying them into the lambda
        container::task dispatch([fun, result_ptr, tup = cc::tuple(cc::move(args)...)]
                                 { cc::apply([&fun, &result_ptr](auto&&... args) { *result_ptr = fun(decltype(args)(args)...); }, tup); });

        submit_raw(s, &dispatch, 1);
        res.set_sync(s);
        return res;
    }
}

// ==========
// Sync return variants

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
[[nodiscard]] sync submit_batched(F&& func, unsigned n, unsigned num_batches_max = td::system::num_logical_cores() * 4)
{
    static_assert(std::is_invocable_v<F, unsigned, unsigned>, "function must be invocable with batch start and end argument");
    static_assert(std::is_same_v<std::invoke_result_t<F, unsigned, unsigned>, void>, "return must be void");
    sync res;
    submit_batched<F>(res, cc::forward<F>(func), n, num_batches_max);
    return res;
}

template <class F>
[[nodiscard]] sync submit_batched_n(F&& func, unsigned n, unsigned num_batches_max = td::system::num_logical_cores() * 4)
{
    static_assert(std::is_invocable_v<F, unsigned, unsigned, unsigned>, "function must be invocable with batch start, end, and index argument");
    static_assert(std::is_same_v<std::invoke_result_t<F, unsigned, unsigned, unsigned>, void>, "return must be void");
    sync res;
    submit_batched_n<F>(res, cc::forward<F>(func), n, num_batches_max);
    return res;
}
}
