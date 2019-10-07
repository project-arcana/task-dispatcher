#pragma once

#include <atomic>

namespace td::container
{
// Thread safe, handle-based versioned ring buffer
// Well behaved on unsigned overflow if N < (unsigned max / 2)
template <class T, unsigned N>
struct VersionRing
{
    static_assert(N < unsigned(-1) / 2, "version_ring too large");

private:
    T mData[N];
    std::atomic_uint mVersion = 0;

    VersionRing(VersionRing const& other) = delete;
    VersionRing(VersionRing&& other) noexcept = delete;
    VersionRing& operator=(VersionRing const& other) = delete;
    VersionRing& operator=(VersionRing&& other) noexcept = delete;

public:
    explicit VersionRing() = default;

    bool isExpired(unsigned handle) const { return mVersion.load(std::memory_order_acquire) - handle > N; }

    unsigned acquire(T value)
    {
        auto handle = mVersion.fetch_add(1, std::memory_order_acquire);
        mData[handle % N] = std::move(value);
        return handle;
    }

    T& get(unsigned handle) { return mData[handle % N]; }

    void reset() { mVersion.store(0, std::memory_order_release); }
};
}
