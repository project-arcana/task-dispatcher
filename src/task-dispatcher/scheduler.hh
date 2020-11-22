#pragma once

#include <atomic>

#include <clean-core/atomic_linked_pool.hh>
#include <clean-core/fwd_array.hh>
#include <clean-core/typedefs.hh>

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

struct scheduler_config
{
    /// amount of fibers created
    /// limits the amount of concurrently waiting tasks
    unsigned num_fibers = 256;

    /// amount of threads used
    /// scheduler creates (num - 1) worker threads, the OS thread calling start() is the main thread
    unsigned num_threads = system::num_logical_cores();

    /// amount of atomic counters created
    /// limits the amount of concurrently live td::sync objects (lifetime: from first submit() to wait())
    unsigned max_num_counters = 512;

    /// amount of tasks that can be concurrently in flight
    unsigned max_num_tasks = 4096;

    /// stack size of each fiber in bytes
    cc::size_t fiber_stack_size = 64 * 1024;

    /// whether to lock the main and worker threads to logical cores
    /// recommended on console-like plattforms
    /// can degrade performance on a multitasking (desktop) OS depending on other process load
    bool pin_threads_to_cores = false;

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
class Scheduler
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
    void submitTasks(container::task* tasks, unsigned num_tasks, handle::counter c);

    /// Resume execution after the given counter has reached a set target
    /// returns the counter value before the wait
    void wait(handle::counter c, bool pinnned = false, int target = 0);

    /// experimental: manually increment a counter, preventing waits to resolve
    /// returns the new counter state
    int incrementCounter(handle::counter c, unsigned amount = 1);

    /// experimental: manually decrement a counter, potentially causing waits on it to resolve
    /// WARNING: this should not be called without prior calls to incrementCounter
    /// returns the new counter state
    int decrementCounter(handle::counter c, unsigned amount = 1);

    /// Returns the amount of threads this scheduler controls
    [[nodiscard]] unsigned getNumThreads() const { return unsigned(mThreads.size()); }

    /// Returns the scheduler running the current task
    [[nodiscard]] static Scheduler& Current() { return *sCurrentScheduler; }
    /// Returns true if called from inside the scheduler
    [[nodiscard]] static bool IsInsideScheduler() { return sCurrentScheduler != nullptr; }
    /// Returns the index of the calling thread, relative to its owning scheduler. returns unsigned(-1) on unowned threads
    [[nodiscard]] static unsigned CurrentThreadIndex();
    /// Returns the index of the calling fiber, relative to its owning scheduler. returns unsigned(-1) on unowned threads
    [[nodiscard]] static unsigned CurrentFiberIndex();


private:
    static thread_local Scheduler* sCurrentScheduler;

public:
    using fiber_index_t = unsigned;
    using thread_index_t = unsigned;
    using counter_index_t = cc::uint16; // Must fit into task metadata
    static auto constexpr invalid_fiber = fiber_index_t(-1);
    static auto constexpr invalid_thread = thread_index_t(-1);
    static auto constexpr invalid_counter = counter_index_t(-1);

private:
    enum class fiber_destination_e : cc::uint8;
    struct worker_thread_t;
    struct worker_fiber_t;
    struct atomic_counter_t;

private:
    size_t const mFiberStackSize;
    unsigned const mNumCounters;
    bool const mEnablePinThreads;
    std::atomic_bool mIsShuttingDown = {false};

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

    bool counterAddWaitingFiber(atomic_counter_t& counter, fiber_index_t fiber_index, thread_index_t pinned_thread_index, int counter_target);
    void counterCheckWaitingFibers(atomic_counter_t& counter, int value);

    int counterIncrement(atomic_counter_t& counter, int amount = 1);

    bool enqueueTasks(td::container::task* tasks, unsigned num_tasks, handle::counter counter);

    Scheduler(Scheduler const& other) = delete;
    Scheduler(Scheduler&& other) noexcept = delete;
    Scheduler& operator=(Scheduler const& other) = delete;
    Scheduler& operator=(Scheduler&& other) noexcept = delete;
};

}
