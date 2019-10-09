#pragma once

#include <utility> // std::move, std::forward, std::enable_if_t, std::is_invocable_r_v
#include <new>

#include <clean-core/typedefs.hh>
#include <clean-core/assert.hh>

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

    LambdaWrapper(LambdaWrapper const&) = delete;
    LambdaWrapper(LambdaWrapper&&) noexcept = delete;
    LambdaWrapper& operator=(LambdaWrapper const&) = delete;
    LambdaWrapper& operator=(LambdaWrapper&&) noexcept = delete;

private:
    T mLambda;
};

struct FuncPtrWrapper final : CallableWrapper
{
    using func_ptr_t = void (*)(void* userdata);

    explicit FuncPtrWrapper(func_ptr_t func_ptr, void* userdata) : func_ptr(func_ptr), userdata(userdata) {}
    void call() final override;

    FuncPtrWrapper(FuncPtrWrapper const&) = delete;
    FuncPtrWrapper(FuncPtrWrapper&&) noexcept = delete;
    FuncPtrWrapper& operator=(FuncPtrWrapper const&) = delete;
    FuncPtrWrapper& operator=(FuncPtrWrapper&&) noexcept = delete;

private:
    func_ptr_t func_ptr;
    void* userdata;
};
}

// POD-struct storing tasks, from either a captureful lambda (of limited size), or a func ptr + void* userdata
// Additionally stores arbitrary metadata of a fixed size
//
// NOTE: If initialized from a lambda, cleanup() must get called exactly once on any copy of that instance, before the last of them is
// either destroyed or re-initialized. Zero calls could leak captured non-trivial-dtor types like std::vector, more than one call would read after
// free. This restriction allows task to be almost POD, but makes usage of this struct outside of rigid scenarios inadvisable.
struct Task
{
private:
    using default_metadata_t = cc::uint16;
    static auto constexpr task_size = td::system::l1_cacheline_size;
    static auto constexpr metadata_size = sizeof(default_metadata_t);
    static auto constexpr usable_buffer_size = task_size - metadata_size;
    static_assert(sizeof(detail::FuncPtrWrapper) <= usable_buffer_size, "task is too small to hold func_ptr_wrapper");

    union {
        cc::uint64 mFirstQword = 0;
        cc::byte mBuffer[task_size];
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
        ASSERT(!isValid() && "Re-initialized task");
        static_assert(sizeof(detail::LambdaWrapper<T>) <= usable_buffer_size, "Lambda capture exceeds task buffer size");
        new (static_cast<void*>(mBuffer)) detail::LambdaWrapper(std::forward<T>(l));
    }

    // From function pointer and userdata void*
    void ptr(detail::FuncPtrWrapper::func_ptr_t func_ptr, void* userdata = nullptr)
    {
        ASSERT(!isValid() && "Re-initialized task");
        new (static_cast<void*>(mBuffer)) detail::FuncPtrWrapper(func_ptr, userdata);
    }

public:
    // Write metadata into the reserved block
    template <class T = default_metadata_t>
    void setMetadata(T data)
    {
        static_assert(sizeof(T) <= metadata_size, "Metadata too large");
        *static_cast<T*>(static_cast<void*>(mBuffer + usable_buffer_size)) = data;
    }

    // Read metadata from the reserved block
    template <class T = default_metadata_t>
    T getMetadata() const
    {
        static_assert(sizeof(T) <= metadata_size, "Metadata too large");
        return *static_cast<T const*>(static_cast<void const*>(mBuffer + usable_buffer_size));
    }

    // Execute the contained task
    void execute()
    {
        ASSERT(isValid() && "Executed uninitialized task");
        (*reinterpret_cast<detail::CallableWrapper*>(mBuffer)).call();
    }

    // Clean up the possibly stored lambda, invalidating the task
    void cleanup()
    {
        ASSERT(isValid() && "Cleaned up uninitialized task");
        (*reinterpret_cast<detail::CallableWrapper*>(mBuffer)).~CallableWrapper();
        ASSERT((invalidate(), true)); // Invalidation is only required for assert checks
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
