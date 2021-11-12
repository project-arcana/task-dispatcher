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
#include <clean-core/utility.hh>

#include <task-dispatcher/td-lean.hh>

// td.hh
// full task-dispatcher API
// contains additional convenience submit variants

namespace td
{
// ==========
// Submit

/// submit a task based on a lambda, with arguments passed to it
/// arguments are moved into the task
template <class F, class... Args>
void submit(Sync& s, F&& fun, Args&&... args)
{
    static_assert(std::is_invocable_v<F, Args...>, "function must be invocable with the given args");
    static_assert(std::is_same_v<std::invoke_result_t<F, Args...>, void>, "return must be void");
    // A lambda callung fun(args...), but moving the args instead of copying them into the lambda
    Task dispatch([fun, tup = cc::tuple(cc::move(args)...)] { cc::apply([&fun](auto&&... args) { fun(decltype(args)(args)...); }, tup); });
    submit_raw(s, cc::span{dispatch});
}

/// submit a task based on a member function, with arguments passed to it
/// arguments are moved into the task
template <class F, class FObj, class... Args, cc::enable_if<std::is_member_function_pointer_v<F>> = true>
void submit(Sync& s, F func, FObj& inst, Args&&... args)
{
    static_assert(std::is_invocable_v<F, FObj, Args...>, "function must be invocable with the given args");
    static_assert(std::is_same_v<std::invoke_result_t<F, FObj, Args...>, void>, "return must be void");
    // A lambda calling fun(args...), but moving the args instead of copying them into the lambda
    Task dispatch([func, inst_ptr = &inst, tup = cc::tuple(cc::move(args)...)]
                  { cc::apply([&func, &inst_ptr](auto&&... args) { (inst_ptr->*func)(decltype(args)(args)...); }, tup); });

    submit_raw(s, cc::span{dispatch});
}

/// submits tasks calling a lambda "void f(unsigned i)" for each i from 0 to n
template <class F>
void submit_n(Sync& sync, F&& func, unsigned n, cc::allocator* scratch_alloc = cc::system_allocator)
{
    static_assert(std::is_invocable_v<F, unsigned>, "function must be invocable with index argument");
    static_assert(std::is_same_v<std::invoke_result_t<F, unsigned>, void>, "return must be void");

    auto tasks = cc::alloc_array<td::Task>::uninitialized(n, scratch_alloc);

    for (auto i = 0u; i < n; ++i)
    {
        tasks[i].initWithLambda([=] { func(i); });
    }

    submit_raw(sync, tasks);
}

/// submits tasks calling a lambda "void f(T& value)" for each element in the span
template <class T, class F>
void submit_each_ref(Sync& sync, F&& func, cc::span<T> vals, cc::allocator* scratch_alloc = cc::system_allocator)
{
    static_assert(std::is_invocable_v<F, T&>, "function must be invocable with element reference");
    static_assert(std::is_same_v<std::invoke_result_t<F, T&>, void>, "return must be void");

    auto tasks = cc::alloc_array<td::Task>::uninitialized(vals.size(), scratch_alloc);

    for (auto i = 0u; i < vals.size(); ++i)
    {
        tasks[i].initWithLambda([func, val_ptr = vals.data() + i] { func(*val_ptr); });
    }

    submit_raw(sync, tasks);
}

/// submits tasks calling a lambda "void f(T value)" for each element in the span
template <class T, class F>
void submit_each_copy(Sync& sync, F&& func, cc::span<T> vals, cc::allocator* scratch_alloc = cc::system_allocator)
{
    static_assert(std::is_invocable_v<F, T>, "function must be invocable with element copy");
    static_assert(std::is_same_v<std::invoke_result_t<F, T>, void>, "return must be void");

    auto tasks = cc::alloc_array<td::Task>::uninitialized(vals.size(), scratch_alloc);

    for (auto i = 0u; i < vals.size(); ++i)
    {
        tasks[i].initWithLambda([func, val_copy = vals[i]] { func(val_copy); });
    }

    submit_raw(sync, tasks);
}

/// submits tasks calling a lambda "void f(unsigned start, unsigned end)" for multiple batches over the range 0 to n
/// num_batches_max: maximum amount of batches to partition the range into
template <class F>
uint32_t submit_batched(Sync& sync, F&& func, uint32_t n, uint32_t num_batches_max = td::getNumLogicalCPUCores() * 4, cc::allocator* scratch_alloc = cc::system_allocator)
{
    static_assert(std::is_invocable_v<F, uint32_t, uint32_t>, "function must be invocable with batch start and end argument");

    auto batch_size = cc::int_div_ceil(n, num_batches_max);
    auto num_batches = cc::int_div_ceil(n, batch_size);

    CC_RUNTIME_ASSERT(num_batches <= num_batches_max && "programmer error");

    auto tasks = cc::alloc_array<td::Task>::uninitialized(num_batches, scratch_alloc);

    for (unsigned batch = 0u, start = 0u, end = cc::min(batch_size, n); //
         batch < num_batches;                                           //
         ++batch, start = batch * batch_size, end = cc::min((batch + 1) * batch_size, n))
    {
        tasks[batch].initWithLambda([=] { func(start, end); });
    }

    submit_raw(sync, tasks);
    return num_batches;
}

/// submits tasks calling a lambda "void f(unsigned start, unsigned end, unsigned batch_i)" for multiple batches over the range 0 to n
/// num_batches_max: maximum amount of batches to partition the range into
template <class F>
uint32_t submit_batched_n(Sync& sync, F&& func, uint32_t num_elements, uint32_t max_num_batches = td::getNumLogicalCPUCores() * 4, cc::allocator* scratch_alloc = cc::system_allocator)
{
    CC_ASSERT(is_scheduler_alive() && "scheduler not alive");
    if (!sync.handle.isValid())
    {
        sync.handle = acquireCounter();
    }

    return submit_batched_on_counter<F>(sync.handle, cc::forward<F>(func), num_elements, max_num_batches, scratch_alloc);
}

// Lambda - sync return variant
template <class F>
[[nodiscard]] Sync submit(F&& fun)
{
    static_assert(std::is_invocable_r_v<void, F>, "function must be invocable without arguments");

    Sync res;
    Task dispatch([=] { fun(); });
    submit_raw(res, cc::span{dispatch});
    return res;
}

// Pointer to member function with arguments - sync return variant
template <class F, class FObj, class... Args, cc::enable_if<std::is_member_function_pointer_v<F>> = true>
[[nodiscard]] Sync submit(F func, FObj& inst, Args&&... args)
{
    static_assert(std::is_invocable_r_v<void, F, FObj, Args...>, "function must be invocable with the given args");

    Sync res;

    // A lambda calling fun(args...), but moving the args instead of copying them into the lambda
    Task dispatch(
        [func, inst_ptr = &inst, tup = cc::tuple(cc::move(args)...)]
        {
            //
            cc::apply([&func, &inst_ptr](auto&&... args) { (inst_ptr->*func)(decltype(args)(args)...); }, tup);
        });

    submit_raw(res, cc::span{dispatch});
    return res;
}

// Lambda with arguments - sync return variant
template <class F, class... Args, cc::enable_if<std::is_invocable_v<F, Args...> && !std::is_member_function_pointer_v<F>> = true>
[[nodiscard]] Sync submit(F&& fun, Args&&... args)
{
    static_assert(std::is_invocable_r_v<void, F, Args...>, "function must be invocable with the given args");
    static_assert(!std::is_member_function_pointer_v<F>, "function must not be a function pointer");

    Sync res;

    // A lambda callung fun(args...), but moving the args instead of copying them into the lambda
    Task dispatch(
        [fun, tup = cc::tuple(cc::move(args)...)]
        {
            //
            cc::apply([&fun](auto&&... args) { fun(decltype(args)(args)...); }, tup);
        });

    submit_raw(res, cc::span{dispatch});
    return res;
}

// ==========
// Sync return variants

template <class F>
[[nodiscard]] Sync submit_n(F&& func, unsigned n)
{
    static_assert(std::is_invocable_v<F, unsigned>, "function must be invocable with index argument");
    static_assert(std::is_same_v<std::invoke_result_t<F, unsigned>, void>, "return must be void");
    Sync res;
    submit_n<F>(res, cc::forward<F>(func), n);
    return res;
}

template <class T, class F>
[[nodiscard]] Sync submit_each_ref(F&& func, cc::span<T> vals)
{
    static_assert(std::is_invocable_v<F, T&>, "function must be invocable with element reference");
    static_assert(std::is_same_v<std::invoke_result_t<F, T&>, void>, "return must be void");
    Sync res;
    submit_each_ref<T, F>(res, cc::forward<F>(func), vals);
    return res;
}

template <class T, class F>
[[nodiscard]] Sync submit_each_copy(F&& func, cc::span<T> vals)
{
    static_assert(std::is_invocable_v<F, T>, "function must be invocable with element value");
    static_assert(std::is_same_v<std::invoke_result_t<F, T>, void>, "return must be void");
    Sync res;
    submit_each_copy<T, F>(res, cc::forward<F>(func), vals);
    return res;
}

template <class F>
[[nodiscard]] Sync submit_batched(F&& func, unsigned n, unsigned num_batches_max = td::getNumLogicalCPUCores() * 4)
{
    static_assert(std::is_invocable_v<F, unsigned, unsigned>, "function must be invocable with batch start and end argument");
    static_assert(std::is_same_v<std::invoke_result_t<F, unsigned, unsigned>, void>, "return must be void");
    Sync res;
    submit_batched<F>(res, cc::forward<F>(func), n, num_batches_max);
    return res;
}

template <class F>
[[nodiscard]] Sync submit_batched_n(F&& func, unsigned n, unsigned num_batches_max = td::getNumLogicalCPUCores() * 4)
{
    static_assert(std::is_invocable_v<F, unsigned, unsigned, unsigned>, "function must be invocable with batch start, end, and index argument");
    static_assert(std::is_same_v<std::invoke_result_t<F, unsigned, unsigned, unsigned>, void>, "return must be void");
    Sync res;
    submit_batched_n<F>(res, cc::forward<F>(func), n, num_batches_max);
    return res;
}
}
