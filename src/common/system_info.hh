#pragma once

namespace td::system
{
// == Compile time system info ==
inline unsigned constexpr l1_cacheline_size = 64; // std::hardware_destructive_interference_size assumption, verified in .cc

// == Run time system info ==
extern unsigned const hardware_concurrency; // std::thread::hardware_concurrency()
}
