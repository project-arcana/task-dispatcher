#pragma once

#include <cstdint>
#include <utility> // std::move, std::forward, std::enable_if_t, std::is_invocable_r_v

#include <task-dispatcher/common/panic.hh>
#include <task-dispatcher/common/system_info.hh>

namespace td::container
{
namespace detail
{
struct CallableWrapper
{
    virtual ~CallableWrapper();
    virtual void call() = 0;
};

template <class T, std::enable_if_t<std::is_invocable_r_v<void, T>, int> = 0>
struct LambdaWrapper final : CallableWrapper
{
    explicit LambdaWrapper(T&& t) : mLambda(std::move(t)) {}
    void call() final override { mLambda(); }

private:
    T mLambda;
};

struct FuncPtrWrapper final : CallableWrapper
{
    using func_ptr_t = void (*)(void* userdata);
    explicit FuncPtrWrapper(func_ptr_t func_ptr, void* userdata) : func_ptr(func_ptr), userdata(userdata) {}
    void call() final override;

private:
    func_ptr_t func_ptr;
    void* userdata;
};
}

// POD-struct storing tasks, from either a captureful lambda (of limited size), or a func ptr + void* userdata
// Additionally stores arbitrary metadata of a fixed size
//
// NOTE: If initialized from a lambda, execute_and_cleanup must get called exactly once on any copy of that instance, before the last of them is
// either destroyed or re-initialized. Zero calls could leak captured non-trivial-dtor types like std::vector, more than one call would read after
// free. This restriction allows task to be almost POD, but makes usage of this struct outside of rigid scenarios inadvisable.
struct Task
{
private:
    using default_metadata_t = uint16_t;
    static auto constexpr task_size = td::system::l1_cacheline_size;
    static auto constexpr metadata_size = sizeof(default_metadata_t);
    static auto constexpr usable_buffer_size = task_size - metadata_size;
    static_assert(sizeof(detail::FuncPtrWrapper) <= usable_buffer_size, "task is too small to hold func_ptr_wrapper");

    union {
        uint64_t mFirstQword = 0;
        char mBuffer[task_size];
    };

public:
    // == Constructors ==

    explicit Task() = default;

    // From a lambda of the form void(void)
    template <class T, std::enable_if_t<std::is_invocable_r_v<void, T>, int> = 0>
    explicit Task(T&& l)
    {
        lambda(std::forward<T>(l));
    }

    // From function pointer and userdata void*
    explicit Task(detail::FuncPtrWrapper::func_ptr_t func_ptr, void* userdata = nullptr) { ptr(func_ptr, userdata); }

public:
    // == Deferred initialization ==

    // From a lambda of the form void(void)
    template <class T, std::enable_if_t<std::is_invocable_r_v<void, T>, int> = 0>
    void lambda(T&& l)
    {
        TD_DEBUG_PANIC_IF(isValid(), "Re-initialized task");
        static_assert(sizeof(detail::LambdaWrapper<T>) <= usable_buffer_size, "Lambda capture exceeds task buffer size");
        new (static_cast<void*>(mBuffer)) detail::LambdaWrapper(std::forward<T>(l));
    }

    // From function pointer and userdata void*
    void ptr(detail::FuncPtrWrapper::func_ptr_t func_ptr, void* userdata = nullptr)
    {
        TD_DEBUG_PANIC_IF(isValid(), "Re-initialized task");
        new (static_cast<void*>(mBuffer)) detail::FuncPtrWrapper(func_ptr, userdata);
    }

public:
    // Write metadata into the reserved block
    template <class T = default_metadata_t>
    void setMetadata(T data)
    {
        static_assert(sizeof(T) <= sizeof(metadata_size), "Metadata too large");
        *reinterpret_cast<T*>(static_cast<void*>(mBuffer + usable_buffer_size)) = data;
    }

    // Read metadata from the reserved block
    template <class T = default_metadata_t>
    T getMetadata() const
    {
        static_assert(sizeof(T) <= sizeof(metadata_size), "Metadata too large");
        return *reinterpret_cast<T const*>(static_cast<void const*>(mBuffer + usable_buffer_size));
    }

    // Execute the contained task
    void execute()
    {
        TD_DEBUG_PANIC_IF(!isValid(), "Executed uninitialized task");
        (*reinterpret_cast<detail::CallableWrapper*>(mBuffer)).call();
    }

    // Clean up the possibly stored lambda, invalidating the task
    void cleanup()
    {
        (*reinterpret_cast<detail::CallableWrapper*>(mBuffer)).~CallableWrapper();
#ifndef NDEBUG
        invalidate();
#endif
    }

    // Execute the contained task and clean it up afterwards (invalidates task)
    void executeAndCleanup()
    {
        execute();
        cleanup();
    }

private:
    bool isValid() const { return mFirstQword != 0; }
    void invalidate() { mFirstQword = 0; }
};
}
