#pragma once

#include <task-dispatcher/td-lean.hh>

#include <task-dispatcher/BatchedSubmission.hh>
#include <task-dispatcher/CallableSubmission.hh>

// td.hh
// full task-dispatcher API (sink header)

namespace td
{
struct Sync;

template <class F, class... Args>
[[deprecated("renamed to td::submitCallable()")]] void submit(Sync& s, F&& fun, Args&&... args) = delete;
template <class F, class FObj, class... Args, cc::enable_if<std::is_member_function_pointer_v<F>> = true>
[[deprecated("renamed to td::submitMethod()")]] void submit(Sync& s, F func, FObj& inst, Args&&... args) = delete;
template <class F>
[[deprecated("renamed to td::submitNumbered()")]] void submit_n(Sync& sync, F&& func, unsigned n, cc::allocator* scratch_alloc = cc::system_allocator) = delete;
template <class T, class F>
[[deprecated("use td::submitBatchedOnArray()")]] void submit_each_ref(Sync& sync, F&& func, cc::span<T> vals, cc::allocator* scratch_alloc = cc::system_allocator)
    = delete;
template <class T, class F>
[[deprecated("use td::submitBatchedOnArray()")]] void submit_each_copy(Sync& sync, F&& func, cc::span<T> vals, cc::allocator* scratch_alloc = cc::system_allocator)
    = delete;
template <class F>
[[deprecated("use td::submitBatched()")]] uint32_t submit_batched(
    Sync& sync, F&& func, uint32_t n, uint32_t num_batches_max = td::getNumLogicalCPUCores() * 4, cc::allocator* scratch_alloc = cc::system_allocator)
    = delete;
template <class F>
[[deprecated("renamed to td::submitBatched()")]] uint32_t submit_batched_n(
    Sync& sync, F&& func, uint32_t num_elements, uint32_t max_num_batches = td::getNumLogicalCPUCores() * 4, cc::allocator* scratch_alloc = cc::system_allocator)
    = delete;

// Lambda - sync return variant
template <class F>
[[deprecated]] [[nodiscard]] Sync submit(F&& fun) = delete;

// Pointer to member function with arguments - sync return variant
template <class F, class FObj, class... Args, cc::enable_if<std::is_member_function_pointer_v<F>> = true>
[[deprecated]] [[nodiscard]] Sync submit(F func, FObj& inst, Args&&... args) = delete;

// Lambda with arguments - sync return variant
template <class F, class... Args, cc::enable_if<std::is_invocable_v<F, Args...> && !std::is_member_function_pointer_v<F>> = true>
[[deprecated]] [[nodiscard]] Sync submit(F&& fun, Args&&... args) = delete;

// ==========
// Sync return variants

template <class F>
[[deprecated]] [[nodiscard]] Sync submit_n(F&& func, unsigned n) = delete;

template <class T, class F>
[[deprecated]] [[nodiscard]] Sync submit_each_ref(F&& func, cc::span<T> vals) = delete;

template <class T, class F>
[[deprecated]] [[nodiscard]] Sync submit_each_copy(F&& func, cc::span<T> vals) = delete;

template <class F>
[[deprecated]] [[nodiscard]] Sync submit_batched(F&& func, unsigned n, unsigned num_batches_max = td::getNumLogicalCPUCores() * 4) = delete;

template <class F>
[[deprecated]] [[nodiscard]] Sync submit_batched_n(F&& func, unsigned n, unsigned num_batches_max = td::getNumLogicalCPUCores() * 4) = delete;
}
