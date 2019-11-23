#pragma once

#include <atomic>

namespace td
{
// Simple spinlock, fullfills BasicLockable named requirement
// can be used with std::lock_guard
struct SpinLock
{
private:
    std::atomic_flag mFlag;

public:
    SpinLock() = default;
    ~SpinLock() = default;

    void lock()
    {
        // acquire and spin
        while (mFlag.test_and_set(std::memory_order_acquire))
            ;
    }

    void unlock()
    {
        // release
        mFlag.clear(std::memory_order_release);
    }

    SpinLock(SpinLock const& other) = delete;
    SpinLock(SpinLock&& other) noexcept = delete;
    SpinLock& operator=(SpinLock const& other) = delete;
    SpinLock& operator=(SpinLock&& other) noexcept = delete;
};

template <typename T>
class LockGuard
{
public:
    explicit LockGuard(T& mutex) : mMutex(mutex) { mMutex.lock(); }
    ~LockGuard() { mMutex.unlock(); }

    LockGuard(const LockGuard&) = delete;
    LockGuard(LockGuard&& other) noexcept = delete;
    LockGuard& operator=(const LockGuard&) = delete;
    LockGuard& operator=(LockGuard&& other) noexcept = delete;

private:
    T& mMutex;
};
}
