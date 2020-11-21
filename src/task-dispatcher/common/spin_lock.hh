#pragma once

#include <atomic>

#include <immintrin.h>

#include <clean-core/macros.hh>

namespace td
{
/// TTAS Spinlock
struct SpinLock
{
public:
    SpinLock() = default;
    ~SpinLock() = default;

    CC_FORCE_INLINE void lock() noexcept
    {
        while (true)
        {
            // immediately try to exchange
            // memory order: locking acquires, unlocking releases
            if (mIsLocked.exchange(true, std::memory_order_acquire) == false)
            {
                // exhange returned false, meaning the lock was previously unlocked, success
                return;
            }

            // exchange failed, wait until the value is false without forcing cache misses
            while (mIsLocked.load(std::memory_order_relaxed))
            {
                // x86 PAUSE to signal spin-wait, improve interleaving
                _mm_pause();
            }
        }
    }

    CC_FORCE_INLINE bool try_lock() noexcept
    {
        // early out using a relaxed load to improve performance when spinning on try_lock()
        // ref: https://rigtorp.se/spinlock/
        return !mIsLocked.load(std::memory_order_relaxed) //
               && !mIsLocked.exchange(true, std::memory_order_acquire);
    }

    CC_FORCE_INLINE void unlock() noexcept
    {
        // release
        mIsLocked.store(false, std::memory_order_release);
    }

    SpinLock(SpinLock const& other) = delete;
    SpinLock(SpinLock&& other) noexcept = delete;
    SpinLock& operator=(SpinLock const& other) = delete;
    SpinLock& operator=(SpinLock&& other) noexcept = delete;

private:
    std::atomic_bool mIsLocked = {false};
};

/// Simple replacement for std::lock_guard
template <typename T>
class LockGuard
{
public:
    [[nodiscard]] CC_FORCE_INLINE explicit LockGuard(T& mutex) : mMutex(mutex) { mMutex.lock(); }
    CC_FORCE_INLINE ~LockGuard() { mMutex.unlock(); }

    LockGuard(const LockGuard&) = delete;
    LockGuard(LockGuard&& other) noexcept = delete;
    LockGuard& operator=(const LockGuard&) = delete;
    LockGuard& operator=(LockGuard&& other) noexcept = delete;

private:
    T& mMutex;
};
}
