#pragma once

#include <atomic>

#include <clean-core/array.hh>

namespace td::container
{
// Thread safe, handle-based versioned ring buffer
// Well behaved on unsigned overflow if N < (unsigned max / 2)
template <class T, unsigned N>
struct VersionRing
{
    static_assert(N > 0);
    static_assert(N < unsigned(-1) / 2, "VersionRing too large");

public:
    explicit VersionRing() = default;

    /// Returns true if the handle no longer refers to a valid slot on the ring buffer
    [[nodiscard]] bool isExpired(unsigned handle) const { return mVersion.load(std::memory_order_acquire) - handle > N; }

    /// Acquires a slot in the ring buffer, writing a value to it
    [[nodiscard]] unsigned acquire(T value)
    {
        auto const handle = mVersion.fetch_add(1, std::memory_order_acquire);
        mData[handle % N] = std::move(value);
        return handle;
    }

    /// Accesses the slot in the ring buffer
    [[nodiscard]] T& get(unsigned handle) { return mData[handle % N]; }
    [[nodiscard]] T const& get(unsigned handle) const { return mData[handle % N]; }

    void reset() { mVersion.store(0, std::memory_order_release); }

private:
    cc::array<T, N> mData = {};
    std::atomic_uint mVersion = 0;

    VersionRing(VersionRing const& other) = delete;
    VersionRing(VersionRing&& other) noexcept = delete;
    VersionRing& operator=(VersionRing const& other) = delete;
    VersionRing& operator=(VersionRing&& other) noexcept = delete;
};
}
