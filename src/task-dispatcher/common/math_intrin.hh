#pragma once

#include <cstdint>

#ifdef _MSC_VER
#include <intrin.h>
#endif

namespace td::intrin
{
inline uint32_t bsr(uint32_t v)
{
#ifdef _MSC_VER
    unsigned long index;
    _BitScanReverse(&index, v);
    return index;
#elif defined __GNUC__
    return sizeof(v) * 8 - __builtin_clz(v);
#endif
}

inline uint32_t bsr(uint64_t v)
{
#ifdef _MSC_VER
    unsigned long index;
    _BitScanReverse64(&index, v);
    return index;
#elif defined __GNUC__
    return sizeof(v) * 8 - __builtin_clzll(v);
#endif
}

inline uint64_t rdtsc()
{
#if defined(__GNUC__) && !defined(__clang__)
    unsigned int lo, hi;
    __asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
#else
    return __rdtsc();
#endif
}
}

namespace td::math
{
[[nodiscard]] inline uint32_t clog2(uint32_t x) { return intrin::bsr((x << 1) - 1); }
[[nodiscard]] inline uint64_t clog2(uint64_t x) { return intrin::bsr((x << 1) - 1); }

[[nodiscard]] inline uint32_t upperpow2(uint32_t x) { return uint32_t(2u) << clog2(x); }
[[nodiscard]] inline uint64_t upperpow2(uint64_t x) { return uint64_t(2u) << clog2(x); }
}
