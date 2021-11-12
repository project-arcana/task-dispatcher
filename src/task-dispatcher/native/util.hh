#pragma once

namespace td::native
{
// loads and unloads Winmm.dll and the function pointers required for the utilities below
bool win32_init_utils();
void win32_shutdown_utils();

// attempts to shorten the OS scheduler timeslice to the minimum amount (~.7ms) using timeBeginPeriod
// this change is global for the entire OS and should be undone at shutdown
bool win32_enable_scheduler_granular();

// reverts the change made by win32_enable_scheduler_granular
bool win32_disable_scheduler_granular();
}
