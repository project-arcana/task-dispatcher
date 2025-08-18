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
struct thread_t;
struct event_t;

enum : uint32_t
{
    event_wait_infinite = 0xFFFFFFFF
};

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

using thread_start_func_t = uint32_t(__stdcall*)(void* arg);
#define TD_NATIVE_THREAD_FUNC_DECL uint32_t __stdcall
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

using thread_start_func_t = void* (*)(void* arg);
#define TD_NATIVE_THREAD_FUNC_DECL void*
#define TD_NATIVE_THREAD_FUNC_END return nullptr
#endif

TD_API bool createThread(size_t stack_size, thread_start_func_t start_routine, void* arg, thread_t* return_thread);

TD_API bool createThread(size_t stack_size, thread_start_func_t start_routine, void* arg, size_t core_affinity, thread_t* return_thread);

TD_API void endCurrentThread();

TD_API void joinThread(thread_t thread);

[[nodiscard]] TD_API thread_t getCurrentThread();

TD_API void setCurrentThreadCoreAffinity(size_t coreAffinity);

TD_API void setCurrentThreadLowPriority();

TD_API void setCurrentThreadDebugName(char const* pName);

TD_API void createEvent(event_t* event);

TD_API void destroyEvent(event_t& eventId);

// returns false on timeout
TD_API bool waitForEvent(event_t& eventId, uint32_t milliseconds);

TD_API void signalEvent(event_t& eventId);

TD_API void threadSleep(uint32_t milliseconds);

}
