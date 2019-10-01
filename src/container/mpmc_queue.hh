#pragma once

#include <atomic>
#include <common/system_info.hh>
#include <cstdint>

namespace td::container
{
// Alloc-free MPMC queue, ~75 cycles per enqueue and dequeue
// http://www.1024cores.net/home/lock-free-algorithms/queues/bounded-mpmc-queue
template <typename T>
class MPMCQueue
{
public:
    MPMCQueue(size_t buffer_size) : _buffer(new cell_t[buffer_size]), buffer_mask_(buffer_size - 1)
    {
        // KW_DEBUG_PANIC_IF((buffer_size < 2) || ((buffer_size & (buffer_size - 1)) != 0), "mpmc_queue size not a power of two");
        for (size_t i = 0; i != buffer_size; i += 1)
            _buffer[i].sequence_.store(i, std::memory_order_relaxed);
        _enqueue_pos.store(0, std::memory_order_relaxed);
        _dequeue_pos.store(0, std::memory_order_relaxed);
    }

    ~MPMCQueue() { delete[] _buffer; }

    bool enqueue(const T& data)
    {
        cell_t* cell;
        size_t pos = _enqueue_pos.load(std::memory_order_relaxed);
        for (;;)
        {
            cell = &_buffer[pos & buffer_mask_];
            size_t seq = cell->sequence_.load(std::memory_order_acquire);
            intptr_t diff = static_cast<intptr_t>(seq) - static_cast<intptr_t>(pos);
            if (diff == 0)
            {
                if (_enqueue_pos.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed))
                    break;
            }
            else if (diff < 0)
                return false;
            else
                pos = _enqueue_pos.load(std::memory_order_relaxed);
        }
        cell->data_ = data;
        cell->sequence_.store(pos + 1, std::memory_order_release);
        return true;
    }

    bool dequeue(T& data)
    {
        cell_t* cell;
        size_t pos = _dequeue_pos.load(std::memory_order_relaxed);
        for (;;)
        {
            cell = &_buffer[pos & buffer_mask_];
            size_t seq = cell->sequence_.load(std::memory_order_acquire);
            intptr_t diff = static_cast<intptr_t>(seq) - static_cast<intptr_t>(pos + 1);
            if (diff == 0)
            {
                if (_dequeue_pos.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed))
                    break;
            }
            else if (diff < 0)
                return false;
            else
                pos = _dequeue_pos.load(std::memory_order_relaxed);
        }
        data = cell->data_;
        cell->sequence_.store(pos + buffer_mask_ + 1, std::memory_order_release);
        return true;
    }

private:
    struct cell_t
    {
        std::atomic<size_t> sequence_;
        T data_;
    };

    alignas(system::l1_cacheline_size) cell_t* const _buffer;
    size_t const buffer_mask_;

    alignas(system::l1_cacheline_size) std::atomic<size_t> _enqueue_pos;
    alignas(system::l1_cacheline_size) std::atomic<size_t> _dequeue_pos;

    MPMCQueue(MPMCQueue const& other) = delete;
    MPMCQueue(MPMCQueue&& other) noexcept = delete;
    MPMCQueue& operator=(MPMCQueue const& other) = delete;
    MPMCQueue& operator=(MPMCQueue&& other) noexcept = delete;
};
}
