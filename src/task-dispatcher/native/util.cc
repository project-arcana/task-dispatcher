#include "util.hh"

#include <cstdio>

#include <clean-core/macros.hh>

#ifdef CC_OS_WINDOWS

#include <clean-core/native/win32_sanitized.hh>

namespace
{
typedef UINT(WINAPI* timeBeginPeriodPtr)(_In_ UINT);
typedef UINT(WINAPI* timeEndPeriodPtr)(_In_ UINT);

HMODULE g_hmodule_winmm = NULL;
timeBeginPeriodPtr g_timebeginperiod = nullptr;
timeEndPeriodPtr g_timeendperiod = nullptr;
}


bool td::native::win32_init_utils()
{
    if (g_hmodule_winmm)
        return true;

    g_hmodule_winmm = LoadLibrary("Winmm.dll");

    if (!g_hmodule_winmm)
    {
        fprintf(stderr, "[task-dispatcher] warning: failed to load Winmm.dll\n");
        return false;
    }

    g_timebeginperiod = (timeBeginPeriodPtr)::GetProcAddress(g_hmodule_winmm, "timeBeginPeriod");
    g_timeendperiod = (timeEndPeriodPtr)::GetProcAddress(g_hmodule_winmm, "timeEndPeriod");

    if (!g_timebeginperiod || !g_timeendperiod)
    {
        fprintf(stderr, "[task-dispatcher] warning: failed to receive function pointers from Winmm.dll\n");
        win32_shutdown_utils();
        return false;
    }

    return true;
}

void td::native::win32_shutdown_utils()
{
    if (g_hmodule_winmm)
    {
        ::FreeLibrary(g_hmodule_winmm);
        g_hmodule_winmm = NULL;
    }

    g_timebeginperiod = nullptr;
    g_timeendperiod = nullptr;
}

bool td::native::win32_enable_scheduler_granular()
{
    // Barely documented behavior of timeBeginPeriod,
    // sets the OS scheduler timeslice to the given value in milliseconds
    // in practice an argument of 1 ends up at ~.7ms
    // see:
    //   https://docs.microsoft.com/en-us/windows/win32/api/timeapi/nf-timeapi-timebeginperiod
    //   https://hero.handmade.network/episode/code/day018/#3200

    // This change is global and should be undone at shutdown
    // It should not be called often (ideally just once)

    if (!g_timebeginperiod)
        return false;

    return g_timebeginperiod(1) == 0 /*TIMERR_NOERROR*/;
}

bool td::native::win32_disable_scheduler_granular()
{
    // Undos the change to the OS scheduler, the "period" specified must
    // be the same as in the first call
    if (!g_timeendperiod)
        return false;

    return g_timeendperiod(1) == 0 /*TIMERR_NOERROR*/;
}

#else

bool td::native::win32_init_utils() { return true; }
void td::native::win32_shutdown_utils() {}
bool td::native::win32_enable_scheduler_granular() { return true; }
bool td::native::win32_disable_scheduler_granular() { return true; }

#endif
