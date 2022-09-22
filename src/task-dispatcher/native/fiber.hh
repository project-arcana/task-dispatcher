#pragma once

#include <stdint.h>

#include <clean-core/fwd.hh>
#include <clean-core/macros.hh>

#include <task-dispatcher/common/api.hh>

#ifndef CC_OS_WINDOWS

#ifdef _FORTIFY_SOURCE
// Suppress "longjmp causes uninitialized stack frame" error
#undef _FORTIFY_SOURCE
#endif

#include <setjmp.h>   // for jmp_buf
#include <ucontext.h> // for ucontext_t
#endif

namespace td::native
{
#ifdef CC_OS_WINDOWS
struct fiber_t
{
    void* native;
};
#else
struct fiber_t
{
    ::ucontext_t fib;
    ::jmp_buf jmp;
};
#endif

TD_API void set_low_thread_prio();

TD_API void create_main_fiber(fiber_t& fiber);

TD_API void delete_main_fiber(fiber_t& fiber);

TD_API void create_fiber(fiber_t& fiber, void (*pFiberEntry)(void*), void* pUserdata, size_t numBytesStack, cc::allocator* pAlloc);

TD_API void delete_fiber(fiber_t& fiber, cc::allocator* pAllocator);

TD_API void switch_to_fiber(fiber_t fiber, fiber_t);
}
