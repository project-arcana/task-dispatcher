#pragma once

#include <stdint.h>

#include <atomic>

#include <clean-core/bits.hh>

namespace td::container
{
// Thread safe, handle-based versioned ring buffer
// Well behaved on unsigned overflow if N < (unsigned max / 2)
template <class T, uint32_t N>
struct VersionRing
{
    static_assert(N > 0);
    static_assert(N < uint32_t(-1) / 2, "VersionRing too large");
    static_assert(cc::is_pow2(N), "VersionRing N must be power of two");

public:
    explicit VersionRing() = default;

    /// Returns true if the handle no longer refers to a valid slot on the ring buffer
    [[nodiscard]] bool isExpired(uint32_t handle) const { return mVersion.load(std::memory_order_acquire) - handle > N; }

    /// Acquires a slot in the ring buffer, writing a value to it
    [[nodiscard]] uint32_t acquire(T value)
    {
        auto const handle = mVersion.fetch_add(1, std::memory_order_acquire);
        this->get(handle) = value;
        return handle;
    }

    /// Accesses the slot in the ring buffer
    [[nodiscard]] T& get(uint32_t handle) { return mData[cc::mod_pow2(handle, N)]; }
    [[nodiscard]] T const& get(uint32_t handle) const { return mData[cc::mod_pow2(handle, N)]; }

    void reset() { mVersion.store(0, std::memory_order_release); }

private:
    T mData[N] = {};
    std::atomic_uint mVersion = 0;

    VersionRing(VersionRing const& other) = delete;
    VersionRing(VersionRing&& other) noexcept = delete;
    VersionRing& operator=(VersionRing const& other) = delete;
    VersionRing& operator=(VersionRing&& other) noexcept = delete;
};
}
