#pragma once

#include <clean-core/allocators/system_allocator.hh>

// huge header, + includes <thread>, <algorithm>, <utility> among others
#include <task-dispatcher/lib/moodycamel/concurrentqueue.h>

// moderate header, we added this option to skip the <chrono> include
#define MOODYCAMEL_NO_INCLUDE_CHRONO 1
#include <task-dispatcher/lib/moodycamel/blockingconcurrentqueue.h>

namespace td
{
struct ConcurrentQueueTraits : moodycamel::ConcurrentQueueDefaultTraits
{
    // to replace malloc in moodycamel, shadow these methods in the traits struct
    // no userdata void* mechanism, must use globals for an allocator ptr
    static inline void* malloc(size_t size) { return cc::system_malloc(size, 8); }
    static inline void free(void* ptr) { cc::system_free(ptr); }
};

template <class T, class Traits = ConcurrentQueueTraits>
using ConcurrentQueue = moodycamel::ConcurrentQueue<T, Traits>;

template <class T, class Traits = ConcurrentQueueTraits>
using BlockingConcurrentQueue = moodycamel::BlockingConcurrentQueue<T, Traits>;

using ConcurrentQueueProducerToken = moodycamel::ProducerToken;
using ConcurrentQueueConsumerTokenToken = moodycamel::ConsumerToken;
}