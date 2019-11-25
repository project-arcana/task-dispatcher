#pragma once

#include <type_traits>

#include <clean-core/enable_if.hh>
#include <clean-core/move.hh>
#include <clean-core/new.hh>
#include <clean-core/typedefs.hh>

#include <task-dispatcher/common/system_info.hh>

namespace td::container
{
// POD-struct storing tasks, from either a captureful lambda (of limited size), or a func ptr + void* userdata
// Additionally stores arbitrary metadata of a fixed size
//
// NOTE: If initialized from a lambda, execute_and_cleanup() must get called exactly once on any copy of that instance, before the last
// of them is either destroyed or re-initialized. Zero calls could leak captured non-trivial-dtor types like std::vector, more than one call would
// read after free. This restriction allows task to be POD, but makes usage of this struct outside of rigid scenarios inadvisable.
struct task
{
public:
    using default_metadata_t = cc::uint16;
    using function_ptr_t = void (*)(void*);
    using execute_and_cleanup_function_t = void(cc::byte*);

    static auto constexpr task_size = td::system::l1_cacheline_size;
    static constexpr auto usable_buffer_size = task_size - sizeof(default_metadata_t) - sizeof(execute_and_cleanup_function_t*);
    using buffer_t = cc::byte[usable_buffer_size];

private:
    alignas(alignof(std::max_align_t)) buffer_t _buffer;
    default_metadata_t _metadata;
    execute_and_cleanup_function_t* _exec_cleanup_func;

public:
    // == Constructors ==
    explicit task() = default;

    // From a lambda of the form void(void)
    template <class T, cc::enable_if<std::is_invocable_r_v<void, T> && std::is_class_v<T>> = true>
    explicit task(T&& l)
    {
        lambda(std::forward<T>(l));
    }

    // From function pointer and userdata void*
    explicit task(function_ptr_t func_ptr, void* userdata = nullptr) { ptr(func_ptr, userdata); }

public:
    // == Deferred initialization ==

    // From a lambda of the form void()
    template <class T, cc::enable_if<std::is_invocable_r_v<void, T> && std::is_class_v<T>> = true>
    void lambda(T&& l)
    {
        static_assert(std::is_class_v<T> && std::is_invocable_r_v<void, T>);
        static_assert(sizeof(T) <= sizeof(buffer_t), "Lambda is too large for task buffer");

        new (cc::placement_new, _buffer) T(cc::move(l));

        _exec_cleanup_func = [](cc::byte* buf) {
            T& ref = *reinterpret_cast<T*>(buf);
            ref.operator()();
            ref.~T();
        };
    }

    // From function pointer of the form void(void*) and userdata void*
    void ptr(function_ptr_t func_ptr, void* userdata = nullptr)
    {
        using fptr_t = function_ptr_t;

        new (cc::placement_new, _buffer) fptr_t(func_ptr);
        new (cc::placement_new, _buffer + sizeof(fptr_t)) void*(userdata);

        _exec_cleanup_func = [](cc::byte* buf) {
            fptr_t& ref = *reinterpret_cast<fptr_t*>(buf);
            void*& ref_arg = *reinterpret_cast<void**>(buf + sizeof(fptr_t));
            ref(ref_arg);
        };
    }

public:
    // Write metadata into the reserved block
    void set_metadata(default_metadata_t data) { _metadata = data; }

    // Read metadata from the reserved block
    default_metadata_t get_metadata() const { return _metadata; }

    // Execute the contained task and clean it up afterwards (invalidates task)
    void execute_and_cleanup() { _exec_cleanup_func(_buffer); }
};
}
