#pragma once

#include <stdint.h>

namespace td
{
template <class T>
struct MPMCQueue;

template <class T, uint32_t N>
struct FIFOQueue;

template <class T, uint32_t N>
struct VersionRing;

struct CounterHandle;
struct AutoCounter;
struct Task;
struct SchedulerConfig;
}
