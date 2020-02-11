#pragma once

namespace td::native {

/// attempts to increase the OS scheduler timeslice to 1ms, returns true on success
bool maximize_os_scheduler_granularity();

}
