#include <array>
#include <container/spmc_queue.hh>
#include <iostream>
#include <td.hh>
#include <common/math_intrin.hh>

namespace
{
void argfun(int a, int b, int c) { printf("Argfun: %d %d %d \n", a, b, c); }

struct Foo
{
    void process_args(int a, int b) { printf("Foo process %d %d \n", a, b); }
};

template <class F, class FObj, class... Args>
void execute(F&& func, FObj& inst, Args&&... args)
{
    (inst.*func)(args...);
}

int calculate_fibonacci(int n)
{
    if (n < 2)
        return n;
    else
    {
        auto f1 = td::submit(calculate_fibonacci, n - 1);
        auto f2 = td::submit(calculate_fibonacci, n - 2);
        return f1.get_unpinned() + f2.get_unpinned();
    }
}

double fac(double num)
{
    double result = 1.0;
    for (double i = 2.0; i < num; i++)
        result *= i;
    return result;
}

double chudnovsky(double k_start, double k_end)
{
    auto res = 0.0;
    for (double k = k_start; k < k_end; k++)
    {
        res += (pow(-1.0, k) * fac(6.0 * k) * (13591409.0 + (545140134.0 * k))) / (fac(3.0 * k) * pow(fac(k), 3.0) * pow(640320.0, 3.0 * k + 3.0 / 2.0));
    }
    return res * 12.0;
}

double calculate_pi(int k, int num_batches_target)
{
    auto batch_size = td::int_div_ceil(k, num_batches_target);
    auto num_batches = td::int_div_ceil(k, batch_size);

    auto batchStorage = new double[num_batches];
    for (auto i = 0; i < num_batches; ++i)
        batchStorage[i] = 0.0;

    auto sync = td::submit_n(
        [batchStorage, batch_size](auto i) {
            auto k_start = double(i * batch_size);
            auto k_end = k_start + double(batch_size);
            batchStorage[i] = chudnovsky(k_start, k_end);
        },
        num_batches);

    td::wait_for_unpinned(sync);

    auto res = 0.0;
    for (auto i = 0; i < num_batches; ++i)
        res += batchStorage[i];

    delete[] batchStorage;

    return 1.0 / res;
}

}

int main()
{
    td::scheduler_config config;
//    config.max_num_jobs = 1000000;
    td::launch(config, [&] {
        if(0)
        {
            auto s1 = td::submit([] { printf("Task 1\n"); });
            td::submit(s1, [] { printf("Task 1 - append \n"); });
            td::submit(s1, argfun, 1, 2, 3);

            // TODO
            //        Foo f;
            //        td::submit(s1, &Foo::process_args, f, 15, 16);

            td::wait_for_unpinned(s1);
        }

        for (auto _ = 0; _ < 10; ++_)
        {
            auto largeTaskAmount = new td::container::Task[config.max_num_jobs];
            for (auto i = 0u; i < config.max_num_jobs; ++i)
                largeTaskAmount[i].lambda([] { /* SWYM */ });

            auto before = td::intrin::rdtsc();
            auto s = td::submit_raw(largeTaskAmount, config.max_num_jobs);
            auto after = td::intrin::rdtsc();

            std::cout << "Average dispatch time: " << (after - before) / double(config.max_num_jobs) << " cycles" << std::endl;

            delete[] largeTaskAmount;

            auto before_wait = td::intrin::rdtsc();
            td::wait_for_unpinned(s);
            auto after_wait = td::intrin::rdtsc();

            std::cout << "Wait time: " << (after_wait - before_wait) << " cycles" << std::endl;

        }

        return;

        {
            auto f1 = td::submit([] { return 5.f * 15.f; });
            std::cout << "Future 1: " << f1.get_unpinned() << std::endl;

            std::cout << "PI: " << calculate_pi(1000, 64) << std::endl;
        }

        {
            td::sync s2;

            td::submit(s2, [] {
                printf("Task 2 start \n");

                auto s2_i = td::submit_n([](auto i) { printf("Task 2 - inner %d \n", i); }, 4);

                td::wait_for_unpinned(s2_i);

                printf("Task 2 end \n");
            });

            td::submit(s2, [] { printf("Task 3 \n"); });

            td::wait_for_unpinned(s2);
        }

        {
            td::sync s3;
            td::submit_n(
                s3,
                [](auto i) {
                    printf("Task 4 - %d start \n", i);

                    auto s4_i = td::submit_n(
                        [i](auto i_inner) {
                            //
                            printf("Task 4 - %d - inner %d \n", i, i_inner);
                        },
                        4);

                    printf("Task 4 - %d wait \n", i);
                    td::wait_for_unpinned(s4_i);
                },
                4);

            td::wait_for_unpinned(s3);
        }
    });


    return 0;
}
