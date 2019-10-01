#pragma once

#include <common/system_info.hh>
#include <cstdint>
#include <utility> // std::move, std::forward, std::enable_if_t, std::is_invocable_r_v

namespace td::container
{
namespace detail
{
struct callable_wrapper
{
    virtual ~callable_wrapper();
    virtual void call() = 0;
};

template <class T, std::enable_if_t<std::is_invocable_r_v<void, T>, int> = 0>
struct lambda_wrapper final : callable_wrapper
{
    explicit lambda_wrapper(T&& t) : lambda(std::move(t)) {}
    void call() final override { lambda(); }

private:
    T lambda;
};

struct func_ptr_wrapper final : callable_wrapper
{
    using func_ptr_t = void (*)(void* userdata);
    explicit func_ptr_wrapper(func_ptr_t func_ptr, void* userdata) : func_ptr(func_ptr), userdata(userdata) {}
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
    static_assert(sizeof(detail::func_ptr_wrapper) <= usable_buffer_size, "task is too small to hold func_ptr_wrapper");

    union {
        uint64_t first_qword = 0;
        char buffer[task_size];
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
    explicit Task(detail::func_ptr_wrapper::func_ptr_t func_ptr, void* userdata = nullptr) { ptr(func_ptr, userdata); }

public:
    // == Deferred initialization ==

    // From a lambda of the form void(void)
    template <class T, std::enable_if_t<std::is_invocable_r_v<void, T>, int> = 0>
    void lambda(T&& l)
    {
        // KW_DEBUG_PANIC_IF(is_valid(), "Re-initialized task");
        static_assert(sizeof(detail::lambda_wrapper<T>) <= usable_buffer_size, "Lambda capture exceeds task buffer size");
        new (static_cast<void*>(buffer)) detail::lambda_wrapper(std::forward<T>(l));
    }

    // From function pointer and userdata void*
    void ptr(detail::func_ptr_wrapper::func_ptr_t func_ptr, void* userdata = nullptr)
    {
        // KW_DEBUG_PANIC_IF(is_valid(), "Re-initialized task");
        new (static_cast<void*>(buffer)) detail::func_ptr_wrapper(func_ptr, userdata);
    }

public:
    // Write metadata into the reserved block
    template <class T = default_metadata_t>
    void set_metadata(T data)
    {
        static_assert(sizeof(T) <= sizeof(metadata_size), "Metadata too large");
        *reinterpret_cast<T*>(static_cast<void*>(buffer + usable_buffer_size)) = data;
    }

    // Read metadata from the reserved block
    template <class T = default_metadata_t>
    T get_metadata() const
    {
        static_assert(sizeof(T) <= sizeof(metadata_size), "Metadata too large");
        return *reinterpret_cast<T const*>(static_cast<void const*>(buffer + usable_buffer_size));
    }

    // Execute the contained task
    void execute()
    {
        // KW_DEBUG_PANIC_IF(!is_valid(), "Executed uninitialized task");
        (*reinterpret_cast<detail::callable_wrapper*>(buffer)).call();
    }

    // Clean up the possibly stored lambda, invalidating the task
    void cleanup()
    {
        (*reinterpret_cast<detail::callable_wrapper*>(buffer)).~callable_wrapper();
#ifndef NDEBUG
        invalidate();
#endif
    }

    // Execute the contained task and clean it up afterwards (invalidates task)
    void execute_and_cleanup()
    {
        execute();
        cleanup();
    }

private:
    bool is_valid() const { return first_qword != 0; }
    void invalidate() { first_qword = 0; }
};
}
