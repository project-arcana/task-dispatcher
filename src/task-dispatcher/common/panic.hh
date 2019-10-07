#pragma once

#include <cc/macros.hh>

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
CC_COLD_FUNC CC_DONT_INLINE void panic(panic_info const& info);

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
#define TD_PANIC_IF(_cond_, _msg_) (CC_LIKELY(!(_cond_)) ? void(0) : ::td::detail::panic({_msg_, CC_PRETTY_FUNC, __FILE__, __LINE__}))

// TD_DEBUG_PANIC_IF is only active in debug configurations
//
// Basically cassert with more output and debugbreak
#ifndef NDEBUG
#define TD_DEBUG_PANIC_IF(_cond_, _msg_) TD_PANIC_IF(_cond_, _msg_)
#else
#define TD_DEBUG_PANIC_IF(_cond_, _msg_) CC_UNUSED(_cond_)
#endif
