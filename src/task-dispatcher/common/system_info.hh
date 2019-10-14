#pragma once

#include <cstddef>

namespace td::system
{
// == Compile time system info ==
inline size_t constexpr l1_cacheline_size = 64; // std::hardware_destructive_interference_size assumption, verified in .cc

// == Run time system info ==
extern unsigned const hardware_concurrency; // std::thread::hardware_concurrency()
}
