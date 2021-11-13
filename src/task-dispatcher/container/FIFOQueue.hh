#pragma once

#include <stdint.h>

#include <clean-core/assert.hh>

namespace td
{
// This is not synchronized at all, TODO: Replace with Yukov or similar
// what we would need here is an MPSC queue
template <class T, uint32_t N>
struct FIFOQueue
{
    static_assert(N > 0);

public:
    FIFOQueue() = default;

    bool dequeue(T& out_ref)
    {
        if (empty())
            return false;

        out_ref = mData[mTail];

        if (mTail == mHead)
        {
            // Queue now empty
            mTail = -1;
            mHead = -1;
        }
        else
        {
            // Increment tail
            ++mTail;
            if (mTail >= N)
                mTail -= N;
        }

        return true;
    }

    void enqueue(T const& val)
    {
        CC_ASSERT(!full() && "FIFOQueue Full");

        if (empty())
            mTail = 0;

        // Increment head
        ++mHead;
        if (mHead >= N)
            mHead -= N;

        mData[mHead] = val;
    }

    bool empty() const { return mTail == -1; }

    bool full() const
    {
        int next_head = mHead + 1;
        if (next_head >= N)
            next_head -= N;

        return next_head == mTail;
    }

private:
    int mHead = -1;
    int mTail = -1;
    T mData[N] = {};

    FIFOQueue(FIFOQueue const& other) = delete;
    FIFOQueue(FIFOQueue&& other) noexcept = delete;
    FIFOQueue& operator=(FIFOQueue const& other) = delete;
    FIFOQueue& operator=(FIFOQueue&& other) noexcept = delete;
};

}
