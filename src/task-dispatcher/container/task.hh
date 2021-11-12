#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include <type_traits>

#include <clean-core/enable_if.hh>
#include <clean-core/forward.hh>
#include <clean-core/function_ptr.hh>
#include <clean-core/move.hh>
#include <clean-core/new.hh>

#ifdef TD_ALLOW_HEAP_TASKS
#include <clean-core/unique_ptr.hh>
#endif

#include <task-dispatcher/common/system_info.hh>

// the size of a task in cachelines (= 64B)
#define TD_FIXED_TASK_SIZE 2

namespace td
{
// POD-struct storing tasks, from either a lambda with limited size capture, or a func ptr + void* userdata
// also stores fixed size metadata
//
// NOTE: If initialized from a lambda, runTask() must get called exactly once on any copy of that instance, before the last
// of them is either destroyed or re-initialized. Zero calls could leak captured non-trivial-dtor types like std::vector, more than one call would
// read after free. This restriction allows task to be POD, but makes usage of this struct outside of rigid scenarios inadvisable.
struct Task
{
    using metadata_t = uint16_t;
    using function_ptr_t = void (*)(void*);
    using execute_and_cleanup_function_t = void(std::byte*);

    enum
    {
        ETask_Size = 64 * TD_FIXED_TASK_SIZE,
        ETask_UsableBufferSize = ETask_Size - sizeof(metadata_t) - sizeof(execute_and_cleanup_function_t*),
    };

    explicit Task() = default;

    // From a lambda of the form void(void)
    template <class T, cc::enable_if<std::is_invocable_r_v<void, T> && std::is_class_v<T>> = true>
    explicit Task(T&& l)
    {
        initWithLambda(cc::forward<T>(l));
    }

    // From function pointer and userdata void*
    explicit Task(cc::function_ptr<void(void*)> func_ptr, void* userdata = nullptr) { initWithFunction(func_ptr, userdata); }

    // From a lambda of the form void()
    template <class T, cc::enable_if<std::is_invocable_r_v<void, T> && std::is_class_v<T>> = true>
    void initWithLambda(T&& l)
    {
        // static_assert(std::is_class_v<T> && std::is_invocable_r_v<void, T>);

        if constexpr (sizeof(T) <= sizeof(mBuffer))
        {
            new (cc::placement_new, mBuffer) T(cc::move(l));

            mInvokingFunction = [](std::byte* buf)
            {
                T& ref = *reinterpret_cast<T*>(buf);
                ref.operator()();
                ref.~T();
            };
        }
        else
        {
#ifdef TD_ALLOW_HEAP_TASKS
            auto heap_lambda = cc::make_unique<T>(cc::move(l));
            new (cc::placement_new, mBuffer) cc::unique_ptr<T>(cc::move(heap_lambda));

            mInvokingFunction = [](std::byte* buf)
            {
                cc::unique_ptr<T>& ref = *reinterpret_cast<cc::unique_ptr<T>*>(buf);
                ref->operator()();
                ref.~unique_ptr<T>();
            };
#else
            static_assert(sizeof(T) == 0, "Lambda capture is too large, enable TD_ALLOW_HEAP_TASKS, reduce capture size or increase "
                                          "TD_FIXED_TASK_SIZE");
#endif
        }
    }

    // From function pointer of the form void(void*) and userdata void*
    void initWithFunction(cc::function_ptr<void(void*)> func_ptr, void* userdata = nullptr)
    {
        memcpy(mBuffer, &func_ptr, sizeof(func_ptr));
        memcpy(mBuffer + sizeof(func_ptr), &userdata, sizeof(userdata));

        mInvokingFunction = [](std::byte* buf)
        {
            cc::function_ptr<void(void*)> readFuncPtr = nullptr;
            memcpy(&readFuncPtr, buf, sizeof(readFuncPtr));

            void* readUserdata = nullptr;
            memcpy(&readUserdata, buf + sizeof(readFuncPtr), sizeof(readUserdata));

            readFuncPtr(readUserdata);
        };
    }

    // Execute the contained task and clean it up afterwards (invalidates task)
    void runTask() { mInvokingFunction(mBuffer); }

private:
    alignas(alignof(std::max_align_t)) std::byte mBuffer[ETask_UsableBufferSize];

public:
    metadata_t mMetadata;

private:
    execute_and_cleanup_function_t* mInvokingFunction;
};

static_assert(sizeof(td::Task) == td::l1_cacheline_size * TD_FIXED_TASK_SIZE, "task type is unexpectedly large");
static_assert(alignof(td::Task) == alignof(std::max_align_t), "task type risks misalignment");
static_assert(std::is_trivial_v<td::Task>, "task is not trivial");
}
