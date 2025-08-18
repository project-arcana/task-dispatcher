#include <nexus/test.hh>

#include <atomic>

#include <task-dispatcher/native/fiber.hh>

namespace
{
struct MultipleFiberArg
{
    uint64_t Counter{0};
    td::native::fiber_t MainFiber;
    td::native::fiber_t FirstFiber;
    td::native::fiber_t SecondFiber;
    td::native::fiber_t ThirdFiber;
    td::native::fiber_t FourthFiber;
    td::native::fiber_t FifthFiber;
    td::native::fiber_t SixthFiber;
};

void FirstLevelFiberStart(void* arg)
{
    auto* singleFiberArg = reinterpret_cast<MultipleFiberArg*>(arg);

    singleFiberArg->Counter += 8;
    td::native::switchToFiber(singleFiberArg->SecondFiber, singleFiberArg->FirstFiber);

    // Return from sixth
    // We just finished 1 -> 2 -> 3 -> 4 -> 5 -> 6 -> 1
    // Intermediate check
    CHECK(((((((0ULL + 8ULL) * 3ULL) + 7ULL) * 6ULL) - 9ULL) * 2ULL) == singleFiberArg->Counter);

    // Now run the rest of the sequence
    singleFiberArg->Counter *= 4;
    td::native::switchToFiber(singleFiberArg->FifthFiber, singleFiberArg->FirstFiber);

    // Return from fifth
    singleFiberArg->Counter += 1;
    td::native::switchToFiber(singleFiberArg->ThirdFiber, singleFiberArg->FirstFiber);

    // We should never get here
    CHECK(false);
}

void SecondLevelFiberStart(void* arg)
{
    auto* singleFiberArg = reinterpret_cast<MultipleFiberArg*>(arg);

    singleFiberArg->Counter *= 3;
    td::native::switchToFiber(singleFiberArg->ThirdFiber, singleFiberArg->SecondFiber);

    // Return from third
    singleFiberArg->Counter += 9;
    td::native::switchToFiber(singleFiberArg->FourthFiber, singleFiberArg->SecondFiber);

    // Return from fourth
    singleFiberArg->Counter += 7;
    td::native::switchToFiber(singleFiberArg->FifthFiber, singleFiberArg->SecondFiber);

    // We should never get here
    CHECK(false);
}

void ThirdLevelFiberStart(void* arg)
{
    auto* singleFiberArg = reinterpret_cast<MultipleFiberArg*>(arg);

    singleFiberArg->Counter += 7;
    td::native::switchToFiber(singleFiberArg->FourthFiber, singleFiberArg->ThirdFiber);

    // Return from first
    singleFiberArg->Counter *= 3;
    td::native::switchToFiber(singleFiberArg->SecondFiber, singleFiberArg->ThirdFiber);

    // Return from fifth
    singleFiberArg->Counter *= 6;
    td::native::switchToFiber(singleFiberArg->SixthFiber, singleFiberArg->ThirdFiber);

    // We should never get here
    CHECK(false);
}

void FourthLevelFiberStart(void* arg)
{
    auto* singleFiberArg = reinterpret_cast<MultipleFiberArg*>(arg);

    singleFiberArg->Counter *= 6;
    td::native::switchToFiber(singleFiberArg->FifthFiber, singleFiberArg->FourthFiber);

    // Return from second
    singleFiberArg->Counter += 8;
    td::native::switchToFiber(singleFiberArg->SixthFiber, singleFiberArg->FourthFiber);

    // Return from sixth
    singleFiberArg->Counter *= 5;
    td::native::switchToFiber(singleFiberArg->SecondFiber, singleFiberArg->FourthFiber);

    // We should never get here
    CHECK(false);
}

void FifthLevelFiberStart(void* arg)
{
    auto* singleFiberArg = reinterpret_cast<MultipleFiberArg*>(arg);

    singleFiberArg->Counter -= 9;
    td::native::switchToFiber(singleFiberArg->SixthFiber, singleFiberArg->FifthFiber);

    // Return from first
    singleFiberArg->Counter *= 5;
    td::native::switchToFiber(singleFiberArg->FirstFiber, singleFiberArg->FifthFiber);

    // Return from second
    singleFiberArg->Counter += 1;
    td::native::switchToFiber(singleFiberArg->ThirdFiber, singleFiberArg->FifthFiber);

    // We should never get here
    CHECK(false);
}

void SixthLevelFiberStart(void* arg)
{
    auto* singleFiberArg = reinterpret_cast<MultipleFiberArg*>(arg);

    singleFiberArg->Counter *= 2;
    td::native::switchToFiber(singleFiberArg->FirstFiber, singleFiberArg->SixthFiber);

    // Return from fourth
    singleFiberArg->Counter -= 9;
    td::native::switchToFiber(singleFiberArg->FourthFiber, singleFiberArg->SixthFiber);

    // Return from third
    singleFiberArg->Counter -= 3;
    td::native::switchToFiber(singleFiberArg->MainFiber, singleFiberArg->SixthFiber);

    // We should never get here
    CHECK(false);
}
}

TEST("td::native::fiber (nested)", exclusive)
{
    auto const kHalfMebibyte = 524288;
    auto* const alloc = cc::system_allocator;

    MultipleFiberArg singleFiberArg;
    singleFiberArg.Counter = 0ULL;
    td::native::createMainFiber(&singleFiberArg.MainFiber);

    td::native::createFiber(&singleFiberArg.FirstFiber, FirstLevelFiberStart, &singleFiberArg, kHalfMebibyte, alloc);
    td::native::createFiber(&singleFiberArg.SecondFiber, SecondLevelFiberStart, &singleFiberArg, kHalfMebibyte, alloc);
    td::native::createFiber(&singleFiberArg.ThirdFiber, ThirdLevelFiberStart, &singleFiberArg, kHalfMebibyte, alloc);
    td::native::createFiber(&singleFiberArg.FourthFiber, FourthLevelFiberStart, &singleFiberArg, kHalfMebibyte, alloc);
    td::native::createFiber(&singleFiberArg.FifthFiber, FifthLevelFiberStart, &singleFiberArg, kHalfMebibyte, alloc);
    td::native::createFiber(&singleFiberArg.SixthFiber, SixthLevelFiberStart, &singleFiberArg, kHalfMebibyte, alloc);

    // The order should be:
    // 1 -> 2 -> 3 -> 4 -> 5 -> 6 -> 1 -> 5 -> 1 -> 3 -> 2 -> 4 -> 6 -> 4 -> 2 -> 5 -> 3 -> 6 -> Main

    td::native::switchToFiber(singleFiberArg.FirstFiber, singleFiberArg.MainFiber);

    td::native::deleteFiber(singleFiberArg.FirstFiber, alloc);
    td::native::deleteFiber(singleFiberArg.SecondFiber, alloc);
    td::native::deleteFiber(singleFiberArg.ThirdFiber, alloc);
    td::native::deleteFiber(singleFiberArg.FourthFiber, alloc);
    td::native::deleteFiber(singleFiberArg.FifthFiber, alloc);
    td::native::deleteFiber(singleFiberArg.SixthFiber, alloc);
    td::native::deleteMainFiber(singleFiberArg.MainFiber);

    CHECK(((((((((((((((((((0ULL + 8ULL) * 3ULL) + 7ULL) * 6ULL) - 9ULL) * 2ULL) * 4) * 5) + 1) * 3) + 9) + 8) - 9) * 5) + 7) + 1) * 6) - 3)
          == singleFiberArg.Counter);
}
