#pragma once

#include <clean-core/assert.hh>

#include <task-dispatcher/CounterHandle.hh>
#include <task-dispatcher/common/api.hh>

namespace td
{
// An automatically managed CounterHandle
// acquires a counter on first usage and releases it when waiting
struct TD_API AutoCounter
{
    CounterHandle handle = {};

    operator CounterHandle() &;
    operator CounterHandle() && = delete;

    AutoCounter() = default;

    AutoCounter(AutoCounter&& rhs) : handle(rhs.handle) { rhs.handle.invalidate(); }
    AutoCounter& operator=(AutoCounter&& rhs)
    {
        CC_ASSERT(!handle.isValid() && "Must call td::waitForCounter() on AutoCounter before letting it go out of scope");
        handle = rhs.handle;
        rhs.handle.invalidate();
        return *this;
    }

    ~AutoCounter() { CC_ASSERT(!handle.isValid() && "Must call td::waitForCounter() on AutoCounter before letting it go out of scope"); }
};
}
