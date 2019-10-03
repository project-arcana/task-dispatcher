#pragma once

// == Macro helpers ==
#define TD_STRINGIFY(n) TD_STRINGIFY_(n)
#define TD_STRINGIFY_(n) #n

#define TD_CONCAT(a, b) TD_CONCAT_(a, b)
#define TD_CONCAT_(a, b) a##b

#define TD_COUNT_OF(a) (sizeof(a) / sizeof(*a))

#define TD_UNUSED_EXPR(_expr_) void(sizeof((_expr_)))

#define TD_MACRO_JOIN_IMPL(arg1, arg2) arg1##arg2
#define TD_MACRO_JOIN(arg1, arg2) TD_MACRO_JOIN_IMPL(arg1, arg2)

#define TD_NOCOPY(_class_)                       \
    _class_(_class_ const&) = delete;            \
    _class_& operator=(_class_ const&) = delete; \
    _class_& operator=(_class_&&) = delete

#define TD_NOCOPYMOVE(_class_)                         \
    _class_(_class_ const& other) = delete;            \
    _class_(_class_&& other) noexcept = delete;        \
    _class_& operator=(_class_ const& other) = delete; \
    _class_& operator=(_class_&& other) noexcept = delete

// == Operating system ==
// TD_OS_WINDOWS, TD_OS_LINUX, TD_OS_OSX, or TD_OS_IOS
#if defined(WIN32) || defined(_WIN32) || defined(__WIN32)
#define TD_OS_WINDOWS
#elif defined(__APPLE__)
#include "TargetConditionals.h"
#if defined(TARGET_OS_MAC)
#define TD_OS_OSX
#elif defined(TARGET_OS_IPHONE)
#define TD_OS_IOS
#else
#error Unknown Apple platform
#endif
#elif defined(__linux__)
#define TD_OS_LINUX
#endif

// == Compiler ==
// TD_COMPILER_MSVC, TD_COMPILER_POSIX, or nothing
#ifdef _MSC_VER
#define TD_COMPILER_MSVC
#elif defined(__MINGW32__) || defined(__MINGW64__) || defined(__clang__) || defined(__GNUC__)
#define TD_COMPILER_POSIX
#endif

// == Compiler specifics ==
#ifdef TD_COMPILER_POSIX

#define TD_PRETTY_FUNC __PRETTY_FUNCTION__
#define TD_NOINLINE __attribute__((noinline))
#define TD_FORCEINLINE __attribute__((always_inline))
#define TD_LIKELY(x) __builtin_expect(bool(x), 1)
#define TD_UNLIKELY(x) __builtin_expect(bool(x), 0)
#define TD_COLD_FUNC __attribute__((cold))
#define TD_HOT_FUNC __attribute__((hot))

#elif defined TD_COMPILER_MSVC

#define TD_PRETTY_FUNC __FUNCTION__
#define TD_NOINLINE __declspec(noinline)
#define TD_FORCEINLINE __forceinline
#define TD_LIKELY(x) x
#define TD_UNLIKELY(x) x
#define TD_COLD_FUNC
#define TD_HOT_FUNC

#else
#error "Unsupported compiler"
#endif
