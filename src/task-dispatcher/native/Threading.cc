#include "Threading.hh"

#include <clean-core/assert.hh>
#include <clean-core/defer.hh>
#include <clean-core/macros.hh>

#ifdef CC_OS_WINDOWS

#include <cstdio>

#include <process.h>
#include <clean-core/native/win32_sanitized.hh>

#else

#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <signal.h>
#include <unistd.h>

#endif

#ifdef CC_OS_WINDOWS

namespace td::native
{
namespace
{

#pragma pack(push, 8)
typedef struct tagTHREADNAME_INFO
{
    DWORD dwType;     // Must be 0x1000.
    LPCSTR szName;    // Pointer to name (in user addr space).
    DWORD dwThreadID; // Thread ID (-1=caller thread).
    DWORD dwFlags;    // Reserved for future use, must be zero.
} THREADNAME_INFO;
#pragma pack(pop)

::CRITICAL_SECTION* getCriticalSectionPtr(td::native::event_t* pEvent)
{
    return reinterpret_cast<::CRITICAL_SECTION*>(&pEvent->criticalSection.buf[0]);
}

::CONDITION_VARIABLE* getConditionVariablePtr(td::native::event_t* pEvent)
{
    return reinterpret_cast<::CONDITION_VARIABLE*>(&pEvent->conditionVariable.buf[0]);
}
}
}

static_assert(alignof(td::native::Win32ConditionVariable) == alignof(::CONDITION_VARIABLE), "Win32CritSection misaligns");
static_assert(sizeof(td::native::Win32ConditionVariable) == sizeof(::CONDITION_VARIABLE), "Win32CritSection has wrong size");

static_assert(alignof(td::native::Win32CritSection) == alignof(::CRITICAL_SECTION), "Win32CritSection misaligns");
static_assert(sizeof(td::native::Win32CritSection) == sizeof(::CRITICAL_SECTION), "Win32CritSection has wrong size");

bool td::native::createThread(size_t stack_size, thread_start_func_t start_routine, void* arg, thread_t* return_thread)
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

bool td::native::createThread(size_t stack_size, thread_start_func_t start_routine, void* arg, size_t core_affinity, thread_t* return_thread)
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

void td::native::endCurrentThread() { ::_endthreadex(0); }

void td::native::joinThread(thread_t thread) { ::WaitForSingleObject(thread.handle, INFINITE); }

td::native::thread_t td::native::getCurrentThread() { return thread_t{::GetCurrentThread(), ::GetCurrentThreadId()}; }

void td::native::setCurrentThreadCoreAffinity(size_t coreAffinity) { ::SetThreadAffinityMask(::GetCurrentThread(), 1ULL << coreAffinity); }

void td::native::setCurrentThreadLowPriority() { ::SetThreadPriority(::GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL); }

void td::native::setCurrentThreadDebugName(char const* pName)
{
    // this throws a SEH exception which is one of two ways of setting a thread name
    // in Win32. the other is less reliable and only available after Win10 1607
    // see https://docs.microsoft.com/en-us/visualstudio/debugger/how-to-set-a-thread-name-in-native-code?view=vs-2019

    THREADNAME_INFO info;
    info.dwType = 0x1000;
    info.szName = pName;
    info.dwThreadID = (DWORD)-1;
    info.dwFlags = 0;
#pragma warning(push)
#pragma warning(disable : 6320 6322)
    __try
    {
        RaiseException(0x406D1388, 0, sizeof(info) / sizeof(ULONG_PTR), (ULONG_PTR*)&info);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }
#pragma warning(pop)
}

void td::native::createEvent(event_t* event)
{
    ::InitializeConditionVariable(getConditionVariablePtr(event));
    ::InitializeCriticalSection(getCriticalSectionPtr(event));
}

void td::native::destroyEvent(event_t& eventId) { ::DeleteCriticalSection(getCriticalSectionPtr(&eventId)); }

bool td::native::waitForEvent(event_t& eventId, uint32_t milliseconds)
{
    ::EnterCriticalSection(getCriticalSectionPtr(&eventId));
    ::BOOL const retval = ::SleepConditionVariableCS(getConditionVariablePtr(&eventId), getCriticalSectionPtr(&eventId), milliseconds);
    CC_ASSERT(retval ? true : GetLastError() == ERROR_TIMEOUT && "Failed to wait on Win32 Condition Variable");
    ::LeaveCriticalSection(getCriticalSectionPtr(&eventId));
    return bool(retval);
}

void td::native::signalEvent(event_t& eventId) { ::WakeAllConditionVariable(getConditionVariablePtr(&eventId)); }

void td::native::threadSleep(uint32_t milliseconds) { ::Sleep(milliseconds); }

#else

namespace td::native::detail
{
namespace
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
}

void td::native::setCurrentThreadDebugName(char const* pName)
{
    (void)pName; // TODO
}

bool td::native::createThread(size_t stack_size, thread_start_func_t start_routine, void* arg, thread_t* return_thread)
{
    return detail::create_posix_thread(stack_size, start_routine, arg, false, 0, return_thread);
}

bool td::native::createThread(size_t stack_size, thread_start_func_t start_routine, void* arg, size_t core_affinity, thread_t* return_thread)
{
    return detail::create_posix_thread(stack_size, start_routine, arg, true, core_affinity, return_thread);
}

void td::native::endCurrentThread() { pthread_exit(nullptr); }

void td::native::joinThread(thread_t thread) { pthread_join(thread.native, nullptr); }

td::native::thread_t td::native::getCurrentThread() { return {pthread_self()}; }

void td::native::setCurrentThreadCoreAffinity(size_t coreAffinity)
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

void td::native::createEvent(event_t* event) { *event = {PTHREAD_COND_INITIALIZER, PTHREAD_MUTEX_INITIALIZER}; }

void td::native::destroyEvent(event_t& /*eventId*/)
{
    // No op
}

bool td::native::waitForEvent(event_t& eventId, uint32_t milliseconds)
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
        int64_t milliseconds_i64 = int64_t(milliseconds);
        waittime.tv_sec = milliseconds_i64 / mills_in_sec;
        milliseconds_i64 -= waittime.tv_sec * mills_in_sec;
        waittime.tv_nsec = milliseconds_i64 * mills_in_sec;
        auto const wait_res = pthread_cond_timedwait(&eventId.cond, &eventId.mutex, &waittime);
        event_was_signalled = wait_res != ETIMEDOUT;
    }

    pthread_mutex_unlock(&eventId.mutex);
    return event_was_signalled;
}

void td::native::signalEvent(event_t& eventId)
{
    pthread_mutex_lock(&eventId.mutex);
    pthread_cond_broadcast(&eventId.cond);
    pthread_mutex_unlock(&eventId.mutex);
}

void td::native::threadSleep(uint32_t milliseconds)
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