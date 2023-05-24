#include <nexus/test.hh>

#include <atomic>

#include <clean-core/array.hh>
#include <clean-core/intrinsics.hh>

#include <rich-log/log.hh>

#include <task-dispatcher/CounterHandle.hh>
#include <task-dispatcher/Scheduler.hh>
#include <task-dispatcher/SchedulerConfig.hh>
#include <task-dispatcher/container/Task.hh>

using namespace td;

namespace
{
auto constexpr spin_default = 25ull;

auto constexpr num_tasks_outer = 5u;
auto constexpr num_tasks_inner = 10u;

void spin_cycles(uint64_t cycles = spin_default)
{
    auto const current = cc::intrin_rdtsc();
    while (cc::intrin_rdtsc() - current < cycles)
    {
        _mm_pause();
    }
}

void outer_task_func(void*)
{
    std::atomic_int* const dependency = new std::atomic_int(0);

    auto s = td::acquireCounter();

    cc::array<Task, num_tasks_inner> tasks;

    for (Task& task : tasks)
    {
        task.initWithLambda(
            [dependency]()
            {
                spin_cycles();
                dependency->fetch_add(1);
            });
    }

    CC_ASSERT(dependency->load() == 0);
    submitTasks(s, tasks);

    waitForCounter(s, true);
    int lastVal = releaseCounter(s);

    CC_ASSERT(lastVal == 0);

    CC_ASSERT(dependency->load() == num_tasks_inner);

    delete dependency;
}

template <int MaxIterations>
void main_task_func(void*)
{
    static_assert(MaxIterations > 0, "at least one iteration required");


    for (auto iter = 0; iter < MaxIterations; ++iter)
    {
        auto s = acquireCounter();
        cc::array<Task, num_tasks_outer> tasks;

        for (Task& task : tasks)
        {
            task.initWithFunction(outer_task_func);
        }

        submitTasks(s, tasks);
        waitForCounter(s, true);

        bool releasedSync = releaseCounterIfOnZero(s);
        CC_ASSERT(releasedSync);
    }
}
}

TEST("td::Scheduler", exclusive)
{
    {
        SchedulerConfig config;

        // Make sure this test does not exceed the configured job limit
        REQUIRE((num_tasks_inner * num_tasks_outer) + 1 < config.maxNumTasks);
    }

    // Run a simple dependency chain
    {
        td::launchScheduler(td::SchedulerConfig{}, Task{main_task_func<1>, nullptr});
    }

    // Run multiple times
    {
        td::launchScheduler(td::SchedulerConfig{}, Task{main_task_func<25>, nullptr});
        td::launchScheduler(td::SchedulerConfig{}, Task{main_task_func<25>, nullptr});
    }

    // Run with constrained threads
    {
        SchedulerConfig config;
        config.numThreads = 2;

        td::launchScheduler(config, Task{main_task_func<150>, nullptr});
    }
}
