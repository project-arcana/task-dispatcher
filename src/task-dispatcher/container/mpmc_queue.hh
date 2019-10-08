#pragma once

#include <atomic>
#include <cstdint>

#include <clean-core/assert.hh>

#include <task-dispatcher/common/system_info.hh>

namespace td::container
{
// Alloc-free MPMC queue, ~75 cycles per enqueue and dequeue
// http://www.1024cores.net/home/lock-free-algorithms/queues/bounded-mpmc-queue
// See licenses/
template <typename T>
class MPMCQueue
{
public:
    MPMCQueue(size_t buffer_size) : mBuffer(new cell[buffer_size]), mBufferMask(buffer_size - 1)
    {
        ASSERT((buffer_size >= 2) && ((buffer_size & (buffer_size - 1)) == 0) && "MPMCQueue size not a power of two");
        for (size_t i = 0; i != buffer_size; i += 1)
            mBuffer[i].sequence_.store(i, std::memory_order_relaxed);
        mEnqueuePos.store(0, std::memory_order_relaxed);
        mDequeuePos.store(0, std::memory_order_relaxed);
    }

    ~MPMCQueue() { delete[] mBuffer; }

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

    alignas(system::l1_cacheline_size) cell* const mBuffer;
    size_t const mBufferMask;

    alignas(system::l1_cacheline_size) std::atomic<size_t> mEnqueuePos;
    alignas(system::l1_cacheline_size) std::atomic<size_t> mDequeuePos;

    MPMCQueue(MPMCQueue const& other) = delete;
    MPMCQueue(MPMCQueue&& other) noexcept = delete;
    MPMCQueue& operator=(MPMCQueue const& other) = delete;
    MPMCQueue& operator=(MPMCQueue&& other) noexcept = delete;
};
}
