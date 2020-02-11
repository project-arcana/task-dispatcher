#include "util.hh"

#include <clean-core/macros.hh>

#ifdef CC_OS_WINDOWS

#include <clean-core/native/win32_sanitized.hh>

bool td::native::maximize_os_scheduler_granularity()
{
    // Undocumented behavior of timeBeginPeriod,
    // sets the OS scheduler timeslice to the given value in milliseconds
    // see:
    //   https://docs.microsoft.com/en-us/windows/win32/api/timeapi/nf-timeapi-timebeginperiod
    //   https://hero.handmade.network/episode/code/day018/#3200

    return ::timeBeginPeriod(1) == TIMERR_NOERROR;
}

#else

bool td::native::maximize_os_scheduler_granularity() { return false; /* TODO */ }

#endif
