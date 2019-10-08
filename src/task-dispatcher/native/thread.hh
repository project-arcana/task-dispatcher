#pragma once

#include <cstdint>

#ifdef _WIN32

#include <atomic>

#include <task-dispatcher/common/win32_sanitized.hh>
#include <process.h>

#include <cc/assert.hh>

#else

#include <pthread.h>
#include <unistd.h>

#endif

namespace td::native
{
// Thin native thread abstraction
#ifdef _WIN32
struct thread_t
{
    ::HANDLE handle;
    ::DWORD id;
};

struct event_t
{
    ::HANDLE event;
    std::atomic_ulong count_waiters;
};

using thread_start_func_t = uint32_t(__stdcall*)(void* arg);
#define TD_NATIVE_THREAD_RETURN_TYPE uint32_t
#define TD_NATIVE_THREAD_FUNC_DECL TD_NATIVE_THREAD_RETURN_TYPE __stdcall
#define TD_NATIVE_THREAD_FUNC_END return 0

inline bool create_thread(uint32_t stackSize, thread_start_func_t startRoutine, void* arg, thread_t* returnThread)
{
    auto const handle = reinterpret_cast<::HANDLE>(::_beginthreadex(nullptr, stackSize, startRoutine, arg, 0U, nullptr));
    returnThread->handle = handle;
    returnThread->id = ::GetThreadId(handle);

    return handle != nullptr;
}

inline bool create_thread(uint32_t stackSize, thread_start_func_t startRoutine, void* arg, size_t coreAffinity, thread_t* returnThread)
{
    auto const handle = reinterpret_cast<::HANDLE>(::_beginthreadex(nullptr, stackSize, startRoutine, arg, CREATE_SUSPENDED, nullptr));

    if (handle == nullptr)
    {
        return false;
    }

    ::DWORD_PTR const mask = 1ULL << coreAffinity;
    ::SetThreadAffinityMask(handle, mask);

    returnThread->handle = handle;
    returnThread->id = ::GetThreadId(handle);
    ::ResumeThread(handle);

    return true;
}

inline void end_current_thread()
{
    ::_endthreadex(0);
}

inline void join_thread(thread_t thread)
{
    ::WaitForSingleObject(thread.handle, INFINITE);
}

inline thread_t get_current_thread()
{
    return thread_t{::GetCurrentThread(), ::GetCurrentThreadId()};
}

inline void set_current_thread_affinity(size_t coreAffinity)
{
    ::SetThreadAffinityMask(::GetCurrentThread(), 1ULL << coreAffinity);
}

inline void create_native_event(event_t* event)
{
    event->event = ::CreateEvent(nullptr, TRUE, FALSE, nullptr);
    event->count_waiters = 0;
}

inline void close_event(event_t eventId)
{
    ::CloseHandle(eventId.event);
}

inline void wait_for_event(event_t& eventId, uint32_t milliseconds)
{
    eventId.count_waiters.fetch_add(1U);

    ::DWORD const retval = ::WaitForSingleObject(eventId.event, milliseconds);
    uint32_t const prev = eventId.count_waiters.fetch_sub(1U);

    if (prev == 1)
    {
        // we were the last to awaken, so reset event.
        ::ResetEvent(eventId.event);
    }

    ASSERT(retval != WAIT_FAILED && prev != 0 && "Failed to wait on native event");
}

inline void signal_event(event_t eventId)
{
    ::SetEvent(eventId.event);
}

inline void thread_sleep(uint32_t milliseconds)
{
    ::Sleep(milliseconds);
}
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
constexpr static uint32_t EVENTWAIT_INFINITE = uint32_t(-1);

using thread_start_func_t = void* (*)(void* arg);
#define TD_NATIVE_THREAD_RETURN_TYPE void*
#define TD_NATIVE_THREAD_FUNC_DECL TD_NATIVE_THREAD_RETURN_TYPE
#define TD_NATIVE_THREAD_FUNC_END return nullptr

inline bool create_thread(uint32_t stackSize, thread_start_func_t startRoutine, void* arg, thread_t* returnThread)
{
    pthread_attr_t threadAttr;
    pthread_attr_init(&threadAttr);

    // Set stack size
    pthread_attr_setstacksize(&threadAttr, stackSize);

    int success = pthread_create(&returnThread->native, &threadAttr, startRoutine, arg);

    // Cleanup
    pthread_attr_destroy(&threadAttr);

    return success == 0;
}

inline bool create_thread(uint32_t stackSize, thread_start_func_t startRoutine, void* arg, size_t coreAffinity, thread_t* returnThread)
{
    pthread_attr_t threadAttr;
    pthread_attr_init(&threadAttr);

    // Set stack size
    pthread_attr_setstacksize(&threadAttr, stackSize);

// TODO: OSX and MinGW Thread Affinity
#if defined(KW_OS_LINUX)
    // Set core affinity
    cpu_set_t cpuSet;
    CPU_ZERO(&cpuSet);
    CPU_SET(coreAffinity, &cpuSet);
    pthread_attr_setaffinity_np(&threadAttr, sizeof(cpu_set_t), &cpuSet);
#else
    (void)coreAffinity;
#endif

    int success = pthread_create(&returnThread->native, &threadAttr, startRoutine, arg);

    // Cleanup
    pthread_attr_destroy(&threadAttr);

    return success == 0;
}

[[noreturn]] inline void end_current_thread()
{
    pthread_exit(nullptr);
}

inline void join_thread(thread_t thread)
{
    pthread_join(thread.native, nullptr);
}

inline thread_t get_current_thread()
{
    return {pthread_self()};
}

inline void set_current_thread_affinity(size_t coreAffinity)
{
// TODO: OSX and MinGW Thread Affinity
#if defined(KW_OS_LINUX)
    cpu_set_t cpuSet;
    CPU_ZERO(&cpuSet);
    CPU_SET(coreAffinity, &cpuSet);

    pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuSet);
#else
    (void)coreAffinity;
#endif
}

inline void create_native_event(event_t* event)
{
    *event = {PTHREAD_COND_INITIALIZER, PTHREAD_MUTEX_INITIALIZER};
}

inline void close_event(event_t /*eventId*/)
{
    // No op
}

inline void wait_for_event(event_t& eventId, int64_t milliseconds)
{
    pthread_mutex_lock(&eventId.mutex);

    constexpr int64_t mills_in_sec = 1000;

    if (milliseconds == EVENTWAIT_INFINITE)
    {
        pthread_cond_wait(&eventId.cond, &eventId.mutex);
    }
    else
    {
        timespec waittime{};
        waittime.tv_sec = milliseconds / mills_in_sec;
        milliseconds -= waittime.tv_sec * mills_in_sec;
        waittime.tv_nsec = milliseconds * mills_in_sec;
        pthread_cond_timedwait(&eventId.cond, &eventId.mutex, &waittime);
    }

    pthread_mutex_unlock(&eventId.mutex);
}

inline void signal_event(event_t eventId)
{
    pthread_mutex_lock(&eventId.mutex);
    pthread_cond_broadcast(&eventId.cond);
    pthread_mutex_unlock(&eventId.mutex);
}

inline void thread_sleep(uint32_t milliseconds)
{
    usleep(milliseconds * 1000);

    // usleep is deprecated, but universally still present
    // the successor would be nanosleep, requiring <time.h>
    // it is available if _POSIX_C_SOURCE >= 199309L
    //    struct timespec ts;
    //    ts.tv_sec = milliseconds / 1000;
    //    ts.tv_nsec = (milliseconds % 1000) * 1000000;
    //    nanosleep(&ts, NULL);
}
#endif
}
