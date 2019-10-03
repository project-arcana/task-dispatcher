#pragma once

#include "macros.hh"

namespace td::detail
{
struct panic_info
{
    char const* message;
    char const* function;
    char const* file;
    int line;
};

// Triggers a debug break, then crashes the program
TD_COLD_FUNC TD_NOINLINE void panic(panic_info const& info);

void debugbreak();
}

// TD_PANIC_IF crashes the application if the condition is met, in every configuration
//
// While it is always active, use it as if it was an optional assert, so:
//
// auto success = action();
// TD_PANIC_IF(!success, "Action failed");
//
// instead of
//
// TD_PANIC_IF(!action(), "Action failed");
//
#define TD_PANIC_IF(_cond_, _msg_) (TD_LIKELY(!(_cond_)) ? void(0) : ::td::detail::panic({_msg_, TD_PRETTY_FUNC, __FILE__, __LINE__}))

// TD_DEBUG_PANIC_IF is only active in debug configurations
//
// Basically cassert with more output and debugbreak
#ifndef NDEBUG
#define TD_DEBUG_PANIC_IF(_cond_, _msg_) TD_PANIC_IF(_cond_, _msg_)
#else
#define TD_DEBUG_PANIC_IF(_cond_, _msg_) TD_UNUSED_EXPR(_cond_)
#endif
