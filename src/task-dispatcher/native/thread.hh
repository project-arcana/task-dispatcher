#pragma once

#include <cstdint>

#include <clean-core/assert.hh>
#include <clean-core/defer.hh>
#include <clean-core/macros.hh>

#ifdef CC_OS_WINDOWS

#include <process.h>
#include <clean-core/native/win32_sanitized.hh>

#else

#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <signal.h>
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
    ::CONDITION_VARIABLE cv;
    ::CRITICAL_SECTION crit_sec;
};

[[maybe_unused]] constexpr static uint32_t event_wait_infinite = INFINITE;

using thread_start_func_t = uint32_t(__stdcall*)(void* arg);
#define TD_NATIVE_THREAD_RETURN_TYPE uint32_t
#define TD_NATIVE_THREAD_FUNC_DECL TD_NATIVE_THREAD_RETURN_TYPE __stdcall
#define TD_NATIVE_THREAD_FUNC_END return 0

inline bool create_thread(size_t stack_size, thread_start_func_t start_routine, void* arg, thread_t* return_thread)
{
    CC_ASSERT(stack_size <= unsigned(-1));
    auto const handle = reinterpret_cast<::HANDLE>(::_beginthreadex(nullptr, unsigned(stack_size), start_routine, arg, 0U, nullptr));
    return_thread->handle = handle;

    if (handle != nullptr)
    {
        return_thread->id = ::GetThreadId(handle);
    }

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

inline void create_event(event_t* event)
{
    ::InitializeConditionVariable(&event->cv);
    ::InitializeCriticalSection(&event->crit_sec);
}

inline void destroy_event(event_t& eventId) { ::DeleteCriticalSection(&eventId.crit_sec); }

// returns false on timeout
inline bool wait_for_event(event_t& eventId, uint32_t milliseconds)
{
    ::EnterCriticalSection(&eventId.crit_sec);
    ::BOOL const retval = ::SleepConditionVariableCS(&eventId.cv, &eventId.crit_sec, milliseconds);
    CC_ASSERT(retval ? true : GetLastError() == ERROR_TIMEOUT && "Failed to wait on native CV");
    ::LeaveCriticalSection(&eventId.crit_sec);
    return bool(retval);
}

inline void signal_event(event_t& eventId) { ::WakeAllConditionVariable(&eventId.cv); }

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

[[maybe_unused]] constexpr static uint32_t event_wait_infinite = uint32_t(-1);

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

inline bool create_posix_thread(size_t stack_size, thread_start_func_t start_routine, void* arg, bool set_core_affinity, size_t core_affinity, thread_t* return_thread)
{
    pthread_attr_t thread_attr;
    pthread_attr_init(&thread_attr);
    CC_DEFER { pthread_attr_destroy(&thread_attr); };

    if (!set_stack_size(thread_attr, stack_size))
        return false;

    if (set_core_affinity)
    {
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
    }

    // we don't want any worker threads to ever receive signals
    // set the signal mask to block all signals (applies to the newly created thread)
    // see https://man7.org/linux/man-pages/man3/pthread_sigmask.3.html#NOTES
    // "A new thread inherits a copy of its creator's signal mask."
    sigset_t old_sig_mask;
    sigset_t sig_mask;
    sigfillset(&sig_mask); // mask now includes all signals
    // block (SIG_BLOCK) all signals and receive the previous mask
    bool const is_signal_mask_set = pthread_sigmask(SIG_BLOCK, &sig_mask, &old_sig_mask) == 0;

    auto const create_res = pthread_create(&return_thread->native, &thread_attr, start_routine, arg);

    // undo the signal mask change for the calling thread
    if (is_signal_mask_set)
    {
        // restore (SIG_SETMASK) the previous mask
        pthread_sigmask(SIG_SETMASK, &old_sig_mask, nullptr);
    }

    return create_res == 0;
}
}

inline bool create_thread(size_t stack_size, thread_start_func_t start_routine, void* arg, thread_t* return_thread)
{
    return detail::create_posix_thread(stack_size, start_routine, arg, false, 0, return_thread);
}

inline bool create_thread(size_t stack_size, thread_start_func_t start_routine, void* arg, size_t core_affinity, thread_t* return_thread)
{
    return detail::create_posix_thread(stack_size, start_routine, arg, true, core_affinity, return_thread);
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

inline void create_event(event_t* event) { *event = {PTHREAD_COND_INITIALIZER, PTHREAD_MUTEX_INITIALIZER}; }

inline void destroy_event(event_t& /*eventId*/)
{
    // No op
}

inline bool wait_for_event(event_t& eventId, int64_t milliseconds)
{
    pthread_mutex_lock(&eventId.mutex);

    constexpr int64_t mills_in_sec = 1000;

    bool event_was_signalled = true;
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
        auto const wait_res = pthread_cond_timedwait(&eventId.cond, &eventId.mutex, &waittime);
        event_was_signalled = wait_res != ETIMEDOUT;
    }

    pthread_mutex_unlock(&eventId.mutex);
    return event_was_signalled;
}

inline void signal_event(event_t& eventId)
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
