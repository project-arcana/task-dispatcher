#pragma once

namespace td::native
{
/// attempts to increase the OS scheduler timeslice to the minimum amount (~.7ms), returns true on success
bool win32_set_scheduler_granular();

/// undos the change
bool win32_undo_scheduler_change();
}
