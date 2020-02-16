#pragma once

#include <atomic>

#include <immintrin.h>

#include <clean-core/macros.hh>

namespace td
{
// test-and-test-and-set spinlock with SSE2 PAUSE
struct SpinLock
{
public:
    SpinLock() = default;
    ~SpinLock() = default;

    CC_FORCE_INLINE void lock()
    {
        do
        {
            wait_until_unlocked();
        } while (mIsLocked.exchange(true, std::memory_order_acquire) == true);
    }

    CC_FORCE_INLINE void wait_until_unlocked()
    {
        while (mIsLocked.load(std::memory_order_relaxed) == true)
        {
            _mm_pause();
        }
    }

    CC_FORCE_INLINE void unlock()
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

template <typename T>
class LockGuard
{
public:
    CC_FORCE_INLINE explicit LockGuard(T& mutex) : mMutex(mutex) { mMutex.lock(); }
    CC_FORCE_INLINE ~LockGuard() { mMutex.unlock(); }

    LockGuard(const LockGuard&) = delete;
    LockGuard(LockGuard&& other) noexcept = delete;
    LockGuard& operator=(const LockGuard&) = delete;
    LockGuard& operator=(LockGuard&& other) noexcept = delete;

private:
    T& mMutex;
};
}
