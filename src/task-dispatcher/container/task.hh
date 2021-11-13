#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <type_traits>

#include <clean-core/enable_if.hh>
#include <clean-core/forward.hh>
#include <clean-core/function_ptr.hh>
#include <clean-core/move.hh>
#include <clean-core/new.hh>

#ifdef TD_ALLOW_HEAP_TASKS
#include <clean-core/allocator.hh>
#endif

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
struct alignas(td::l1_cacheline_size* TD_FIXED_TASK_SIZE) Task
{
    enum
    {
        ETask_Size = td::l1_cacheline_size * TD_FIXED_TASK_SIZE,
        ETask_UsableBufferSize = ETask_Size - sizeof(uint16_t) - sizeof(void*),
    };

    explicit Task() = default;

    // From a lambda of the form void(void)
    template <class T, cc::enable_if<std::is_invocable_r_v<void, T> && std::is_class_v<T>> = true>
    explicit Task(T&& lambda, cc::allocator* pHeapTaskAlloc = cc::system_allocator)
    {
        initWithLambda(cc::forward<T>(lambda), pHeapTaskAlloc);
    }

    // From function pointer and userdata void*
    explicit Task(cc::function_ptr<void(void*)> func_ptr, void* pUserdata = nullptr) { initWithFunction(func_ptr, pUserdata); }

    // From a lambda of the form void()
    template <class T, cc::enable_if<std::is_invocable_r_v<void, T> && std::is_class_v<T>> = true>
    void initWithLambda(T&& lambda, cc::allocator* pHeapTaskAlloc = cc::system_allocator)
    {
        // static_assert(std::is_class_v<T> && std::is_invocable_r_v<void, T>);

        if constexpr (sizeof(T) <= sizeof(mBuffer))
        {
            static_assert(alignof(T) <= alignof(Task), "Lambda misaligned");

            T* const pWrittenLambda = new (cc::placement_new, mBuffer) T(cc::move(lambda));
            // assert below should never happen
            CC_ASSERT(reinterpret_cast<std::byte*>(pWrittenLambda) == mBuffer && "Lambda misaligned");

            mInvokingFunction = [](std::byte* pBuffer)
            {
                // recover the lambda from the buffer (only this function "knows" its type T)
                T* const pLambda = reinterpret_cast<T*>(pBuffer);
                // run the lambda
                pLambda->operator()();
                // destroy the lambda (relevant for non-POD per-value captures)
                pLambda->~T();
            };
        }
        else
        {
#ifdef TD_ALLOW_HEAP_TASKS
            CC_ASSERT(pHeapTaskAlloc != nullptr && "If TD_ALLOW_HEAP_TASKS is active, the allocator must be specified");

            // allocate a new lambda, move constructed from the given one
            T* const pHeapLambda = pHeapTaskAlloc->new_t<T>(cc::move(lambda));
            // write the pointer
            memcpy(mBuffer, &pHeapLambda, sizeof(pHeapLambda));
            // write the pointer to the allocator
            memcpy(mBuffer + sizeof(pHeapLambda), &pHeapTaskAlloc, sizeof(pHeapTaskAlloc));

            mInvokingFunction = [](std::byte* pBuffer)
            {
                // read the lambda pointer from the buffer
                T* pReadLambda = nullptr;
                memcpy(&pReadLambda, pBuffer, sizeof(pReadLambda));

                // read the allocator pointer from the buffer
                cc::allocator* pReadAlloc = nullptr;
                memcpy(&pReadAlloc, pBuffer + sizeof(pReadLambda), sizeof(pReadAlloc));

                // run the lambda
                pReadLambda->operator()();

                // delete the lambda (relevant for non-POD per-value captures) and free it via the given allocator
                pReadAlloc->delete_t<T>(pReadLambda);
            };
#else
            static_assert(sizeof(T) == 0, "Lambda capture is too large, enable TD_ALLOW_HEAP_TASKS, reduce capture size or increase "
                                          "TD_FIXED_TASK_SIZE");
#endif
        }
    }

    // From function pointer of the form void(void*) and userdata void*
    void initWithFunction(cc::function_ptr<void(void*)> pFunction, void* pUserdata = nullptr)
    {
        memcpy(mBuffer, &pFunction, sizeof(pFunction));
        memcpy(mBuffer + sizeof(pFunction), &pUserdata, sizeof(pUserdata));

        mInvokingFunction = [](std::byte* pBuffer)
        {
            // read the function pointer from the buffer
            cc::function_ptr<void(void*)> pReadFunction = nullptr;
            memcpy(&pReadFunction, pBuffer, sizeof(pReadFunction));

            // read the userdata pointer from the buffer
            void* pReadUserdata = nullptr;
            memcpy(&pReadUserdata, pBuffer + sizeof(pReadFunction), sizeof(pReadUserdata));

            // call the function pointer
            pReadFunction(pReadUserdata);
        };
    }

    // Execute the contained task and clean it up afterwards (invalidates task)
    void runTask() { mInvokingFunction(mBuffer); }

private:
    alignas(alignof(std::max_align_t)) std::byte mBuffer[ETask_UsableBufferSize];

public:
    uint16_t mMetadata;

private:
    cc::function_ptr<void(std::byte*)> mInvokingFunction;
};
}
