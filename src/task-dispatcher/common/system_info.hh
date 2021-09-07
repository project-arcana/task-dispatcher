#pragma once

#include <cstddef>
#include <cstdint>

#include "api.hh"

namespace td::system
{
enum
{
    l1_cacheline_size = 64
};

// Returns amount of logical CPU cores (calls std::thread::hardware_concurrency)
TD_API uint32_t num_logical_cores() noexcept;

// Returns amount of physical CPU cores (platform-specific, expensive on Windows)
// WARNING: This is inaccurate on Linux for non-Intel and doesn't account for hyperthreading being disabled (but available)
[[deprecated("Inaccurate, do not use")]] 
TD_API uint32_t num_physical_cores() noexcept;
}
