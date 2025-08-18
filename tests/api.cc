#include <nexus/test.hh>

#include <array>
#include <atomic>
#include <iostream>
#include <thread>
#include <vector>

#include <task-dispatcher/td.hh>

namespace
{
int gSink = 0;

void function() { ++gSink; }

void functionWithArgs(int a, int b, int c) { gSink += (a + b + c); }

int functionWithReturn() { return ++gSink; }

int functionWithArgsAndReturn(int a, int b, int c)
{
    gSink += (a + b + c);
    return gSink;
}

struct DummyType
{
    int mValue = 0;

    void method() { ++mValue; }

    void methodWithArgs(int a, int b, int c) { mValue += (a + b + c); }

    void overloadedMethod() { ++mValue; }
    void overloadedMethod(void*) { ++mValue; }
};
}

TEST("td API - compilation", exclusive)
{
    /// Aims to cover the entire API surface, making compilation or logic errors visible
    /// Little to no asserts

    // Launch variants
    {
        CHECK(!td::isInsideScheduler());

        // simple
        td::launch([] { CHECK(td::isInsideScheduler()); });

        // rvalue config
        td::launch(td::SchedulerConfig{}, [] { CHECK(td::isInsideScheduler()); });

        // lvalue config
        td::SchedulerConfig conf;
        td::launch(conf, [] { CHECK(td::isInsideScheduler()); });

        CHECK(!td::isInsideScheduler());
    }

    td::launch([] {
        REQUIRE(td::isInsideScheduler());

        // single submission
        {
            (void)0; // for clang-format

            td::AutoCounter s1;

            // regular lambdas or function pointers

            // Without arguments
            td::submitLambda(s1, [] {});
            td::submitLambda(
                s1, +[] {});
            td::submitLambda(s1, function);

            // advanced callables with arguments
            td::submitCallable(
                s1, [](float arg) { gSink += int(arg); }, 1.f);
            td::submitCallable(s1, functionWithArgs, 4, 5, 6);

            // methods
            DummyType dummy;
            REQUIRE(dummy.mValue == 0);

            // methods can be submitted directly
            td::submitMethod(s1, &dummy, &DummyType::method);
            // arguments are immediately moved into the task
            td::submitMethod(s1, &dummy, &DummyType::methodWithArgs, 1, 2, 3);
            // overloaded methods must be cast to a specific function pointer type
            td::submitMethod(s1, &dummy, static_cast<void (DummyType::*)(void*)>(&DummyType::overloadedMethod), nullptr);

            td::waitForCounter(s1);

            CHECK(dummy.mValue == 1 + (1 + 2 + 3) + 1);

            // redundant wait
            td::waitForCounter(s1);
        }

        // batched submission
        {
            cc::allocator* scratch = cc::system_allocator;
            td::AutoCounter s1;

            // submits 50 tasks called with i = 0 ... 49
            td::submitNumbered(
                s1,
                [](uint32_t i) {
                    // this is a single task
                    gSink += i;
                },
                50, scratch);

            // submits up to 16 tasks called on equally sized ranges inside 0 to 499
            td::submitBatched(
                s1,
                [](uint32_t begin, uint32_t end, uint32_t /*batchIdx*/) {
                    // this is a single task responsible for a certain range in 0 .. 499
                    for (auto i = begin; i < end; ++i)
                    {
                        gSink += i;
                    }
                },
                500, 16, scratch);

            // submits up to 16 tasks calling the lambda once for each element in batches
            int values[50] = {};
            td::submitBatchedOnArray<int>(
                s1,
                [](int& val, uint32_t /*elementIdx*/, uint32_t /*batchIdx*/) {
                    // this is called for each element in the task's batch range
                    ++val;
                },
                values, 16, scratch);

            td::waitForCounter(s1);
        }

        // move-only arguments and lambdas
        {
            auto u1 = std::make_unique<int>(1);
            auto u2 = std::make_unique<int>(2);
            auto u3 = std::make_unique<int>(3);
            auto u4 = std::make_unique<int>(4);

            auto l_moveonly = [u = std::move(u1)] { gSink += *u; };

            td::AutoCounter s;
            td::submitLambda(s, std::move(l_moveonly));
            td::submitCallable(
                s, [](std::unique_ptr<int> const& u) { gSink += *u; }, u2);

            td::submitCallable(
                s, [](std::unique_ptr<int> const& u) { gSink += *u; }, std::move(u3));

            td::waitForCounter(s);
        }
    });

    CHECK(!td::isInsideScheduler());
}


TEST("td API - consistency", exclusive)
{
    /// Basic consistency and sanity checks, WIP

    CHECK(!td::isInsideScheduler());

    td::launch([] {
        CHECK(td::isInsideScheduler());

        auto const main_thread_id = std::this_thread::get_id();

        // Staying on the main thread when using pinned wait
        auto const num_tasks = td::getNumLogicalCPUCores() * 50;
        for (auto i = 0; i < 50; ++i)
        {
            td::AutoCounter s1;
            td::submitNumbered(
                s1,
                [](uint32_t) {
                    //
                    ++gSink;
                },
                num_tasks, cc::system_allocator);

            td::waitForCounter(s1);
            CHECK(std::this_thread::get_id() == main_thread_id);
        }

        // Nested Multi-wait
        {
            td::AutoCounter s1, s2, s3;
            int a = 0, b = 0, c = 0;

            td::submitLambda(s1, [&] {
                td::submitLambda(s2, [&] {
                    CC_ASSERT(a == 0);
                    CC_ASSERT(b == 0);
                    CC_ASSERT(c == 0);
                });

                td::waitForCounter(s2);

                a = 1;
                b = 2;
                c = 3;
            });

            td::submitLambda(s3, [&] {
                td::waitForCounter(s1);
                CC_ASSERT(a == 1);
                CC_ASSERT(b == 2);
                CHECK(c == 3);
            });

            td::waitForCounter(s3);

            CHECK(!s1.handle.isValid());
            CHECK(!s2.handle.isValid());
            CHECK(!s3.handle.isValid());
            CHECK(std::this_thread::get_id() == main_thread_id);
        }

        // Chained dependency
        {
            enum
            {
                Num = 50
            };

            int res_array[Num] = {};
            td::AutoCounter syncs[Num] = {};

            for (auto i = 0; i < Num; ++i)
            {
                CHECK(res_array[i] == 0);
                if (i < Num - 1)
                {
                    td::submitLambda(syncs[i], [dat_ptr = res_array, i] { dat_ptr[i] = i; });
                }

                if (i > 0)
                {
                    td::waitForCounter(syncs[i - 1]);
                    CHECK(res_array[i - 1] == i - 1);
                }
            }

            td::waitForCounter(syncs[Num - 1]);

            CHECK(std::this_thread::get_id() == main_thread_id);
        }

        // Number of executions
        {
            enum
            {
                Num = 500
            };

            std::atomic_int counter = {0};
            REQUIRE(counter.load() == 0);

            td::AutoCounter s;
            td::submitNumbered(
                s,
                [&](uint32_t /*idx*/) { //
                    counter.fetch_add(1);
                },
                Num, cc::system_allocator);
            td::waitForCounter(s);

            CHECK(counter.load() == Num);

            CHECK(std::this_thread::get_id() == main_thread_id);
        }

        // submit_each
        {
            int values[30] = {};

            for (auto v : values)
                CHECK(v == 0);

            td::AutoCounter s;
            td::submitBatchedOnArray<int>(
                s,
                [](int& v, uint32_t /*elementIdx*/, uint32_t /*batchIdx*/) {
                    //
                    v = 1;
                },
                values, 16, cc::system_allocator);

            td::waitForCounter(s);

            for (auto v : values)
            {
                CHECK(v == 1);
            }
        }
    });

    CHECK(!td::isInsideScheduler());
}
