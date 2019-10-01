#pragma once

#include <atomic>
#include <common/system_info.hh>
#include <cstdint>

#include "container/mpmc_queue.hh"
#include "container/task.hh"
#include "container/version_ring.hh"
#include "sync.hh"

namespace td
{
namespace native
{
struct thread_t;
struct fiber_t;
}

struct scheduler_config
{
    uint32_t num_fibers = 128;
    uint32_t num_threads = system::hardware_concurrency;
    uint32_t max_num_counters = 128;
    uint32_t max_num_jobs = 2048;
    size_t fiber_stack_size = 64 * 1024;

    // Some values in this config must be a power of 2
    // Round up all values to the next power of 2
    void ceil_to_pow2();

    // Check for internal consistency
    bool is_valid() const;

    // ceil_to_pow2 + is_valid
    bool validate();
};

// Fiber-based job scheduler
// Never allocates after the main task starts executing, supports custom allocators
// Never throws or behaves erroneously, crashes immediately on any fatal error
// ::run_jobs and ::wait must only be called from scheduler tasks
// td::sync objects passed to ::run_jobs must eventually be waited upon using ::wait
class Scheduler
{
public:
    struct tls_t;

public:
    explicit Scheduler(scheduler_config const& config = scheduler_config());

    // Launch the scheduler with the given main task
    void start(container::Task main_task);

    // Enqueue the given tasks and associate them with a sync object
    void submitTasks(container::Task* jobs, uint32_t num_jobs, td::sync& sync);
    // Resume execution after the given sync object has reached a set target
    void wait(td::sync& sync, uint32_t target = 0);

    // The scheduler running the current task
    static Scheduler& current() { return *s_current_scheduler; }
    // Returns true if called from inside the scheduler
    static bool isInsideScheduler() { return s_current_scheduler != nullptr; }

private:
    static thread_local Scheduler* s_current_scheduler;

private:
    using fiber_index_t = uint16_t;
    using thread_index_t = uint8_t;
    using counter_index_t = uint16_t;
    static auto constexpr invalid_fiber = fiber_index_t(-1);
    static auto constexpr invalid_thread = thread_index_t(-1);
    static auto constexpr invalid_counter = counter_index_t(-1);

private:
    enum class fiber_destination_e : uint8_t;
    struct worker_fiber_t;
    struct atomic_counter_t;

private:
    size_t const _fiber_stack_size;
    std::atomic_bool _shutting_down = {false};

    // Threads
    native::thread_t* _threads;
    thread_index_t const _num_threads;

    // Fibers
    worker_fiber_t* _fibers;
    fiber_index_t const _num_fibers;

    // Counters
    atomic_counter_t* _counters;
    counter_index_t const _num_counters;

    // Queues
    container::MPMCQueue<container::Task> _jobs;
    container::MPMCQueue<fiber_index_t> _idle_fibers;
    container::MPMCQueue<fiber_index_t> _resumable_fibers;
    container::MPMCQueue<counter_index_t> _free_counters;

    // User handles
    static auto constexpr max_handles_in_flight = 512;
    container::VersionRing<counter_index_t, max_handles_in_flight> _counter_handles;

private:
    // Callbacks, wrapped into a friend struct for member access
    struct callback_funcs;
    friend struct callback_funcs;
    friend struct scheduler_config;

private:
    fiber_index_t acquire_free_fiber();
    counter_index_t acquire_free_counter();

    void yield_to_fiber(fiber_index_t target_fiber, fiber_destination_e own_destination);
    void clean_up_prev_fiber();

    bool get_next_job(container::Task& job);

    bool counter_add_waiting_fiber(atomic_counter_t& counter, fiber_index_t fiber_index, uint32_t counter_target);
    void counter_check_waiting_fibers(atomic_counter_t& counter, uint32_t value);

    void counter_increment(atomic_counter_t& counter, uint32_t amount = 1);
    void counter_decrement(atomic_counter_t& counter, uint32_t amount = 1);

    Scheduler(Scheduler const& other) = delete;
    Scheduler(Scheduler&& other) noexcept = delete;
    Scheduler& operator=(Scheduler const& other) = delete;
    Scheduler& operator=(Scheduler&& other) noexcept = delete;
};

}
