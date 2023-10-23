#pragma once

#include <type_traits>

#include <clean-core/allocator.hh>
#include <clean-core/assert.hh>
#include <clean-core/enable_if.hh>
#include <clean-core/forward.hh>
#include <clean-core/utility.hh>

#include <task-dispatcher/CounterHandle.hh>
#include <task-dispatcher/Scheduler.hh>
#include <task-dispatcher/container/Task.hh>

//
// Batched submission
//
// larger header than the raw scheduler

namespace td
{
// submits multiple tasks calling a lambda "void f(uint32_t i)" from 0 to num - 1
template <class F>
void submitNumbered(CounterHandle counter, F&& func, uint32_t numElements, cc::allocator* scratch, ETaskPriority priority = ETaskPriority::Default)
{
    static_assert(std::is_invocable_v<F, unsigned>, "function must be invocable with index argument");
    static_assert(std::is_same_v<std::invoke_result_t<F, uint32_t>, void>, "return must be void");

    td::Task* tasks = scratch->new_array_sized<td::Task>(numElements);

    for (uint32_t i = 0u; i < numElements; ++i)
    {
        tasks[i].initWithLambda([=] { func(i); });
    }

    td::submitTasks(counter, cc::span{tasks, numElements}, priority);
    scratch->delete_array_sized(tasks, numElements);
}

// submits tasks calling a lambda "void f(uint start, uint end, uint batchIdx)" for evenly sized batches from 0 to num - 1
// maxNumBatches: maximum amount of batches to partition the range into
// returns amount of batches
template <class F>
uint32_t submitBatched(CounterHandle counter, F&& func, uint32_t numElements, uint32_t maxNumBatches, cc::allocator* scratch, ETaskPriority priority = ETaskPriority::Default)
{
    static_assert(std::is_invocable_v<F, uint32_t, uint32_t, uint32_t>, "function must be invocable with element start, end, and index argument");

    if (numElements == 0 || maxNumBatches == 0)
    {
        return 0;
    }

    uint32_t const batchSize = cc::int_div_ceil(numElements, maxNumBatches);
    uint32_t const numBatches = cc::int_div_ceil(numElements, batchSize);
    CC_ASSERT(numBatches <= maxNumBatches && "programmer error");

    td::Task* tasks = scratch->new_array_sized<td::Task>(numBatches);

    for (uint32_t batch = 0u, start = 0u, end = cc::min(batchSize, numElements); //
         batch < numBatches;                                                     //
         ++batch, start = batch * batchSize, end = cc::min((batch + 1) * batchSize, numElements))
    {
        tasks[batch].initWithLambda([=] { func(start, end, batch); });
    }

    td::submitTasks(counter, cc::span{tasks, numBatches}, priority);
    scratch->delete_array_sized(tasks, numBatches);
    return numBatches;
}

// submits batched tasks calling a lambda "void f(T& value, uint32_t idx, uint32_t batchIdx)" for each element in the span
// data in 'values' must stay alive until all tasks ran through
// returns amount of batches
template <class T, class F>
uint32_t submitBatchedOnArray(CounterHandle counter, F&& func, cc::span<T> values, uint32_t maxNumBatches, cc::allocator* scratch, ETaskPriority priority = ETaskPriority::Default)
{
    static_assert(std::is_invocable_v<F, T&, uint32_t, uint32_t>, "function must be invocable with element reference");
    static_assert(std::is_same_v<std::invoke_result_t<F, T&, uint32_t, uint32_t>, void>, "return must be void");

    T* const pValues = values.data();
    uint32_t const numValues = uint32_t(values.size());

    // copy the lambda
    return td::submitBatched(
        counter,
        [func, pValues](uint32_t start, uint32_t end, uint32_t batchIdx)
        {
            for (uint32_t i = start; i < end; ++i)
            {
                func(pValues[i], i, batchIdx);
            }
        },
        numValues, maxNumBatches, scratch, priority);
}

}
