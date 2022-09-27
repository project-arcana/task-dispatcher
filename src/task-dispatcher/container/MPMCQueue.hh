#pragma once

#include <stddef.h>
#include <stdint.h>

#include <atomic>

#include <clean-core/alloc_array.hh>
#include <clean-core/assert.hh>
#include <clean-core/bits.hh>

namespace td
{
// Alloc-free MPMC queue, ~75 cycles per enqueue and dequeue
// http://www.1024cores.net/home/lock-free-algorithms/queues/bounded-mpmc-queue
// See licenses/

template <class T>
struct MPMCQueue
{
public:
    MPMCQueue() = default;
    explicit MPMCQueue(size_t buffer_size, cc::allocator* allocator) { initialize(buffer_size, allocator); }

    void initialize(size_t bufferSize, cc::allocator* alloc)
    {
        CC_ASSERT(bufferSize >= 2 && cc::is_pow2(bufferSize) && "MPMCQueue size not a power of two");

        mBufferMask = bufferSize - 1;
        mBuffer.reset(alloc, bufferSize);
        for (size_t i = 0; i != bufferSize; ++i)
        {
            mBuffer[i].sequence_.store(i, std::memory_order_relaxed);
        }

        mEnqueuePos.store(0, std::memory_order_relaxed);
        mDequeuePos.store(0, std::memory_order_relaxed);
    }

    bool enqueue(const T& data)
    {
        cell* cell;
        size_t pos = mEnqueuePos.load(std::memory_order_relaxed);
        for (;;)
        {
            cell = &mBuffer[pos & mBufferMask];
            size_t seq = cell->sequence_.load(std::memory_order_acquire);
            intptr_t diff = static_cast<intptr_t>(seq) - static_cast<intptr_t>(pos);
            if (diff == 0)
            {
                if (mEnqueuePos.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed))
                    break;
            }
            else if (diff < 0)
                return false;
            else
                pos = mEnqueuePos.load(std::memory_order_relaxed);
        }
        cell->data_ = data;
        cell->sequence_.store(pos + 1, std::memory_order_release);
        return true;
    }

    bool dequeue(T& data)
    {
        cell* cell;
        size_t pos = mDequeuePos.load(std::memory_order_relaxed);
        for (;;)
        {
            cell = &mBuffer[pos & mBufferMask];
            size_t seq = cell->sequence_.load(std::memory_order_acquire);
            intptr_t diff = static_cast<intptr_t>(seq) - static_cast<intptr_t>(pos + 1);
            if (diff == 0)
            {
                if (mDequeuePos.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed))
                    break;
            }
            else if (diff < 0)
                return false;
            else
                pos = mDequeuePos.load(std::memory_order_relaxed);
        }
        data = cell->data_;
        cell->sequence_.store(pos + mBufferMask + 1, std::memory_order_release);
        return true;
    }

private:
    struct cell
    {
        std::atomic<size_t> sequence_;
        T data_;
    };

    alignas(64) cc::alloc_array<cell> mBuffer;

    size_t mBufferMask = 0;

    alignas(64) std::atomic<size_t> mEnqueuePos = {0};
    alignas(64) std::atomic<size_t> mDequeuePos = {0};

    MPMCQueue(MPMCQueue const& other) = delete;
    MPMCQueue(MPMCQueue&& other) noexcept = delete;
    MPMCQueue& operator=(MPMCQueue const& other) = delete;
    MPMCQueue& operator=(MPMCQueue&& other) noexcept = delete;
};
}
