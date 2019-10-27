#pragma once

#include <utility>

#include <clean-core/new.hh>
#include <clean-core/typedefs.hh>

#include <task-dispatcher/common/system_info.hh>

namespace td::container
{
namespace detail
{
struct callable_wrapper
{
    virtual ~callable_wrapper();
    virtual void call() = 0;
};

template <class T>
struct lambda_wrapper final : callable_wrapper
{
    static_assert(std::is_invocable_r_v<void, T>, "Lambda must have no arguments");
    explicit lambda_wrapper(T&& t) : _lambda(std::move(t)) {}
    void call() final override { _lambda(); }

    lambda_wrapper(lambda_wrapper const&) = delete;
    lambda_wrapper(lambda_wrapper&&) noexcept = delete;
    lambda_wrapper& operator=(lambda_wrapper const&) = delete;
    lambda_wrapper& operator=(lambda_wrapper&&) noexcept = delete;

private:
    T _lambda;
};

struct func_ptr_wrapper final : callable_wrapper
{
    using func_ptr_t = void (*)(void* _userdata);

    explicit func_ptr_wrapper(func_ptr_t func_ptr, void* userdata) : _func_ptr(func_ptr), _userdata(userdata) {}
    void call() final override;

    func_ptr_wrapper(func_ptr_wrapper const&) = delete;
    func_ptr_wrapper(func_ptr_wrapper&&) noexcept = delete;
    func_ptr_wrapper& operator=(func_ptr_wrapper const&) = delete;
    func_ptr_wrapper& operator=(func_ptr_wrapper&&) noexcept = delete;

private:
    func_ptr_t _func_ptr;
    void* _userdata;
};
}

// POD-struct storing tasks, from either a captureful lambda (of limited size), or a func ptr + void* userdata
// Additionally stores arbitrary metadata of a fixed size
//
// NOTE: If initialized from a lambda, cleanup() must get called exactly once on any copy of that instance, before the last of them is
// either destroyed or re-initialized. Zero calls could leak captured non-trivial-dtor types like std::vector, more than one call would read after
// free. This restriction allows task to be almost POD, but makes usage of this struct outside of rigid scenarios inadvisable.
struct task
{
public:
    using default_metadata_t = cc::uint16;
    static auto constexpr task_size = td::system::l1_cacheline_size;
    static auto constexpr metadata_size = sizeof(default_metadata_t);
    static auto constexpr usable_buffer_size = task_size - metadata_size;
    static_assert(sizeof(detail::func_ptr_wrapper) <= usable_buffer_size, "task is too small to hold func_ptr_wrapper");

private:
    cc::byte _buffer[task_size];

public:
    // == Constructors ==
    explicit task() = default;

    // From a lambda of the form void(void)
    template <class T, std::enable_if_t<std::is_invocable_r_v<void, T>, int> = 0>
    explicit task(T&& l)
    {
        lambda(std::forward<T>(l));
    }

    // From function pointer and userdata void*
    explicit task(detail::func_ptr_wrapper::func_ptr_t func_ptr, void* userdata = nullptr) { ptr(func_ptr, userdata); }

public:
    // == Deferred initialization ==

    // From a lambda of the form void()
    template <class T, std::enable_if_t<std::is_invocable_r_v<void, T>, int> = 0>
    void lambda(T&& l)
    {
        static_assert(sizeof(detail::lambda_wrapper<T>) <= usable_buffer_size, "Lambda capture exceeds task buffer size");
        new (cc::placement_new, static_cast<void*>(_buffer)) detail::lambda_wrapper<T>(std::forward<T>(l));
    }

    // From function pointer of the form void(void*) and userdata void*
    void ptr(detail::func_ptr_wrapper::func_ptr_t func_ptr, void* userdata = nullptr)
    {
        new (cc::placement_new, static_cast<void*>(_buffer)) detail::func_ptr_wrapper(func_ptr, userdata);
    }

public:
    // Write metadata into the reserved block
    template <class T = default_metadata_t>
    void set_metadata(T data)
    {
        static_assert(sizeof(T) <= metadata_size, "Metadata too large");
        *static_cast<T*>(static_cast<void*>(_buffer + usable_buffer_size)) = data;
    }

    // Read metadata from the reserved block
    template <class T = default_metadata_t>
    T get_metadata() const
    {
        static_assert(sizeof(T) <= metadata_size, "Metadata too large");
        return *static_cast<T const*>(static_cast<void const*>(_buffer + usable_buffer_size));
    }

    // Execute the contained task
    void execute() { (*reinterpret_cast<detail::callable_wrapper*>(_buffer)).call(); }

    // Clean up the possibly stored lambda, invalidating the task
    void cleanup() { (*reinterpret_cast<detail::callable_wrapper*>(_buffer)).~callable_wrapper(); }

    // Execute the contained task and clean it up afterwards (invalidates task)
    void execute_and_cleanup()
    {
        execute();
        cleanup();
    }
};
}
