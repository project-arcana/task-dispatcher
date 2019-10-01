#include "scheduler.hh"

#include <common/math_intrin.hh>

void td::scheduler_config_t::ceil_to_pow2()
{
    num_fibers = math::nextpow2(num_fibers);
    max_num_counters = math::nextpow2(max_num_counters);
    max_num_jobs = math::nextpow2(max_num_jobs);
}

bool td::scheduler_config_t::is_valid() const
{
    auto const nonzero = [](auto v) -> bool { return v != 0; };

    bool valid = true;

    // Nonzero
    valid &= nonzero(num_fibers);
    valid &= nonzero(num_threads);
    valid &= nonzero(max_num_counters);
    valid &= nonzero(max_num_jobs);

    // Powers of 2
    valid &= math::ispow2(num_fibers);
    valid &= math::ispow2(max_num_counters);
    valid &= math::ispow2(max_num_jobs);

    // Invalid number of fibers, threads or counters
    valid &= bool(num_fibers < scheduler::invalid_fiber);
    valid &= bool(num_threads < scheduler::invalid_thread);
    valid &= bool(max_num_counters < scheduler::invalid_counter);

    return valid;
}

bool td::scheduler_config_t::validate()
{
    ceil_to_pow2();
    return is_valid();
}
