#pragma once

#include <task-dispatcher/CounterHandle.hh>
#include <task-dispatcher/common/api.hh>

namespace td
{
// a critical section inside of the scheduler
// unlike an OS primitive, this yields the locking fiber instead of the locking OS thread
// all threads interacting with one of these must be inside the td scheduler
struct TD_API CriticalSection
{
    CriticalSection();
    ~CriticalSection();

    CriticalSection(CriticalSection&& rhs) noexcept
    {
        mCounter = rhs.mCounter;
        rhs.mCounter = {};
    }

    CriticalSection& operator=(CriticalSection&& rhs) noexcept
    {
        if (mCounter.isValid())
        {
            destroy();
        }
        mCounter = rhs.mCounter;
        rhs.mCounter = {};
        return *this;
    }

    CriticalSection(CriticalSection const&) = delete;
    CriticalSection& operator=(CriticalSection const&) = delete;

    // pinned: whether the locking fiber must wake up on the same OS thread after the lock is acquired
    void lock(bool pinned = true) noexcept;

    void unlock() noexcept;

private:
    void destroy();

    CounterHandle mCounter;
};
}
