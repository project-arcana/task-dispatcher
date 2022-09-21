#pragma once

#include <stdint.h>

#include <clean-core/macros.hh>

#include <task-dispatcher/common/api.hh>

#ifdef CC_OS_WINDOWS

#include <clean-core/native/win32_fwd.hh>

#else

// TODO: cut down on includes here
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <signal.h>
#include <unistd.h>

#endif

namespace td::native
{
#ifdef CC_OS_WINDOWS
struct thread_t
{
    ::HANDLE handle;
    ::DWORD id;
};

struct alignas(8) Win32CritSection
{
    char buf[40];
};

struct alignas(8) Win32ConditionVariable
{
    char buf[8];
};

struct event_t
{
    Win32ConditionVariable conditionVariable; // = CONDITION_VARIABLE
    Win32CritSection criticalSection;         // = CRITICAL_SECTION
};

enum : uint32_t
{
    event_wait_infinite = 0xFFFFFFFF
};

using thread_start_func_t = uint32_t(__stdcall*)(void* arg);
#define TD_NATIVE_THREAD_RETURN_TYPE uint32_t
#define TD_NATIVE_THREAD_FUNC_DECL TD_NATIVE_THREAD_RETURN_TYPE __stdcall
#define TD_NATIVE_THREAD_FUNC_END return 0

#else

struct thread_t
{
    pthread_t native;
};

struct event_t
{
    pthread_cond_t cond;
    pthread_mutex_t mutex;
};

[[maybe_unused]] constexpr static uint32_t event_wait_infinite = uint32_t(-1);

using thread_start_func_t = void* (*)(void* arg);
#define TD_NATIVE_THREAD_RETURN_TYPE void*
#define TD_NATIVE_THREAD_FUNC_DECL TD_NATIVE_THREAD_RETURN_TYPE
#define TD_NATIVE_THREAD_FUNC_END return nullptr

#endif

TD_API void set_current_thread_debug_name(int id);

TD_API bool create_thread(size_t stack_size, thread_start_func_t start_routine, void* arg, thread_t* return_thread);

TD_API bool create_thread(size_t stack_size, thread_start_func_t start_routine, void* arg, size_t core_affinity, thread_t* return_thread);

TD_API void end_current_thread();

TD_API void join_thread(thread_t thread);

[[nodiscard]] TD_API thread_t get_current_thread();

TD_API void set_current_thread_affinity(size_t coreAffinity);

TD_API void create_event(event_t* event);

TD_API void destroy_event(event_t& eventId);

// returns false on timeout
TD_API bool wait_for_event(event_t& eventId, uint32_t milliseconds);

TD_API void signal_event(event_t& eventId);

TD_API void thread_sleep(uint32_t milliseconds);

}
