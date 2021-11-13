#pragma once

#include <stdint.h>

#include <clean-core/function_ptr.hh>

#include <task-dispatcher/common/api.hh>
#include <task-dispatcher/common/system_info.hh>

namespace td
{
struct TD_API SchedulerConfig
{
    // amount of fibers created
    // limits the amount of concurrently waiting tasks
    uint32_t numFibers = 256;

    // amount of threads used
    // scheduler creates (num - 1) worker threads, the OS thread calling start() is the main thread
    uint32_t numThreads = getNumLogicalCPUCores();

    // amount of atomic counters created
    // limits the amount of concurrently live td::sync objects (lifetime: from first submit() to wait())
    uint32_t maxNumCounters = 512;

    // amount of tasks that can be concurrently in flight
    uint32_t maxNumTasks = 4096;

    // stack size of each fiber in bytes
    uint32_t fiberStackSizeBytes = 64 * 1024;

    // maximum amount of fibers waiting on a single counter
    uint32_t maxNumWaitingFibersPerCounter = 256;

    // whether to lock the main and worker threads to logical cores
    // can degrade performance on a multitasking (desktop) OS depending on other process load
    // recommended on console-like plattforms, embedded systems
    bool pinThreadsToCores = false;

    // functions that is called by each worker thread once at launch and once at shutdown,
    // called as func(worker_index, is_start, userdata)
    cc::function_ptr<void(uint32_t, bool, void*)> workerThreadStartStopFunction = nullptr;
    void* workerThreadStartStopUserdata = nullptr;

    // function that is used as a SEH filter in the global __try/__except block of each fiber (Win32 only)
    // argument is EXCEPTION_POINTERS*
    // returns CONTINUE_EXECUTION (-1), CONTINUE_SEARCH (0), or EXECUTE_HANDLER (1)
    cc::function_ptr<int32_t(void*)> fiberSEHFilter = nullptr;

    // the source for all allocations, only hit during init and shutdown
    cc::allocator* staticAlloc = cc::system_allocator;

    // Some values in this config must be a power of 2
    // Round up all values to the next power of 2
    void ceilValuesToPowerOfTwo();

    // Check for internal consistency
    bool isValid() const;
};
}