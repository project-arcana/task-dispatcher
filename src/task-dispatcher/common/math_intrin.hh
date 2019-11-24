#pragma once

#include <cstdint>

#ifdef _MSC_VER
#include <clean-core/native/win32_sanitized.hh>
#endif

namespace td::intrin
{
inline uint32_t bsr(uint32_t v)
{
#ifdef _MSC_VER
    DWORD index;
    _BitScanReverse(&index, v);
    return index;
#elif defined __GNUC__
    return sizeof(v) * 8 - __builtin_clz(v);
#endif
}

inline uint32_t bsr(uint64_t v)
{
#ifdef _MSC_VER
    DWORD index;
    _BitScanReverse64(&index, v);
    return index;
#elif defined __GNUC__
    return sizeof(v) * 8 - __builtin_clzll(v);
#endif
}

inline uint32_t cas(volatile uint32_t* dst, uint32_t cmp, uint32_t exc)
{
#ifdef _MSC_VER
    return _InterlockedCompareExchange(dst, exc, cmp);
#elif defined __GNUC__
    return __sync_val_compare_and_swap(dst, cmp, exc);
#endif
}

inline uint64_t rdtsc() { return __rdtsc(); }
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
