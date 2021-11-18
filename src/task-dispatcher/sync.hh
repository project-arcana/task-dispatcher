#pragma once

#include <stdint.h>

namespace td
{
// Handle to a counter, the core synchronization mechanism
struct CounterHandle
{
    uint32_t _value = 0;

    CounterHandle() = default;
    explicit constexpr CounterHandle(uint32_t val) : _value(val) {}

    void invalidate() & { _value = 0; }
    bool isValid() const { return _value != 0; }

    bool operator==(CounterHandle rhs) const { return _value == rhs._value; }
    bool operator!=(CounterHandle rhs) const { return _value != rhs._value; }
};

inline constexpr CounterHandle NullCounterHandle = CounterHandle{};

// An automatically managed CounterHandle
// TODO(JK): make compatible with existing overloads
// idea: l-value qualified conversion operator to counter handle doing the optional check
// then overload the wait function on Sync& explicitly and run the optional free
// make sure to delete const& overloads to avoid errors there
// might even want to add move ctor and assert in the dtor to make sure that someone waited on the handle and cleared it
struct Sync
{
    CounterHandle handle = {};
};
}
