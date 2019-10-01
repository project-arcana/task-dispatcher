#include "scheduler.hh"

#include <cassert>
#include <limits> // Only for sanity check static_asserts

#include "native/fiber.hh"
#include "native/thread.hh"

thread_local td::Scheduler* td::Scheduler::s_current_scheduler = nullptr;

namespace td
{
struct Scheduler::atomic_counter_t
{
    struct waiting_fiber_t
    {
        uint32_t counter_target = 0;               // the counter value that this fiber is waiting for
        fiber_index_t fiber_index = invalid_fiber; // index of the waiting fiber
        std::atomic_bool in_use{true};             // whether this slot in the array is currently being processed
    };

    std::atomic<uint32_t> count; // The value of this counter

    static auto constexpr max_waiting = 16;
    waiting_fiber_t waiting_fibers[max_waiting];
    std::atomic_bool free_waiting_slots[max_waiting];

    // Resets this counter for re-use
    void reset()
    {
        count.store(0, std::memory_order_release);

        for (auto i = 0; i < max_waiting; ++i)
            free_waiting_slots[i].store(true);
    }

    friend td::Scheduler;
};

enum class Scheduler::fiber_destination_e : uint8_t
{
    none,
    waiting,
    pool
};

struct Scheduler::worker_fiber_t
{
    native::fiber_t native;

    // True if this fiber is currently waiting (called yield_to_fiber with destination waiting)
    // and has been cleaned up by the fiber it yielded to (via clean_up_prev_fiber)
    std::atomic_bool is_waiting_cleaned_up{false};
};

struct Scheduler::tls_t
{
    native::fiber_t thread_fiber; // thread fiber, not part of scheduler::_fibers

    fiber_index_t current_fiber_index = invalid_fiber;
    fiber_index_t previous_fiber_index = invalid_fiber;

    fiber_destination_e previous_fiber_dest = fiber_destination_e::none;
};

}

namespace
{
thread_local td::Scheduler::tls_t s_tls;
}

namespace td
{
struct Scheduler::callback_funcs
{
    struct primary_fiber_arg_t
    {
        Scheduler* owning_scheduler;
        container::Task main_job;
    };

    static TD_NATIVE_THREAD_FUNC_DECL worker_func(void* arg_void)
    {
        Scheduler* scheduler = static_cast<class td::Scheduler*>(arg_void);
        scheduler->s_current_scheduler = scheduler;

        // Set up thread fiber
        native::create_main_fiber(s_tls.thread_fiber);

        {
            s_tls.current_fiber_index = scheduler->acquire_free_fiber();
            auto& fiber = scheduler->_fibers[s_tls.current_fiber_index].native;

            native::switch_to_fiber(fiber, s_tls.thread_fiber);
        }

        native::delete_main_fiber(s_tls.thread_fiber);
        native::end_current_thread();

        TD_NATIVE_THREAD_FUNC_END;
    }

    static void fiber_func(void* arg_void)
    {
        Scheduler* scheduler = static_cast<class td::Scheduler*>(arg_void);
        scheduler->clean_up_prev_fiber();

        container::Task job;
        while (!scheduler->_shutting_down.load(std::memory_order_acquire))
        {
            if (scheduler->get_next_job(job))
            {
                // Received a job, execute it
                job.execute_and_cleanup();

                // The job returned, decrement the counter
                scheduler->counter_decrement(scheduler->_counters[job.get_metadata()], 1);
            }
            else
            {
                // Job queue is empty, sleep 1ms to reduce contention
                native::thread_sleep(1);
            }
        }

        // Switch back to thread fiber of the current thread
        native::switch_to_fiber(s_tls.thread_fiber, scheduler->_fibers[s_tls.current_fiber_index].native);

        // KW_PANIC_IF(true, "Reached end of fiber_func");
    }

    static void primary_fiber_func(void* arg_void)
    {
        primary_fiber_arg_t& arg = *static_cast<primary_fiber_arg_t*>(arg_void);

        // Run main task
        arg.main_job.execute_and_cleanup();

        // Shut down
        arg.owning_scheduler->_shutting_down.store(true, std::memory_order_release);

        // Return to main thread fiber
        native::switch_to_fiber(s_tls.thread_fiber, arg.owning_scheduler->_fibers[s_tls.current_fiber_index].native);

        // KW_PANIC_IF(true, "Reached end of primary_fiber_func");
    }
};
}

td::Scheduler::fiber_index_t td::Scheduler::acquire_free_fiber()
{
    fiber_index_t res;
    for (auto attempt = 0;; ++attempt)
    {
        if (_idle_fibers.dequeue(res))
            return res;

        if (attempt > 10)
            fprintf(stderr, "Scheduler warning: Failing to find free fiber, possibly deadlocked\n");
    }
}

td::Scheduler::counter_index_t td::Scheduler::acquire_free_counter()
{
    counter_index_t free_counter;
    auto success = _free_counters.dequeue(free_counter);
    // KW_PANIC_IF(!success, "No free counters available, consider increasing config.max_num_counters");
    _counters[free_counter].reset();
    return free_counter;
}

void td::Scheduler::yield_to_fiber(td::Scheduler::fiber_index_t target_fiber, td::Scheduler::fiber_destination_e own_destination)
{
    s_tls.previous_fiber_index = s_tls.current_fiber_index;
    s_tls.previous_fiber_dest = own_destination;
    s_tls.current_fiber_index = target_fiber;

    //    KW_DEBUG_PANIC_IF(s_tls.previous_fiber_index == invalid_fiber || s_tls.current_fiber_index == invalid_fiber, "Switching to or from an invalid "
    //                                                                                                                 "fiber");
    //    KW_LOG_DIAG("[yield_to_fiber] Switching from " << int(s_tls.previous_fiber_index) << " ("
    //                                                   << (own_destination == fiber_destination_e::pool ? "pooling" : "waiting") << ") to "
    //                                                   << int(s_tls.current_fiber_index));

    native::switch_to_fiber(_fibers[s_tls.current_fiber_index].native, _fibers[s_tls.previous_fiber_index].native);
    clean_up_prev_fiber();
}

void td::Scheduler::clean_up_prev_fiber()
{
    switch (s_tls.previous_fiber_dest)
    {
    case fiber_destination_e::none:
        return;
    case fiber_destination_e::pool:
        // The fiber is being pooled, add it to the idle fibers
        _idle_fibers.enqueue(s_tls.previous_fiber_index);
        break;
    case fiber_destination_e::waiting:
        // The fiber is waiting for a dependency, and can be safely resumed from now on
        // KW_LOG_DIAG("[clean_up_prev_fiber] Waiting fiber " << s_tls.previous_fiber_index << " cleaned up");
        _fibers[s_tls.previous_fiber_index].is_waiting_cleaned_up.store(true, std::memory_order_relaxed);
        break;
    }

    s_tls.previous_fiber_index = invalid_fiber;
    s_tls.previous_fiber_dest = fiber_destination_e::none;
}

bool td::Scheduler::get_next_job(td::container::Task& job)
{
    // Sleeping fibers with jobs that had their dependencies resolved in the meantime
    // have the highest priority
    fiber_index_t resumable_fiber_index;
    bool got_resumable = _resumable_fibers.dequeue(resumable_fiber_index);
    if (got_resumable)
    {
        bool expected = true;
        auto const casSuccess = std::atomic_compare_exchange_strong_explicit(&_fibers[resumable_fiber_index].is_waiting_cleaned_up, &expected, false, //
                                                                             std::memory_order_seq_cst, std::memory_order_relaxed);
        if ((casSuccess)) // TODO: TD_LIKELY
        {
            // is_waiting_cleaned_up was true, and is now exchanged to false
            // The resumable fiber is properly cleaned up and can be switched to

            // KW_LOG_DIAG("[get_next_job] Acquired resumable fiber " << int(resumable_fiber_index) << " which is cleaned up, yielding");
            yield_to_fiber(resumable_fiber_index, fiber_destination_e::pool);

            // returned, now fall through to next job acquisition
            // TODO: Think about if this shouldn't retry immediately
        }
        else
        {
            // The resumable fiber is not yet cleaned up

            // This should only happen if _resumable_fibers is almost empty, and
            // the latency impact is low in those cases
            //            KW_LOG_DIAG("[get_next_job] Acquired resumable fiber " << int(resumable_fiber_index) << ", not cleaned up, re-enqueueing");
            _resumable_fibers.enqueue(resumable_fiber_index);

            // Sleep 1ms before falling through to next job acquisition to avoid hammering this queue
            // (scenario: _jobs is empty and _resumable_fibers only contains a single fiber which isn't cleaned up)
            native::thread_sleep(1);
        }
    }

    return _jobs.dequeue(job);
}

bool td::Scheduler::counter_add_waiting_fiber(td::Scheduler::atomic_counter_t& counter, fiber_index_t fiber_index, uint32_t counter_target)
{
    for (auto i = 0u; i < atomic_counter_t::max_waiting; ++i)
    {
        // Acquire free waiting slot
        bool expected = true;
        if (!std::atomic_compare_exchange_strong_explicit(&counter.free_waiting_slots[i], &expected, false, //
                                                          std::memory_order_seq_cst, std::memory_order_relaxed))
            continue;

        atomic_counter_t::waiting_fiber_t& slot = counter.waiting_fibers[i];
        slot.fiber_index = fiber_index;
        slot.counter_target = counter_target;
        slot.in_use.store(false);

        // Check if already done
        auto counter_val = counter.count.load(std::memory_order_relaxed);
        if (slot.in_use.load(std::memory_order_acquire))
            return false;

        if (slot.counter_target == counter_val)
        {
            expected = false;
            if (!std::atomic_compare_exchange_strong_explicit(&slot.in_use, &expected, true, //
                                                              std::memory_order_seq_cst, std::memory_order_relaxed))
                return false;

            counter.free_waiting_slots[i].store(true, std::memory_order_release);
            return true;
        }

        return false;
    }

    // Panic if there is no space left in conter waiting_slots
    //    KW_PANIC_IF(true, "Counter waiting slots are full");
    return false;
}

void td::Scheduler::counter_check_waiting_fibers(td::Scheduler::atomic_counter_t& counter, uint32_t value)
{
    // Go over each waiting fiber slot
    for (auto i = 0u; i < atomic_counter_t::max_waiting; ++i)
    {
        // Skip free slots
        if (counter.free_waiting_slots[i].load(std::memory_order_acquire))
            continue;

        auto& slot = counter.waiting_fibers[i];

        // Skip the slot if it is in use already
        if (slot.in_use.load(std::memory_order_acquire))
            continue;

        // If this slot's dependency is met
        if (slot.counter_target == value)
        {
            // Lock the slot to be used by this thread
            bool expected = false;
            if (!std::atomic_compare_exchange_strong_explicit(&slot.in_use, &expected, true, //
                                                              std::memory_order_seq_cst, std::memory_order_relaxed))
                // Failed to lock, this slot is already being handled on a different thread (which stole it right between the two checks)
                continue;

            //            KW_LOG_DIAG("[cnst_check_waiting_fibers] Counter reached " << value << ", making waiting fiber " << slot.fiber_index << " resumable");


            // The waiting fiber is ready, and locked by this thread, store it in _resumable_fibers
            bool success = _resumable_fibers.enqueue(slot.fiber_index);

            // Panic if there is no space left in TLS ready_fibers
            // This should never happen
            //            KW_PANIC_IF(!success, "_resumable_fibers full");

            // Free the slot
            counter.free_waiting_slots[i].store(true, std::memory_order_release);
        }
    }
}

void td::Scheduler::counter_increment(td::Scheduler::atomic_counter_t& counter, uint32_t amount)
{
    auto previous = counter.count.fetch_add(amount);
    counter_check_waiting_fibers(counter, previous + amount);
}

void td::Scheduler::counter_decrement(td::Scheduler::atomic_counter_t& counter, uint32_t amount)
{
    auto previous = counter.count.fetch_sub(amount);
    counter_check_waiting_fibers(counter, previous - amount);
}

td::Scheduler::Scheduler(scheduler_config const& config)
  : _fiber_stack_size(config.fiber_stack_size),
    _num_threads(static_cast<thread_index_t>(config.num_threads)),
    _num_fibers(static_cast<fiber_index_t>(config.num_fibers)),
    _num_counters(static_cast<counter_index_t>(config.max_num_counters)),
    _jobs(config.max_num_jobs),
    _idle_fibers(_num_fibers),
    _resumable_fibers(_num_fibers), // TODO: Smaller?
    _free_counters(config.max_num_counters)
{
    //    KW_PANIC_IF(!config.is_valid(), "Scheduler config invalid, use scheduler_config_t::validate()");
    //    KW_PANIC_IF(config.num_threads > system::hardware_concurrency, "More threads than physical cores configured");

    static_assert(ATOMIC_INT_LOCK_FREE == 2 && ATOMIC_BOOL_LOCK_FREE == 2, "No lock-free atomics available on this platform");
    static_assert(invalid_fiber == std::numeric_limits<fiber_index_t>().max(), "Invalid fiber index corrupt");
    static_assert(invalid_thread == std::numeric_limits<thread_index_t>().max(), "Invalid thread index corrupt");
    static_assert(invalid_counter == std::numeric_limits<counter_index_t>().max(), "Invalid counter index corrupt");
}

void td::Scheduler::submitTasks(td::container::Task* jobs, uint32_t num_jobs, td::sync& sync)
{
    counter_index_t counter_index;
    if (sync.initialized)
    {
        // Initialized handle, read its counter index
//        KW_PANIC_IF(_counter_handles.is_expired(sync.handle), "Attempted to run jobs using an expired sync, consider increasing "
//                                                              "scheduler::max_handles_in_flight");
        counter_index = _counter_handles.get(sync.handle);
    }
    else
    {
        // Unitialized handle, acquire a free counter and link it to the handle
        counter_index = acquire_free_counter();
        sync.handle = _counter_handles.acquire(counter_index);
        sync.initialized = true;
    }

    counter_increment(_counters[counter_index], num_jobs);

    // TODO: Multi-enqueue
    bool success = true;
    for (auto i = 0u; i < num_jobs; ++i)
    {
        jobs[i].set_metadata(counter_index);
        success &= _jobs.enqueue(jobs[i]);
    }

//    KW_PANIC_IF(!success, "Job queue is full, consider increasing config.max_num_jobs");
}

void td::Scheduler::wait(td::sync& sync, uint32_t target)
{
    // Skip uninitialized sync handles
    if (!sync.initialized)
    {
//        KW_LOG_DIAG("[wait] Waiting on uninitialized sync, resuming immediately");
        return;
    }

    if (_counter_handles.isExpired(sync.handle))
    {
//        KW_PANIC_IF(true, "Attempted to wait on an expired sync, consider increasing scheduler::max_handles_in_flight");
    }

    auto const counter_index = _counter_handles.get(sync.handle);

    // The current fiber is now waiting, but not yet cleaned up
    _fibers[s_tls.current_fiber_index].is_waiting_cleaned_up.store(false, std::memory_order_release);

    if (counter_add_waiting_fiber(_counters[counter_index], s_tls.current_fiber_index, target))
    {
        // Already done
//        KW_LOG_DIAG("[wait] Wait for counter " << int(counter_index) << " is over early, resuming immediately");
    }
    else
    {
        // Not already done, prepare to yield
//        KW_LOG_DIAG("[wait] Waiting for counter " << int(counter_index) << ", yielding");
        yield_to_fiber(acquire_free_fiber(), fiber_destination_e::waiting);
    }

    // Either the counter was already on target, or this fiber has been awakened because it is now on target,
    // return execution

    // If the counter has reached zero, free it for re-use and de-initialize the sync handle
    if (_counters[counter_index].count.load(std::memory_order_acquire) == 0)
    {
        _free_counters.enqueue(counter_index);
        sync.initialized = false;
    }
}

void td::Scheduler::start(td::container::Task main_task)
{
    _threads = new native::thread_t[_num_threads];
    _fibers = new worker_fiber_t[_num_fibers];
    _counters = new atomic_counter_t[_num_counters];
    _shutting_down.store(false, std::memory_order_seq_cst);

    // Initialize main thread variables, create the thread fiber
    // The main thread is thread 0 by convention
    auto& main_thread = _threads[0];
    {
        // Receive native thread handle, lock to core 0
        main_thread = native::get_current_thread();
        native::set_current_thread_affinity(0);

        s_current_scheduler = this;

        // Create main fiber on this thread
        native::create_main_fiber(s_tls.thread_fiber);

//        kw::dev::log::set_current_thread_index(0);
    }

    // Populate fiber pool
    for (fiber_index_t i = 0; i < _num_fibers; ++i)
    {
        native::create_fiber(_fibers[i].native, callback_funcs::fiber_func, this, _fiber_stack_size);
        _idle_fibers.enqueue(i);
    }

    // Populate free counter queue
    for (counter_index_t i = 0; i < _num_counters; ++i)
    {
        _free_counters.enqueue(i);
    }

    // Launch worker threads, starting at 1
    {
        for (thread_index_t i = 1; i < _num_threads; ++i)
        {
            native::thread_t& thread = _threads[i];

            // TODO: Adjust this stack size
            // On Win 10 1803 and Linux 4.18 this seems to be entirely irrelevant
            auto constexpr thread_stack_overhead_safety = sizeof(void*) * 16;
            if (!native::create_thread(uint32_t(_fiber_stack_size) + thread_stack_overhead_safety, callback_funcs::worker_func, this, i, &thread))
            {
                //      KW_PANIC_IF(true, "Failed to create worker thread");
            }
        }
    }

    // Prepare the primary fiber
    {
        // Prepare the args for the primary fiber
        callback_funcs::primary_fiber_arg_t primary_fiber_arg;
        primary_fiber_arg.owning_scheduler = this;
        primary_fiber_arg.main_job = main_task;

        s_tls.current_fiber_index = acquire_free_fiber();
        auto& initial_fiber = _fibers[s_tls.current_fiber_index];

        // reset the fiber, creating the primary fiber
        native::delete_fiber(initial_fiber.native);
        native::create_fiber(initial_fiber.native, callback_funcs::primary_fiber_func, &primary_fiber_arg, _fiber_stack_size);

        // Launch the primary fiber
        native::switch_to_fiber(initial_fiber.native, s_tls.thread_fiber);
    }

    // The primary fiber has returned, begin shutdown
    {
        // Spin until shutdown has propagated
        while (_shutting_down.load(std::memory_order_seq_cst) != true)
        {
            // Spin
        }

        // Delete the main fiber
        native::delete_main_fiber(s_tls.thread_fiber);

        // Join worker threads, starting at 1
        for (auto i = 1u; i < _num_threads; ++i)
            native::join_thread(_threads[i]);

        // Clean up
        {
            for (auto i = 0u; i < _num_fibers; ++i)
                native::delete_fiber(_fibers[i].native);

            // Free arrays
            delete[] _threads;
            delete[] _fibers;
            delete[] _counters;

            // Empty queues
            container::Task job_dump;
            fiber_index_t fiber_dump;
            counter_index_t counter_dump;
            while (_jobs.dequeue(job_dump))
                ;
            while (_idle_fibers.dequeue(fiber_dump))
                ;
            while (_resumable_fibers.dequeue(fiber_dump))
                ;
            while (_free_counters.dequeue(counter_dump))
                ;

            // Reset counter handles
            _counter_handles.reset();

            // Clear s_current_scheduler
            s_current_scheduler = nullptr;
        }
    }
}
