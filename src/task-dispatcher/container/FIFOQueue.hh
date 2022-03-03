#pragma once

#include <stdint.h>

#include <clean-core/alloc_array.hh>
#include <clean-core/assert.hh>

namespace td
{
// This is not synchronized at all, TODO: Replace with Yukov or similar
// what we would need here is an MPSC queue
template <class T>
struct FIFOQueue
{
public:
    FIFOQueue() = default;

    void initialize(size_t size, cc::allocator* alloc)
    {
        CC_ASSERT(mData.empty() && size > 0 && alloc);
        mData.reset(alloc, size);
    }

    bool dequeue(T& out_ref)
    {
        CC_ASSERT(!mData.empty());

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
            mTail = cc::wrapped_increment(mTail, int(mData.size()));
        }

        return true;
    }

    void enqueue(T const& val)
    {
        CC_ASSERT(!mData.empty());

        CC_ASSERT(!full() && "FIFOQueue Full");

        if (empty())
            mTail = 0;

        // Increment head
        mHead = cc::wrapped_increment(mHead, int(mData.size()));

        mData[mHead] = val;
    }

    bool empty() const { return mTail == -1; }

    bool full() const
    {
        int next_head = cc::wrapped_increment(mHead, int(mData.size()));

        return next_head == mTail;
    }

private:
    int mHead = -1;
    int mTail = -1;

    cc::alloc_array<T> mData;

    FIFOQueue(FIFOQueue const& other) = delete;
    FIFOQueue(FIFOQueue&& other) noexcept = delete;
    FIFOQueue& operator=(FIFOQueue const& other) = delete;
    FIFOQueue& operator=(FIFOQueue&& other) noexcept = delete;
};

}
