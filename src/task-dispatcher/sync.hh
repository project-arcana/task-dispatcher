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

// An automatically managed CounterHandle
struct Sync
{
    CounterHandle handle = {};
};
}
