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
    AutoCounter() = default;

    AutoCounter(AutoCounter&& rhs) noexcept : handle(rhs.handle) { rhs.handle.invalidate(); }
    AutoCounter& operator=(AutoCounter&& rhs) noexcept
    {
        CC_ASSERT(!handle.isValid() && "Must call td::waitForCounter() on AutoCounter before dropping it");
        handle = rhs.handle;
        rhs.handle.invalidate();
        return *this;
    }

    ~AutoCounter() { CC_ASSERT(!handle.isValid() && "Must call td::waitForCounter() on AutoCounter before dropping it"); }

    operator CounterHandle() &;

    [[deprecated("For less restricted usage, use raw CounterHandle")]] AutoCounter(AutoCounter const&) = delete;
    [[deprecated("For less restricted usage, use raw CounterHandle")]] AutoCounter& operator=(AutoCounter const&) = delete;
    [[deprecated("Would immediately drop AutoCounter")]] operator CounterHandle() && = delete;

    CounterHandle handle = {};
};
}
