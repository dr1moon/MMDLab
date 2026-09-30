#include "Runtime/Core/ComputeThreadsGroup.h"
#include "Runtime/Core/TestFramework.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <mutex>
#include <set>
#include <stdexcept>
#include <thread>
#include <vector>

MMDLAB_TEST(Core.ComputeThreadsGroup, RunsEveryIndexExactlyOnce)
{
    MmdLab::ComputeThreadsGroup group(3);
    constexpr std::size_t kCount = 1000;
    std::vector<std::atomic<int>> hits(kCount);

    group.ParallelFor(kCount, [&](const std::size_t i) { hits[i].fetch_add(1); });

    bool allOnce = true;
    for (const std::atomic<int>& hit : hits)
    {
        allOnce = allOnce && hit.load() == 1;
    }
    MMDLAB_CHECK(allOnce);
}

MMDLAB_TEST(Core.ComputeThreadsGroup, EmptyBatchRunsNothing)
{
    MmdLab::ComputeThreadsGroup group(2);
    std::atomic<int> calls{ 0 };

    group.ParallelFor(0, [&](std::size_t) { calls.fetch_add(1); });

    MMDLAB_CHECK_EQUAL(0, calls.load());
}

MMDLAB_TEST(Core.ComputeThreadsGroup, SingleIndexRunsOnCallingThread)
{
    MmdLab::ComputeThreadsGroup group(2);
    std::thread::id ranOn;

    group.ParallelFor(1, [&](std::size_t) { ranOn = std::this_thread::get_id(); });

    MMDLAB_CHECK(ranOn == std::this_thread::get_id());
}

MMDLAB_TEST(Core.ComputeThreadsGroup, ZeroWorkersRunsSeriallyOnCallingThread)
{
    MmdLab::ComputeThreadsGroup group(0);
    const std::thread::id caller = std::this_thread::get_id();
    std::vector<std::size_t> order;
    bool allOnCaller = true;

    group.ParallelFor(5, [&](const std::size_t i)
    {
        allOnCaller = allOnCaller && std::this_thread::get_id() == caller;
        order.push_back(i);
    });

    MMDLAB_CHECK_EQUAL(static_cast<std::size_t>(0), group.WorkerCount());
    MMDLAB_CHECK(allOnCaller);
    MMDLAB_CHECK((order == std::vector<std::size_t>{ 0, 1, 2, 3, 4 }));
}

MMDLAB_TEST(Core.ComputeThreadsGroup, SpreadsIndicesAcrossThreads)
{
    // Every index blocks until all of them have started, which can only happen when the batch
    // runs on as many threads as it has indices (the caller plus every worker). The deadline keeps
    // a broken group from hanging the test run.
    constexpr std::size_t kWorkers = 3;
    MmdLab::ComputeThreadsGroup group(kWorkers);
    std::atomic<std::size_t> arrived{ 0 };
    std::mutex threadsMutex;
    std::set<std::thread::id> threads;

    group.ParallelFor(kWorkers + 1, [&](std::size_t)
    {
        {
            std::lock_guard lock(threadsMutex);
            threads.insert(std::this_thread::get_id());
        }
        arrived.fetch_add(1);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (arrived.load() < kWorkers + 1 && std::chrono::steady_clock::now() < deadline)
        {
            std::this_thread::yield();
        }
    });

    MMDLAB_CHECK_EQUAL(kWorkers + 1, threads.size());
}

MMDLAB_TEST(Core.ComputeThreadsGroup, RethrowsFailureAfterEveryIndexFinishes)
{
    MmdLab::ComputeThreadsGroup group(3);
    constexpr std::size_t kCount = 64;
    std::atomic<std::size_t> finished{ 0 };
    bool threw = false;

    try
    {
        group.ParallelFor(kCount, [&](const std::size_t i)
        {
            if (i == 7)
            {
                throw std::runtime_error("index 7 failed");
            }
            finished.fetch_add(1);
        });
    }
    catch (const std::runtime_error&)
    {
        threw = true;
    }

    MMDLAB_CHECK(threw);
    MMDLAB_CHECK_EQUAL(kCount - 1, finished.load());
}

MMDLAB_TEST(Core.ComputeThreadsGroup, RunsManyBatchesBackToBack)
{
    // Short batches in a tight loop exercise the hand-off between batches: a worker that wakes
    // late must not run a batch that has already been joined, or run the same batch twice.
    MmdLab::ComputeThreadsGroup group(4);
    constexpr std::size_t kBatches = 2000;
    constexpr std::size_t kCount = 6;
    std::atomic<std::size_t> total{ 0 };
    bool everyBatchComplete = true;

    for (std::size_t batch = 0; batch < kBatches; ++batch)
    {
        std::atomic<std::size_t> done{ 0 };
        group.ParallelFor(kCount, [&](std::size_t) { done.fetch_add(1); total.fetch_add(1); });
        everyBatchComplete = everyBatchComplete && done.load() == kCount;
    }

    MMDLAB_CHECK(everyBatchComplete);
    MMDLAB_CHECK_EQUAL(kBatches * kCount, total.load());
}

MMDLAB_TEST(Core.ComputeThreadsGroup, StopIsIdempotent)
{
    MmdLab::ComputeThreadsGroup group(2);
    std::atomic<int> calls{ 0 };
    group.ParallelFor(4, [&](std::size_t) { calls.fetch_add(1); });

    group.Stop();
    group.Stop();

    MMDLAB_CHECK_EQUAL(static_cast<std::size_t>(0), group.WorkerCount());
    MMDLAB_CHECK_EQUAL(4, calls.load());
}
