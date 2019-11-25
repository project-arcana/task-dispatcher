#pragma once

#include <cstddef>

namespace td::system
{
// == Compile time architecture info ==
inline size_t constexpr l1_cacheline_size = 64; // std::hardware_destructive_interference_size assumption, verified in .cc

// == Run time system info ==
[[nodiscard]] unsigned num_logical_cores() noexcept;  // std::thread::hardware_concurrency
[[nodiscard]] unsigned num_physical_cores() noexcept; // platform-specific, potentially expensive
}
