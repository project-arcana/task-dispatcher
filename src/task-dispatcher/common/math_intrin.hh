#pragma once

#include <cstdint>

#ifdef _MSC_VER
#include <intrin.h>
#else
#include <x86intrin.h>
#endif

namespace td::intrin
{
inline unsigned long bsr(uint32_t v) noexcept
{
    unsigned long index;
    _BitScanReverse(&index, v);
    return index;
}

inline unsigned long bsr(uint64_t v) noexcept
{
    unsigned long index;
    _BitScanReverse64(&index, v);
    return index;
}

inline unsigned long long rdtsc() noexcept { return __rdtsc(); }
}

namespace td::math
{
template <class T>
[[nodiscard]] constexpr bool ispow2(T x)
{
    return ((x & (x - 1)) == 0);
}

[[nodiscard]] inline uint32_t log2(uint32_t x) { return intrin::bsr(x); }
[[nodiscard]] inline uint64_t log2(uint64_t x) { return intrin::bsr(x); }

[[nodiscard]] inline uint32_t clog2(uint32_t x) { return intrin::bsr((x << 1) - 1); }
[[nodiscard]] inline uint64_t clog2(uint64_t x) { return intrin::bsr((x << 1) - 1); }

[[nodiscard]] inline uint32_t upperpow2(uint32_t x) { return uint32_t(2u) << clog2(x); }
[[nodiscard]] inline uint64_t upperpow2(uint64_t x) { return uint64_t(2u) << clog2(x); }

[[nodiscard]] inline uint32_t nextpow2(uint32_t x) { return x < 2 ? 1 : uint32_t(1u) << (intrin::bsr(x - 1)); }
[[nodiscard]] inline uint64_t nextpow2(uint64_t x) { return x < 2 ? 1 : uint64_t(1u) << (intrin::bsr(x - 1)); }

}
