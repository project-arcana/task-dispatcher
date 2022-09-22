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

void td::native::set_low_thread_prio() { ::SetThreadPriority(::GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL); }

void td::native::create_main_fiber(fiber_t& fib)
{
    fib.native = ::ConvertThreadToFiber(nullptr);
    CC_ASSERT(fib.native != nullptr && "Thread to fiber conversion failed");
}

void td::native::delete_main_fiber(fiber_t&) { ::ConvertFiberToThread(); }

void td::native::create_fiber(fiber_t& fib, void (*fiber_proc)(void*), void* ctx, size_t stack_size, cc::allocator*)
{
    fib.native = ::CreateFiber(stack_size, static_cast<LPFIBER_START_ROUTINE>(fiber_proc), ctx);
    CC_ASSERT(fib.native != nullptr && "Fiber creation failed");
}

void td::native::delete_fiber(fiber_t& fib, cc::allocator*) { ::DeleteFiber(fib.native); }

void td::native::switch_to_fiber(fiber_t fib, fiber_t) { ::SwitchToFiber(fib.native); }

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

void td::native::set_low_thread_prio() {}

void td::native::create_main_fiber(fiber_t& fib) { std::memset(&fib, 0, sizeof(fib)); }

void td::native::delete_main_fiber(fiber_t&)
{
    // no op
}

void td::native::create_fiber(fiber_t& fib, void (*ufnc)(void*), void* uctx, size_t stack_size, cc::allocator* alloc)
{
    ::getcontext(&fib.fib);
    fib.fib.uc_stack.ss_sp = reinterpret_cast<void*>(alloc->alloc(stack_size));
    fib.fib.uc_stack.ss_size = stack_size;
    fib.fib.uc_link = nullptr;
    ::ucontext_t tmp;
    fiber_ctx_t ctx = {ufnc, uctx, &fib.jmp, &tmp};
    ::makecontext(&fib.fib, reinterpret_cast<void (*)()>(fiber_start_fnc), 1, &ctx);
    ::swapcontext(&tmp, &fib.fib);
}

void td::native::delete_fiber(fiber_t& fib, cc::allocator* alloc) { alloc->free(fib.fib.uc_stack.ss_sp); }

void td::native::switch_to_fiber(fiber_t& fib, fiber_t& prv)
{
    if (::_setjmp(prv.jmp) == 0)
        ::_longjmp(fib.jmp, 1);
}


#endif
