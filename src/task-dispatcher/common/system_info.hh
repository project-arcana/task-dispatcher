#pragma once

#include <stdint.h>

#include "api.hh"

namespace td
{
enum
{
    l1_cacheline_size = 64
};

// Returns amount of logical CPU cores
TD_API uint32_t getNumLogicalCPUCores() noexcept;

// Returns amount of physical CPU cores (platform-specific, expensive on Windows)
// WARNING: This is inaccurate on Linux for non-Intel and doesn't account for hyperthreading being disabled (but available)
[[deprecated("Inaccurate, do not use")]] TD_API uint32_t getNumPhysicalCPUCores() noexcept;
}
