#pragma once

#include <cstdint>

#include <clean-core/assert.hh>
#include <clean-core/defer.hh>
#include <clean-core/macros.hh>

#ifdef CC_OS_WINDOWS

#include <atomic>

#include <process.h>
#include <clean-core/native/win32_sanitized.hh>

#else

#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <unistd.h>

#endif

namespace td::native
{
// Thin native thread abstraction
#ifdef CC_OS_WINDOWS
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

inline bool create_thread(size_t stack_size, thread_start_func_t start_routine, void* arg, thread_t* return_thread)
{
    CC_ASSERT(stack_size <= unsigned(-1));
    auto const handle = reinterpret_cast<::HANDLE>(::_beginthreadex(nullptr, unsigned(stack_size), start_routine, arg, 0U, nullptr));
    return_thread->handle = handle;
    return_thread->id = ::GetThreadId(handle);

    return handle != nullptr;
}

inline bool create_thread(size_t stack_size, thread_start_func_t start_routine, void* arg, size_t core_affinity, thread_t* return_thread)
{
    CC_ASSERT(stack_size <= unsigned(-1));
    auto const handle = reinterpret_cast<::HANDLE>(::_beginthreadex(nullptr, unsigned(stack_size), start_routine, arg, CREATE_SUSPENDED, nullptr));

    if (handle == nullptr)
    {
        return false;
    }

    ::DWORD_PTR const mask = 1ULL << core_affinity;
    ::SetThreadAffinityMask(handle, mask);

    return_thread->handle = handle;
    return_thread->id = ::GetThreadId(handle);
    ::ResumeThread(handle);

    return true;
}

inline void end_current_thread() { ::_endthreadex(0); }

inline void join_thread(thread_t thread) { ::WaitForSingleObject(thread.handle, INFINITE); }

[[nodiscard]] inline thread_t get_current_thread() { return thread_t{::GetCurrentThread(), ::GetCurrentThreadId()}; }

inline void set_current_thread_affinity(size_t coreAffinity) { ::SetThreadAffinityMask(::GetCurrentThread(), 1ULL << coreAffinity); }

inline void create_native_event(event_t* event)
{
    event->event = ::CreateEvent(nullptr, TRUE, FALSE, nullptr);
    event->count_waiters = 0;
}

inline void close_event(event_t eventId) { ::CloseHandle(eventId.event); }

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

    CC_ASSERT(retval != WAIT_FAILED && prev != 0 && "Failed to wait on native event");
}

inline void signal_event(event_t eventId) { ::SetEvent(eventId.event); }

inline void thread_sleep(uint32_t milliseconds) { ::Sleep(milliseconds); }
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
constexpr static uint32_t event_wait_infinite = uint32_t(-1);

using thread_start_func_t = void* (*)(void* arg);
#define TD_NATIVE_THREAD_RETURN_TYPE void*
#define TD_NATIVE_THREAD_FUNC_DECL TD_NATIVE_THREAD_RETURN_TYPE
#define TD_NATIVE_THREAD_FUNC_END return nullptr

namespace detail
{
[[nodiscard]] inline size_t get_page_size()
{
    auto res = sysconf(_SC_PAGE_SIZE);
    CC_ASSERT(res > 0 && "Error retrieving page size");
    return size_t(res);
}

[[nodiscard]] inline size_t round_to_page_size(size_t stack_size)
{
    auto const page_size = get_page_size();

    auto const remainder = stack_size % page_size;
    if (remainder == 0)
        return stack_size;
    else
        return stack_size + page_size - remainder;
}

inline bool set_stack_size(pthread_attr_t& thread_attributes, size_t stack_size)
{
    size_t default_stack_size = size_t(-1);
    pthread_attr_getstacksize(&thread_attributes, &default_stack_size);
    CC_ASSERT(default_stack_size >= PTHREAD_STACK_MIN);

    // ceil the stack size to the default stack size, otherwise pthread_create can fail with EINVAL (undocumented)
    stack_size = stack_size < default_stack_size ? default_stack_size : stack_size;

    auto const setstack_res = pthread_attr_setstacksize(&thread_attributes, stack_size);
    if (setstack_res != 0)
    {
        // on some systems, stack size must be a multiple of the system page size, retry
        auto const setstack_res_retry = pthread_attr_setstacksize(&thread_attributes, detail::round_to_page_size(stack_size));

        if (setstack_res_retry != 0)
            // Retry failed
            return false;
    }

    return true;
}
}

inline bool create_thread(size_t stack_size, thread_start_func_t start_routine, void* arg, thread_t* return_thread)
{
    pthread_attr_t thread_attr;
    pthread_attr_init(&thread_attr);
    CC_DEFER { pthread_attr_destroy(&thread_attr); };

    if (!detail::set_stack_size(thread_attr, stack_size))
        return false;

    auto const create_res = pthread_create(&return_thread->native, &thread_attr, start_routine, arg);
    return create_res == 0;
}

inline bool create_thread(size_t stack_size, thread_start_func_t start_routine, void* arg, size_t core_affinity, thread_t* return_thread)
{
    pthread_attr_t thread_attr;
    pthread_attr_init(&thread_attr);
    CC_DEFER { pthread_attr_destroy(&thread_attr); };

    if (!detail::set_stack_size(thread_attr, stack_size))
        return false;

#if defined(CC_OS_LINUX)
    // Set core affinity
    cpu_set_t cpu_set;
    CPU_ZERO(&cpu_set);
    CPU_SET(core_affinity, &cpu_set);
    auto const setaffinity_res = pthread_attr_setaffinity_np(&thread_attr, sizeof(cpu_set_t), &cpu_set);
    if (setaffinity_res != 0)
        return false;
#else
    // TODO: OSX and MinGW Thread Affinity
    (void)coreAffinity;
#endif

    auto const create_res = pthread_create(&return_thread->native, &thread_attr, start_routine, arg);
    return create_res == 0;
}

[[noreturn]] inline void end_current_thread() { pthread_exit(nullptr); }

inline void join_thread(thread_t thread) { pthread_join(thread.native, nullptr); }

[[nodiscard]] inline thread_t get_current_thread() { return {pthread_self()}; }

inline void set_current_thread_affinity(size_t coreAffinity)
{
// TODO: OSX and MinGW Thread Affinity
#if defined(CC_OS_LINUX)
    cpu_set_t cpuSet;
    CPU_ZERO(&cpuSet);
    CPU_SET(coreAffinity, &cpuSet);

    pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuSet);
#else
    (void)coreAffinity;
#endif
}

inline void create_native_event(event_t* event) { *event = {PTHREAD_COND_INITIALIZER, PTHREAD_MUTEX_INITIALIZER}; }

inline void close_event(event_t /*eventId*/)
{
    // No op
}

inline void wait_for_event(event_t& eventId, int64_t milliseconds)
{
    pthread_mutex_lock(&eventId.mutex);

    constexpr int64_t mills_in_sec = 1000;

    if (milliseconds == event_wait_infinite)
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
