#pragma once

#include <atomic>

namespace td::container
{
// Thread safe, handle-based versioned ring buffer
// Well behaved on unsigned overflow if N < (unsigned max / 2)
template <class T, unsigned N>
struct version_ring
{
    static_assert(N < unsigned(-1) / 2, "version_ring too large");

private:
    T _data[N];
    std::atomic_uint _version = 0;

    version_ring(version_ring const& other) = delete;
    version_ring(version_ring&& other) noexcept = delete;
    version_ring& operator=(version_ring const& other) = delete;
    version_ring& operator=(version_ring&& other) noexcept = delete;

public:
    explicit version_ring() = default;

    bool is_expired(unsigned handle) const { return _version.load(std::memory_order_acquire) - handle > N; }

    unsigned acquire(T value)
    {
        auto handle = _version.fetch_add(1, std::memory_order_acquire);
        _data[handle % N] = std::move(value);
        return handle;
    }

    T& get(unsigned handle) { return _data[handle % N]; }

    void reset() { _version.store(0, std::memory_order_release); }
};
}
