#include "scheduler.hh"

#include <clean-core/bits.hh>

void td::scheduler_config::ceil_to_pow2()
{
    num_fibers = cc::ceil_pow2(num_fibers);
    max_num_counters = cc::ceil_pow2(max_num_counters);
    max_num_tasks = cc::ceil_pow2(max_num_tasks);
}

bool td::scheduler_config::is_valid() const
{
    if (!static_alloc)
    {
        return false;
    }

    auto f_is_positive = [](auto v) -> bool { return v > 0; };

    bool valid = true;

    // Greater than zero
    valid &= f_is_positive(num_fibers);
    valid &= f_is_positive(num_threads);
    valid &= f_is_positive(max_num_counters);
    valid &= f_is_positive(max_num_tasks);

    // Powers of 2
    valid &= cc::is_pow2(num_fibers);
    valid &= cc::is_pow2(max_num_counters);
    valid &= cc::is_pow2(max_num_tasks);

    // Valid number of fibers, threads and counters
    valid &= bool(num_fibers < Scheduler::invalid_fiber);
    valid &= bool(num_threads < Scheduler::invalid_thread);
    valid &= bool(max_num_counters < Scheduler::invalid_counter);

    return valid;
}

bool td::scheduler_config::validate()
{
    ceil_to_pow2();
    return is_valid();
}
