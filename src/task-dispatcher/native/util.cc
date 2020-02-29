#include "util.hh"

#include <clean-core/macros.hh>

#ifdef CC_OS_WINDOWS

#include <clean-core/native/win32_sanitized.hh>

extern "C"
{
    DECLSPEC_IMPORT
    UINT WINAPI timeBeginPeriod(_In_ UINT uPeriod);

    DECLSPEC_IMPORT
    UINT WINAPI timeEndPeriod(_In_ UINT uPeriod);
};

bool td::native::win32_set_scheduler_granular()
{
    // Barely documented behavior of timeBeginPeriod,
    // sets the OS scheduler timeslice to the given value in milliseconds
    // in practice an argument of 1 ends up at ~.7ms
    // see:
    //   https://docs.microsoft.com/en-us/windows/win32/api/timeapi/nf-timeapi-timebeginperiod
    //   https://hero.handmade.network/episode/code/day018/#3200

    // This change is global and should be undone at shutdown
    // It should not be called often (ideally just once)

    return ::timeBeginPeriod(1) == 0 /*TIMERR_NOERROR*/;
}

bool td::native::win32_undo_scheduler_change()
{
    // Undos the change to the OS scheduler, the "period" specified must
    // be the same as in the first call

    return ::timeEndPeriod(1) == 0 /*TIMERR_NOERROR*/;
}

#else

bool td::native::win32_set_scheduler_granular() { return false; /* no equivalent */ }
bool td::native::win32_undo_scheduler_change() { return false; /* no equivalent */ }

#endif
