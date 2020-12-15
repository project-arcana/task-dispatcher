#pragma once

#include <cstddef>

#include "api.hh"

namespace td::system
{
// == Compile time architecture info ==
inline size_t constexpr l1_cacheline_size = 64; // std::hardware_destructive_interference_size assumption, verified in .cc

// == Run time system info ==
[[nodiscard]] TD_API unsigned num_logical_cores() noexcept;  // std::thread::hardware_concurrency
[[nodiscard]] TD_API unsigned num_physical_cores() noexcept; // platform-specific, expensive on Windows
}
