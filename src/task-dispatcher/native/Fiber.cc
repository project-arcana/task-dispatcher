#include "Fiber.hh"

#include <clean-core/allocator.hh>
#include <clean-core/assert.hh>

#ifdef CC_OS_WINDOWS
#include <clean-core/native/win32_sanitized.hh>

// work-around for some versions of cygwin
extern "C" inline int __gxx_personality_v0() { return 0; }

#else

#include <setjmp.h>
#include <sys/times.h>
#include <ucontext.h>
#include <unistd.h>

#include <cstdlib>
#include <cstring>

#endif

#ifdef CC_OS_WINDOWS

// Thin native fiber abstraction for Win32

void td::native::createMainFiber(fiber_t* pOutFiber)
{
    CC_ASSERT(pOutFiber);

    pOutFiber->native = ::ConvertThreadToFiber(nullptr);
    CC_ASSERT(pOutFiber->native != nullptr && "Thread to fiber conversion failed");
}

void td::native::deleteMainFiber(fiber_t const&) { ::ConvertFiberToThread(); }

void td::native::createFiber(fiber_t* pOutFiber, void (*pFiberEntry)(void*), void* pUserdata, size_t numBytesStack, cc::allocator*)
{
    CC_ASSERT(pOutFiber);

    pOutFiber->native = ::CreateFiber(numBytesStack, static_cast<LPFIBER_START_ROUTINE>(pFiberEntry), pUserdata);
    CC_ASSERT(pOutFiber->native != nullptr && "Fiber creation failed");
}

void td::native::deleteFiber(fiber_t const& fib, cc::allocator*) { ::DeleteFiber(fib.native); }

void td::native::switchToFiber(fiber_t const& destFiber, fiber_t const& srcFiber)
{
    (void)srcFiber;
    ::SwitchToFiber(destFiber.native);
}

#else

// Fiber API avoiding naive ucontext performance pitfalls for unix
// http://www.1024cores.net/home/lock-free-algorithms/tricks/fibers
// See licenses/

namespace
{
struct fiber_ctx_t
{
    void (*fnc)(void*);
    void* ctx;
    ::jmp_buf* cur;
    ::ucontext_t* prv;
};

void fiber_start_fnc(void* p)
{
    fiber_ctx_t* ctx = reinterpret_cast<fiber_ctx_t*>(p);
    void (*volatile ufnc)(void*) = ctx->fnc;
    void* volatile uctx = ctx->ctx;
    if (::_setjmp(*ctx->cur) == 0)
    {
        ::ucontext_t tmp;
        ::swapcontext(&tmp, ctx->prv);
    }
    ufnc(uctx);
}
}

void td::native::createMainFiber(fiber_t* pOutFiber) { std::memset(pOutFiber, 0, sizeof(fiber_t)); }

void td::native::deleteMainFiber(fiber_t const&)
{
    // no op
}

void td::native::createFiber(fiber_t* pOutFiber, void (*ufnc)(void*), void* uctx, size_t stack_size, cc::allocator* alloc)
{
    ::getcontext(&pOutFiber->fib);
    pOutFiber->fib.uc_stack.ss_sp = reinterpret_cast<void*>(alloc->alloc(stack_size));
    pOutFiber->fib.uc_stack.ss_size = stack_size;
    pOutFiber->fib.uc_link = nullptr;
    ::ucontext_t tmp;
    fiber_ctx_t ctx = {ufnc, uctx, &pOutFiber->jmp, &tmp};
    ::makecontext(&pOutFiber->fib, reinterpret_cast<void (*)()>(fiber_start_fnc), 1, &ctx);
    ::swapcontext(&tmp, &pOutFiber->fib);
}

void td::native::deleteFiber(fiber_t const& fib, cc::allocator* alloc) { alloc->free(fib.fib.uc_stack.ss_sp); }

void td::native::switchToFiber(fiber_t const& destFiber, fiber_t const& srcFiber)
{
    if (::_setjmp(const_cast<fiber_t&>(srcFiber).jmp) == 0)
        ::_longjmp(const_cast<fiber_t&>(destFiber).jmp, 1);
}

#endif
