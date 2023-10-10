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
struct fiber_t;

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

TD_API void createMainFiber(fiber_t* pOutFiber);

TD_API void deleteMainFiber(fiber_t const& fiber);

TD_API void createFiber(fiber_t* pOutFiber, void (*pFiberEntry)(void*), void* pThreadStartstopFunc_Userdata, size_t numBytesStack, cc::allocator* pAlloc);

TD_API void deleteFiber(fiber_t const& fiber, cc::allocator* pAllocator);

TD_API void switchToFiber(fiber_t const& destFiber, fiber_t const& srcFiber);
}
