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
    return static_alloc != nullptr && num_fibers > 0 && num_threads > 0 && max_num_counters > 0 && max_num_tasks > 0 && cc::is_pow2(num_fibers)
           && cc::is_pow2(max_num_counters) && cc::is_pow2(max_num_tasks);
}