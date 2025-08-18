#include "SchedulerConfig.hh"

#include <clean-core/bits.hh>

void td::SchedulerConfig::ceilValuesToPowerOfTwo()
{
    numFibers = cc::ceil_pow2(numFibers);
    maxNumCounters = cc::ceil_pow2(maxNumCounters);
    maxNumTasks = cc::ceil_pow2(maxNumTasks);
}

bool td::SchedulerConfig::isValid() const
{
    return staticAlloc != nullptr && numFibers > 0 && numThreads > 0 && maxNumCounters > 0 && maxNumTasks > 0 && cc::is_pow2(numFibers)
           && cc::is_pow2(maxNumCounters) && cc::is_pow2(maxNumTasks);
}