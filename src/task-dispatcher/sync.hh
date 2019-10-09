#pragma once

namespace td
{
struct sync
{
    bool initialized = false;
    unsigned handle;

    explicit sync() = default;
};
}
