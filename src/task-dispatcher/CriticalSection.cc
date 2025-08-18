#include "CriticalSection.hh"

#include <clean-core/assert.hh>

#include "Scheduler.hh"

namespace
{
enum CritSecState : int32_t
{
    CritSecState_Unlocked = 0,
    CritSecState_Locked
};
}

td::CriticalSection::CriticalSection()
{
    CC_ASSERT(td::isInsideScheduler() && "td::CriticalSection must only be used from inside a scheduler");
    mCounter = td::acquireCounter();
}

td::CriticalSection::~CriticalSection() { destroy(); }

void td::CriticalSection::lock(bool pinned) noexcept
{
    while (true)
    {
        // try to acquire
        int32_t prevState = td::compareAndSwapCounter(mCounter, CritSecState_Unlocked, CritSecState_Locked);

        if (prevState == CritSecState_Unlocked)
        {
            // CAS successful
            return;
        }

        // wait until this is unlocked, then retry CAS
        td::waitForCounter(mCounter, pinned);
    }
}

void td::CriticalSection::unlock() noexcept
{
    int32_t prevState = td::compareAndSwapCounter(mCounter, CritSecState_Locked, CritSecState_Unlocked);
    CC_ASSERT(prevState == CritSecState_Locked && "unlocked a critical section that was already unlocked");
}

void td::CriticalSection::destroy()
{
    if (mCounter.isValid())
    {
        td::releaseCounter(mCounter);
        mCounter = {};
    }
}
