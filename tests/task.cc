#include <nexus/test.hh>

#include <array>
#include <memory>

#include <task-dispatcher/common/system_info.hh>
#include <task-dispatcher/container/Task.hh>

namespace
{
int gSink = 0;

void test_func(void* userdata) { gSink += *static_cast<int*>(userdata); }
}

TEST("td::Task (lifetime)")
{
    // Constants are not constexpr variables because
    // lambda captures are a concern of this test.
    // Could be randomized.
#define SHARED_INT_VALUE 55

    td::Task task;
    std::weak_ptr<int> weakInt;

    for (auto i = 0; i < 3; ++i)
    {
        bool taskExecuted = false;

        {
            std::shared_ptr<int> sharedInt = std::make_shared<int>(SHARED_INT_VALUE);
            weakInt = sharedInt;

            task.initWithLambda([sharedInt, &taskExecuted]() {
                // Check if sharedInt is alive and correct
                CHECK(*sharedInt == SHARED_INT_VALUE);

                taskExecuted = true;
            });

            // sharedInt runs out of scope
        }

        // Check if task kept the lambda capture alive
        CHECK(!weakInt.expired());
        CHECK(!taskExecuted);

        task.runTask();

        // Check if execute_and_cleanup destroyed the lambda capture
        CHECK(taskExecuted);
        CHECK(weakInt.expired());
    }
#undef SHARED_INT_VALUE
}

TEST("td::Task (static)")
{
    // Lambdas
    {
        int constexpr a = 0, b = 1, c = 2;
        int x = 3, y = 4;
        auto uptr = std::make_unique<int>(1);

        auto l_trivial = [] { ++gSink; };
        auto l_ref_cap = [&] { gSink += (a - b + c); };
        auto l_val_cap = [=] { gSink += (a - b + c); };
        auto l_val_cap_mutable = [=]() mutable { gSink += (x += y); };
        auto l_noexcept = [&]() noexcept { gSink += (a - b + c); };
        auto l_constexpr = [=]() constexpr { return a - b + c; };
        auto l_noncopyable = [p = std::move(uptr)] { gSink += *p; };

        // Test if these lambda types compile
        td::Task(std::move(l_trivial)).runTask();
        td::Task(std::move(l_ref_cap)).runTask();
        td::Task(std::move(l_val_cap)).runTask();
        td::Task(std::move(l_val_cap_mutable)).runTask();
        td::Task(std::move(l_noexcept)).runTask();
        td::Task(std::move(l_constexpr)).runTask();
        td::Task(std::move(l_noncopyable)).runTask();
    }

    // Function pointers
    {
        {
            gSink = 0;
            auto expected_stack = 0;
            auto stack_increment = 5;

            auto const check_increment = [&]() {
                expected_stack += stack_increment;
                CHECK(gSink == expected_stack);
            };

            auto l_decayable = [](void* userdata) { gSink += *static_cast<int*>(userdata); };

            CHECK(gSink == expected_stack);

            td::Task(l_decayable, &stack_increment).runTask();
            check_increment();

            td::Task([](void* userdata) { gSink += *static_cast<int*>(userdata); }, &stack_increment).runTask();
            check_increment();

            td::Task(
                +[](void* userdata) { gSink += *static_cast<int*>(userdata); }, &stack_increment)
                .runTask();
            check_increment();

            td::Task(test_func, &stack_increment).runTask();
            check_increment();
        }

        // Function pointer variant, takes lambdas and function pointers void(void*)
        td::Task([](void*) {}).runTask();
        td::Task(+[](void*) {}).runTask();
        td::Task([](void*) {}, nullptr).runTask();
        td::Task(
            +[](void*) {}, nullptr)
            .runTask();

        // Lambda variant, takes lambdas and function pointers void()
        td::Task([] {}).runTask();
        // td::Task(+[] {}).execute_and_cleanup(); // ERROR
        // td::Task([] {}, nullptr).execute_and_cleanup(); // ERROR
        // td::Task(+[] {}, nullptr).execute_and_cleanup(); // ERROR
    }
}

TEST("td::Task (metadata)")
{
    enum
    {
        TASK_CANARY_INITIAL = 20,
        CAPTURE_PAD_SIZE = 32,
    };

    td::Task task;

    using metadata_t = decltype(td::Task::mMetadata);
    for (auto testMetadata : {metadata_t(0), metadata_t(100), metadata_t(255), metadata_t(-1)})
    {
        metadata_t taskRunCanary = TASK_CANARY_INITIAL;

        std::array<char, CAPTURE_PAD_SIZE> pad;
        std::fill(pad.begin(), pad.end(), 0);

        task.mMetadata = testMetadata;

        task.initWithLambda([testMetadata, &taskRunCanary, pad]() {
            // Sanity
            CHECK(taskRunCanary == TASK_CANARY_INITIAL);

            taskRunCanary = testMetadata;

            for (auto i = 0u; i < CAPTURE_PAD_SIZE; ++i)
            {
                // Check if the pad does not collide with the metadata
                CHECK(pad[i] == 0);
            }
        });

        // Test if the task write didn't compromise the metadata
        CHECK(task.mMetadata == testMetadata);

        // Sanity
        CHECK(taskRunCanary == TASK_CANARY_INITIAL);

        task.runTask();

        // Test if the task ran correctly
        CHECK(taskRunCanary == testMetadata);

        // Test if the metadata survived
        CHECK(task.mMetadata == testMetadata);
    }
}
