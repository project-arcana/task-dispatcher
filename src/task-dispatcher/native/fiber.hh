#pragma once

#include <clean-core/macros.hh>

#ifdef CC_OS_WINDOWS

#include <cstdint>
#include <cstdlib>

#include <clean-core/assert.hh>
#include <clean-core/native/win32_sanitized.hh>


#else

#ifdef _FORTIFY_SOURCE
// Suppress "longjmp causes uninitialized stack frame" error
#undef _FORTIFY_SOURCE
#endif

#include <setjmp.h>
#include <sys/times.h>
#include <ucontext.h>
#include <unistd.h>

#include <cstdint>
#include <cstdlib>
#include <cstring>

#endif

namespace td::native
{
// malloc passthrough, might want to capitalize on this later
struct allocator
{
    void* alloc(size_t size) const { return ::malloc(size); }

    void free(void* block) const { return ::free(block); }
};

inline auto constexpr default_alloc = allocator{};

#ifdef CC_OS_WINDOWS
// Thin native fiber abstraction for Win32

struct fiber_t
{
    void* native;
};

inline uint64_t get_tick_count() { return ::GetTickCount64(); }

inline void set_low_thread_prio() { ::SetThreadPriority(::GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL); }

inline void create_main_fiber(fiber_t& fib)
{
    fib.native = ::ConvertThreadToFiber(nullptr);
    CC_ASSERT(fib.native != nullptr && "Thread to fiber conversion failed");
}

inline void delete_main_fiber(fiber_t&) { ::ConvertFiberToThread(); }

inline void create_fiber(fiber_t& fib, void (*fiber_proc)(void*), void* ctx, size_t stack_size, allocator const& = default_alloc)
{
    fib.native = ::CreateFiber(stack_size, static_cast<LPFIBER_START_ROUTINE>(fiber_proc), ctx);
    CC_ASSERT(fib.native != nullptr && "Fiber creation failed");
}

inline void delete_fiber(fiber_t& fib, allocator const& = default_alloc) { ::DeleteFiber(fib.native); }

inline void switch_to_fiber(fiber_t fib, fiber_t) { ::SwitchToFiber(fib.native); }

// work-around for some versions of cygwin
extern "C" inline int __gxx_personality_v0() { return 0; }

#else
// Fiber API avoiding naive ucontext performance pitfalls for unix
// http://www.1024cores.net/home/lock-free-algorithms/tricks/fibers
// See licenses/

inline uint64_t get_tick_count()
{
    struct tms tms;
    return static_cast<uint64_t>(times(&tms) * (1000 / sysconf(_SC_CLK_TCK)));
}

inline void set_low_thread_prio() {}


struct fiber_t
{
    ::ucontext_t fib;
    ::jmp_buf jmp;
};

struct fiber_ctx_t
{
    void (*fnc)(void*);
    void* ctx;
    ::jmp_buf* cur;
    ::ucontext_t* prv;
};

static void fiber_start_fnc(void* p)
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

inline void create_main_fiber(fiber_t& fib) { std::memset(&fib, 0, sizeof(fib)); }

inline void delete_main_fiber(fiber_t&)
{
    // no op
}

inline void create_fiber(fiber_t& fib, void (*ufnc)(void*), void* uctx, size_t stack_size, allocator const& alloc = default_alloc)
{
    ::getcontext(&fib.fib);
    fib.fib.uc_stack.ss_sp = alloc.alloc(stack_size);
    fib.fib.uc_stack.ss_size = stack_size;
    fib.fib.uc_link = nullptr;
    ::ucontext_t tmp;
    fiber_ctx_t ctx = {ufnc, uctx, &fib.jmp, &tmp};
    ::makecontext(&fib.fib, reinterpret_cast<void (*)()>(fiber_start_fnc), 1, &ctx);
    ::swapcontext(&tmp, &fib.fib);
}

inline void delete_fiber(fiber_t& fib, allocator const& alloc = default_alloc) { alloc.free(fib.fib.uc_stack.ss_sp); }

inline void switch_to_fiber(fiber_t& fib, fiber_t& prv)
{
    if (::_setjmp(prv.jmp) == 0)
        ::_longjmp(fib.jmp, 1);
}

#endif
}
