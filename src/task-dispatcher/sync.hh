#pragma once

#include <cstdint>

namespace td
{
using counter_handle_t = uint32_t;

struct sync
{
    bool initialized = false;
    counter_handle_t handle;

    explicit sync() = default;
};
}
