#include "AutoCounter.hh"

#include "Scheduler.hh"

td::AutoCounter::operator td::CounterHandle() &
{
    if (!handle.isValid())
    {
        handle = td::acquireCounter();
    }

    return handle;
}

int32_t td::waitForCounter(AutoCounter& autoCounter, bool pinned)
{
    if (!autoCounter.handle.isValid())
    {
        // return immediately for uninitialized syncs
        return 0;
    }

    // perform real wait
    int32_t const res = td::waitForCounter(autoCounter.handle, pinned);

    // this call must not be raced on
    if (td::releaseCounterIfOnZero(autoCounter.handle))
    {
        // mark the sync as uninitialized
        autoCounter.handle.invalidate();
    }

    return res;
}