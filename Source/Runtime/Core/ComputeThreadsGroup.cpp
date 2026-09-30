#include "Runtime/Core/ComputeThreadsGroup.h"

#include "Runtime/Core/Log.h"
#include "Runtime/Core/Runnable.h"
#include "Runtime/Core/Thread.h"

#include <algorithm>
#include <atomic>
#include <format>
#include <string>

namespace MmdLab
{
// One fork of work, owned by the submitting thread for the whole ParallelFor call. It outlives
// every claim because the submitter returns only after all workers have left it.
struct ComputeThreadsGroup::BatchState
{
    const std::function<void(std::size_t)>* body = nullptr;
    std::size_t count = 0;
    std::atomic<std::size_t> next{ 0 }; // Next unclaimed index.
    std::mutex failureMutex;
    std::exception_ptr failure;         // First exception seen, rethrown by the submitter.
};

// One compute worker: it idles between batches, joins each published batch once, claims indices
// from it until none are left, and exits when the group stops.
class ComputeThreadsGroup::Worker final : public Runnable
{
public:
    explicit Worker(ComputeThreadsGroup& group)
        : group_(group)
    {
    }

    uint32_t Run() override
    {
        std::uint64_t lastGeneration = 0;
        while (true)
        {
            BatchState* batch = nullptr;
            {
                // A worker that wakes after its batch was already joined sees `batch_` cleared or
                // a generation it has served, and goes back to sleep instead of touching it.
                std::unique_lock lock(group_.mutex_);
                group_.wake_.wait(lock, [&] {
                    return group_.stopping_ || (group_.batch_ != nullptr && group_.generation_ != lastGeneration);
                });
                if (group_.stopping_)
                {
                    return 0;
                }
                batch = group_.batch_;
                lastGeneration = group_.generation_;
                ++group_.activeWorkers_;
            }

            group_.RunBatch(*batch);

            {
                std::lock_guard lock(group_.mutex_);
                --group_.activeWorkers_;
            }
            group_.joined_.notify_one();
        }
    }

    void Stop() override
    {
        {
            std::lock_guard lock(group_.mutex_);
            group_.stopping_ = true;
        }
        group_.wake_.notify_all();
    }

private:
    ComputeThreadsGroup& group_;
};

ComputeThreadsGroup::ComputeThreadsGroup(const std::size_t workerCount)
{
    LogInfo("Core", std::format("ComputeThreadsGroup: {} worker(s)", workerCount));

    workers_.reserve(workerCount);
    threads_.reserve(workerCount);
    for (std::size_t i = 0; i < workerCount; ++i)
    {
        workers_.push_back(std::make_unique<Worker>(*this));
        threads_.push_back(std::make_unique<Thread>(
            *workers_.back(), L"ComputeThreadsGroup-" + std::to_wstring(i)));
    }
}

ComputeThreadsGroup::~ComputeThreadsGroup()
{
    Stop();
}

void ComputeThreadsGroup::Stop()
{
    {
        std::lock_guard lock(mutex_);
        stopping_ = true;
    }
    wake_.notify_all();
    threads_.clear(); // Each Thread's destructor asks its worker to stop and joins it.
    workers_.clear();
}

void ComputeThreadsGroup::ParallelFor(const std::size_t count, const std::function<void(std::size_t)>& body)
{
    if (count == 0)
    {
        return;
    }
    BatchState batch;
    batch.body = &body;
    batch.count = count;

    // A batch of one is not worth a thread handoff: it runs inline with the same exception
    // contract, so the one-model scene pays nothing for the group. So does a group with no
    // workers.
    if (count == 1 || threads_.empty())
    {
        RunBatch(batch);
    }
    else
    {
        {
            std::lock_guard lock(mutex_);
            batch_ = &batch;
            ++generation_;
        }
        // Wake only as many workers as there are indices beyond the one the caller takes. Each
        // wake costs the caller a kernel transition, so waking the whole group delays the caller's
        // own first claim until the woken workers have already taken every index, and the surplus
        // workers wake only to find nothing left.
        const std::size_t wakeCount = std::min(count - 1, threads_.size());
        for (std::size_t i = 0; i < wakeCount; ++i)
        {
            wake_.notify_one();
        }

        // The submitting thread claims indices like a worker rather than idling while it waits.
        RunBatch(batch);

        // Join. Unpublish first so no late-waking worker can join, then wait for the ones that
        // did. A worker leaves only after it has seen the index counter run past `count`, so once
        // none are active every index has been claimed and has finished.
        std::unique_lock lock(mutex_);
        batch_ = nullptr;
        joined_.wait(lock, [this] { return activeWorkers_ == 0; });
    }

    if (batch.failure != nullptr)
    {
        std::rethrow_exception(batch.failure);
    }
}

void ComputeThreadsGroup::RunBatch(BatchState& batch)
{
    while (true)
    {
        const std::size_t index = batch.next.fetch_add(1, std::memory_order_relaxed);
        if (index >= batch.count)
        {
            return;
        }
        try
        {
            (*batch.body)(index);
        }
        catch (...)
        {
            // Keep the first failure for the submitting thread. It must never escape a worker,
            // where an uncaught exception would terminate the process.
            std::lock_guard lock(batch.failureMutex);
            if (batch.failure == nullptr)
            {
                batch.failure = std::current_exception();
            }
        }
    }
}
} // namespace MmdLab
