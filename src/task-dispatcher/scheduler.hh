#pragma once

#include <atomic>
#include <cstdint>

#include <clean-core/atomic_linked_pool.hh>
#include <clean-core/function_ptr.hh>
#include <clean-core/fwd_array.hh>
#include <clean-core/typedefs.hh>

#include <task-dispatcher/common/api.hh>
#include <task-dispatcher/common/system_info.hh>
#include <task-dispatcher/container/mpmc_queue.hh>
#include <task-dispatcher/container/task.hh>
#include <task-dispatcher/sync.hh>

namespace td
{
namespace native
{
struct thread_t;
struct fiber_t;
struct event_t;
}

struct TD_API scheduler_config
{
    /// amount of fibers created
    /// limits the amount of concurrently waiting tasks
    uint32_t num_fibers = 256;

    /// amount of threads used
    /// scheduler creates (num - 1) worker threads, the OS thread calling start() is the main thread
    uint32_t num_threads = system::num_logical_cores();

    /// amount of atomic counters created
    /// limits the amount of concurrently live td::sync objects (lifetime: from first submit() to wait())
    uint32_t max_num_counters = 512;

    /// amount of tasks that can be concurrently in flight
    uint32_t max_num_tasks = 4096;

    /// stack size of each fiber in bytes
    uint32_t fiber_stack_size = 64 * 1024;

    /// whether to lock the main and worker threads to logical cores
    /// recommended on console-like plattforms
    /// can degrade performance on a multitasking (desktop) OS depending on other process load
    bool pin_threads_to_cores = false;

    /// functions that is called by each worker thread once at launch and once at shutdown,
    /// called as func(worker_index, is_start, userdata)
    cc::function_ptr<void(uint32_t, bool, void*)> worker_thread_startstop_function = nullptr;
    void* worker_thread_startstop_userdata = nullptr;

    /// function that is used as a SEH filter in the global __try/__except block of each fiber (Win32 only)
    /// argument is EXCEPTION_POINTERS*
    /// returns CONTINUE_EXECUTION (-1), CONTINUE_SEARCH (0), or EXECUTE_HANDLER (1)
    cc::function_ptr<int32_t(void*)> fiber_seh_filter = nullptr;

public:
    /// Some values in this config must be a power of 2
    /// Round up all values to the next power of 2
    void ceil_to_pow2();

    /// Check for internal consistency
    bool is_valid() const;

    /// ceil_to_pow2 + is_valid
    bool validate();
};

// Fiber-based task scheduler
// Never allocates after the main task starts executing
// submitTasks and wait must only be called from inside scheduler tasks
// td::sync objects passed to submitTasks must eventually be waited upon using wait
class TD_API Scheduler
{
public:
    struct tls_t;

public:
    explicit Scheduler(scheduler_config const& config = scheduler_config());
    ~Scheduler();

    /// Launch the scheduler with the given main task
    void start(container::task main_task);

    /// acquire a counter
    [[nodiscard]] handle::counter acquireCounterHandle();

    /// release a counter
    /// returns the last counter state
    int releaseCounter(handle::counter c);

    /// release a counter if a target is reached
    /// returns true if the release succeeded
    [[nodiscard]] bool releaseCounterIfOnTarget(handle::counter c, int target);

    /// Enqueue the given tasks and associate them with a counter object
    void submitTasks(container::task* tasks, uint32_t num_tasks, handle::counter c);

    /// Resume execution after the given counter has reached a set target
    /// returns the counter value before the wait
    int wait(handle::counter c, bool pinnned = false, int target = 0);

    /// experimental: manually increment a counter, preventing waits to resolve
    /// returns the new counter state
    int incrementCounter(handle::counter c, uint32_t amount = 1);

    /// experimental: manually decrement a counter, potentially causing waits on it to resolve
    /// WARNING: this should not be called without prior calls to incrementCounter
    /// returns the new counter state
    int decrementCounter(handle::counter c, uint32_t amount = 1);

    /// Returns the amount of threads this scheduler controls
    [[nodiscard]] uint32_t getNumThreads() const { return uint32_t(mThreads.size()); }

    /// Returns the scheduler running the current task
    [[nodiscard]] static Scheduler& Current();
    /// Returns true if called from inside the scheduler
    [[nodiscard]] static bool IsInsideScheduler();
    /// Returns the index of the calling thread, relative to its owning scheduler. returns uint32_t(-1) on unowned threads
    [[nodiscard]] static uint32_t CurrentThreadIndex();
    /// Returns the index of the calling fiber, relative to its owning scheduler. returns uint32_t(-1) on unowned threads
    [[nodiscard]] static uint32_t CurrentFiberIndex();

public:
    using fiber_index_t = uint32_t;
    using thread_index_t = uint32_t;
    using counter_index_t = uint16_t; // Must fit into task metadata
    static auto constexpr invalid_fiber = fiber_index_t(-1);
    static auto constexpr invalid_thread = thread_index_t(-1);
    static auto constexpr invalid_counter = counter_index_t(-1);

private:
    enum class fiber_destination_e : uint8_t;
    struct worker_thread_t;
    struct worker_fiber_t;
    struct atomic_counter_t;

private:
    std::atomic_bool mIsShuttingDown = {false};
    scheduler_config mConfig;

    cc::fwd_array<worker_thread_t> mThreads;
    cc::fwd_array<worker_fiber_t> mFibers;
    cc::fwd_array<atomic_counter_t> mCounters;

    // Queues
    container::MPMCQueue<container::task> mTasks;
    container::MPMCQueue<fiber_index_t> mIdleFibers;
    container::MPMCQueue<fiber_index_t> mResumableFibers;
    container::MPMCQueue<counter_index_t> mFreeCounters;

    struct AtomicCounterHandleContent
    {
        counter_index_t counterIndex = invalid_counter;
        uint32_t pad = 0;
    };

    cc::atomic_linked_pool<AtomicCounterHandleContent, true> mCounterHandles;

    // Worker wakeup event
    native::event_t* mEventWorkAvailable;

private:
    // Callbacks, wrapped into a friend struct for member access
    struct callback_funcs;
    friend struct callback_funcs;

private:
    fiber_index_t acquireFreeFiber();

    void yieldToFiber(fiber_index_t target_fiber, fiber_destination_e own_destination);
    void cleanUpPrevFiber();

    bool getNextTask(container::task& task);
    bool tryResumeFiber(fiber_index_t fiber);

    bool counterAddWaitingFiber(atomic_counter_t& counter, fiber_index_t fiber_index, thread_index_t pinned_thread_index, int counter_target, int& out_counter_val);
    void counterCheckWaitingFibers(atomic_counter_t& counter, int value);

    int counterIncrement(atomic_counter_t& counter, int amount = 1);

    bool enqueueTasks(td::container::task* tasks, uint32_t num_tasks, handle::counter counter);

    Scheduler(Scheduler const& other) = delete;
    Scheduler(Scheduler&& other) noexcept = delete;
    Scheduler& operator=(Scheduler const& other) = delete;
    Scheduler& operator=(Scheduler&& other) noexcept = delete;
};

}
