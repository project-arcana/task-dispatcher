#pragma once

#include <stdint.h>

#include <clean-core/function_ptr.hh>
#include <clean-core/fwd.hh>

#include <task-dispatcher/common/api.hh>
#include <task-dispatcher/common/system_info.hh>
#include <task-dispatcher/fwd.hh>

namespace td
{
struct TD_API scheduler_config
{
    // amount of fibers created
    // limits the amount of concurrently waiting tasks
    uint32_t num_fibers = 256;

    // amount of threads used
    // scheduler creates (num - 1) worker threads, the OS thread calling start() is the main thread
    uint32_t num_threads = system::num_logical_cores();

    // amount of atomic counters created
    // limits the amount of concurrently live td::sync objects (lifetime: from first submit() to wait())
    uint32_t max_num_counters = 512;

    // amount of tasks that can be concurrently in flight
    uint32_t max_num_tasks = 4096;

    // stack size of each fiber in bytes
    uint32_t fiber_stack_size = 64 * 1024;

    // whether to lock the main and worker threads to logical cores
    // can degrade performance on a multitasking (desktop) OS depending on other process load
    // recommended on console-like plattforms, embedded systems
    bool pin_threads_to_cores = false;

    // functions that is called by each worker thread once at launch and once at shutdown,
    // called as func(worker_index, is_start, userdata)
    cc::function_ptr<void(uint32_t, bool, void*)> worker_thread_startstop_function = nullptr;
    void* worker_thread_startstop_userdata = nullptr;

    // function that is used as a SEH filter in the global __try/__except block of each fiber (Win32 only)
    // argument is EXCEPTION_POINTERS*
    // returns CONTINUE_EXECUTION (-1), CONTINUE_SEARCH (0), or EXECUTE_HANDLER (1)
    cc::function_ptr<int32_t(void*)> fiber_seh_filter = nullptr;

    // the source for all allocations, only hit during init and shutdown
    cc::allocator* static_alloc = cc::system_allocator;

    // Some values in this config must be a power of 2
    // Round up all values to the next power of 2
    void ceil_to_pow2();

    // Check for internal consistency
    bool is_valid() const;
};

TD_API void launchScheduler(scheduler_config const& config, container::task const& mainTask);

// create a counter handle
// used to associate submitted tasks with, and then wait upon completion
TD_API [[nodiscard]] handle::counter acquireCounter();

// release a counter handle, returns last counter value
TD_API int32_t releaseCounter(handle::counter counter);

// release a counter handle if it reached zero
// returns true if the counter was released
TD_API [[nodiscard]] bool releaseCounterIfOnZero(handle::counter counter);

// submit multiple tasks to the scheduler for immediate execution
// the counter is incremented by the amount of tasks and decremented once per completed task
TD_API void submitTasks(handle::counter counter, cc::span<container::task> tasks);

// waits until the counter reaches zero
// pinnned: if true, the task entering the wait can only resume on the same OS thread
// returns the counter value before the wait
TD_API int32_t waitForCounter(handle::counter counter, bool pinned = true);

// manually increment a counter, preventing waits on it to resolve
// normally, a counter is incremented by 1 for every task submitted on it
// WARNING: without subsequent calls to decrementCounter, this will deadlock wait-calls on the sync
// returns the new counter value
TD_API int32_t incrementCounter(handle::counter c, uint32_t amount = 1);

// manually decrement a sync object, potentially causing waits on it to resolve
// normally, a sync is decremented once a task submitted on it is finished
// WARNING: without previous calls to increment_sync, this will cause wait-calls to resolve before all tasks have finished
// returns the new counter value
TD_API int32_t decrementCounter(handle::counter c, uint32_t amount = 1);

// returns the amount of threads the current scheduler has
TD_API uint32_t getNumThreadsInScheduler();

// returns true if the call is being made from within a scheduler
TD_API bool isInsideScheduler();

// returns the index of the current thread, or uint32_t(-1) on unowned threads
TD_API uint32_t getCurrentThreadIndex();

// returns the index of the current fiber, or uint32_t(-1) on unowned threads
TD_API uint32_t getCurrentFiberIndex();

}
