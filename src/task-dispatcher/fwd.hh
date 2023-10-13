#pragma once

#include <stdint.h>

namespace cc
{
template <class T>
struct mpmc_queue;
}

namespace td
{
template <class T>
using MPMCQueue = cc::mpmc_queue<T>;

template <class T>
struct FIFOQueue;

template <class T, uint32_t N>
struct VersionRing;

struct CounterHandle;
struct AutoCounter;

template <bool AllowHeapAlloc, class... Args>
struct TTask;

#ifdef TD_ALLOW_HEAP_TASKS
using Task = TTask<true>;
#else
using Task = TTask<false>;
#endif

struct SchedulerConfig;
}
