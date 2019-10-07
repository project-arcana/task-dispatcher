#include "system_info.hh"

#include <new>
#include <thread>

#ifdef _MSC_VER
static_assert(td::system::l1_cacheline_size == std::hardware_destructive_interference_size, "L1 Cacheline size assumption wrong");
#else
// Clang doesn't support std::hardware_destructive_interference yet
#endif

unsigned const td::system::hardware_concurrency = std::thread::hardware_concurrency();
