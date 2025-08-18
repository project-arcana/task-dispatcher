#include <nexus/test.hh>

#include <array>
#include <memory>

#include <task-dispatcher/native/fiber.hh>

namespace
{
using data_block_t = std::array<int, 50>;

void fillData(data_block_t& dat)
{
    for (auto i = 0u; i < dat.size(); ++i)
        dat[i] = int(i) * 2;
};

bool checkData(data_block_t const& dat)
{
    for (auto i = 0u; i < dat.size(); ++i)
        if (dat[i] != int(i) * 2)
            return false;

    return true;
};

struct test_fiber_arg
{
    int counter = 0;
    td::native::fiber_t mainFiber;
    td::native::fiber_t otherFiber;
};

void test_fiber_func(void* arg)
{
    auto* const singleFiberArg = static_cast<test_fiber_arg*>(arg);

    CHECK(singleFiberArg->counter == 0);
    ++singleFiberArg->counter;
    CHECK(singleFiberArg->counter == 1);

    // Check stack survival
    {
        data_block_t stack_data;
        std::shared_ptr<data_block_t> shared_data = std::make_shared<data_block_t>();
        std::unique_ptr<data_block_t> unique_data = std::make_unique<data_block_t>();

        fillData(stack_data);
        fillData(*shared_data);
        fillData(*unique_data);

        td::native::switchToFiber(singleFiberArg->mainFiber, singleFiberArg->otherFiber);

        CHECK(checkData(stack_data));
        CHECK(checkData(*shared_data));
        CHECK(checkData(*unique_data));
    }

    // Returned

    CHECK(singleFiberArg->counter == 2);
    singleFiberArg->counter = 50;
    CHECK(singleFiberArg->counter == 50);

    td::native::switchToFiber(singleFiberArg->mainFiber, singleFiberArg->otherFiber);

    // We should never get here
    CHECK(false);
}
}

TEST("td::native::fiber", exclusive)
{
    auto constexpr kHalfMebibyte = 524288;

    test_fiber_arg singleFiberArg;
    singleFiberArg.counter = 0;

    CHECK(singleFiberArg.counter == 0);

    td::native::createMainFiber(&singleFiberArg.mainFiber);

    CHECK(singleFiberArg.counter == 0);

    td::native::createFiber(&singleFiberArg.otherFiber, test_fiber_func, &singleFiberArg, kHalfMebibyte, cc::system_allocator);

    CHECK(singleFiberArg.counter == 0);

    {
        // Check local stack survival
        {
            data_block_t stack_data;
            std::shared_ptr<data_block_t> shared_data = std::make_shared<data_block_t>();
            std::unique_ptr<data_block_t> unique_data = std::make_unique<data_block_t>();

            fillData(stack_data);
            fillData(*shared_data);
            fillData(*unique_data);

            td::native::switchToFiber(singleFiberArg.otherFiber, singleFiberArg.mainFiber);

            CHECK(checkData(stack_data));
            CHECK(checkData(*shared_data));
            CHECK(checkData(*unique_data));
        }

        CHECK(singleFiberArg.counter == 1);

        ++singleFiberArg.counter;

        td::native::switchToFiber(singleFiberArg.otherFiber, singleFiberArg.mainFiber);

        CHECK(singleFiberArg.counter == 50);
    }

    td::native::deleteFiber(singleFiberArg.otherFiber, cc::system_allocator);
    td::native::deleteMainFiber(singleFiberArg.mainFiber);
}
