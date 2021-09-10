#pragma once

#include <cstdint>

namespace td
{
namespace handle
{
using handle_t = uint32_t;
inline constexpr handle_t null_handle_value = 0;

struct counter
{
    handle_t _value = null_handle_value;
    counter() = default;
    explicit constexpr counter(handle_t val) : _value(val) {}
    void invalidate() & { _value = null_handle_value; }
    [[nodiscard]] constexpr bool is_valid() const noexcept { return _value != null_handle_value; }
    [[nodiscard]] constexpr bool operator==(counter rhs) const noexcept { return _value == rhs._value; }
    [[nodiscard]] constexpr bool operator!=(counter rhs) const noexcept { return _value != rhs._value; }
};

inline constexpr counter null_counter = handle::counter{null_handle_value};
}

struct sync
{
    handle::counter handle = handle::null_counter;
};
}
