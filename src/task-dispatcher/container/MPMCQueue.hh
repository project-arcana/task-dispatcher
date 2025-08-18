#pragma once

#include <clean-core/experimental/mpmc_queue.hh>

namespace td
{
template <class T>
using MPMCQueue = cc::mpmc_queue<T>;
}
