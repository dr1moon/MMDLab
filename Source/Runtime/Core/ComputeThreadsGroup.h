#pragma once

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>

namespace MmdLab
{
class Thread;

// The compute execution resource: a group of worker threads that run independent CPU-bound jobs
// on behalf of the rest of the runtime. It holds no policy of its own -- a caller with a set of
// independent jobs (for example one per model being evaluated) forks them across the group and
// joins before using the results. The Runtime Data Graph decides when work is eligible; the
// group decides only where it runs.
//
// The jobs of one fork share nothing but their index: the caller passes a body that touches only
// what its index names. ParallelFor runs every index exactly once -- on the workers and on the
// calling thread -- and returns only when they have all finished.
//
// Only the calling thread submits work, so the group needs no work-stealing queue: a batch is an
// index counter the workers claim from, one at a time. The calling thread claims indices too, so
// it never idles while it waits, and a batch of one runs inline without waking a worker at all.
class ComputeThreadsGroup final
{
public:
    // Starts `workerCount` threads named "ComputeThreadsGroup-N". The constructor blocks until
    // every worker has started, so a constructed group is ready to accept work. Zero workers is
    // valid: every ParallelFor then runs serially on the calling thread.
    explicit ComputeThreadsGroup(std::size_t workerCount);
    ~ComputeThreadsGroup();

    ComputeThreadsGroup(const ComputeThreadsGroup&) = delete;
    ComputeThreadsGroup& operator=(const ComputeThreadsGroup&) = delete;

    // Stops accepting work and joins the workers. A no-op when already stopped. Must not be
    // called while a ParallelFor is in flight.
    void Stop();

    // Runs `body(0)`, `body(1)`, ... `body(count - 1)` across the group and the calling thread,
    // and returns when every call has finished. `count == 0` does nothing; `count == 1` runs
    // inline. If a call throws, the first exception is rethrown here after every call has
    // finished. Only one ParallelFor may be in flight at a time, and `body` must not call
    // ParallelFor.
    void ParallelFor(std::size_t count, const std::function<void(std::size_t)>& body);

    // Number of worker threads in the group (the calling thread is additional).
    [[nodiscard]] std::size_t WorkerCount() const { return threads_.size(); }

private:
    class Worker;
    struct BatchState;

    // Claims and runs indices of `batch` until none are left.
    void RunBatch(BatchState& batch);

    std::mutex mutex_;
    std::condition_variable wake_;       // Workers idle here; the group wakes them per batch.
    std::condition_variable joined_;     // The submitter waits here for the workers to leave.
    BatchState* batch_ = nullptr;        // The batch workers may join, or null between batches.
    std::uint64_t generation_ = 0;       // Bumped per batch, so a worker joins each batch once.
    std::size_t activeWorkers_ = 0;      // Workers currently running the active batch.
    bool stopping_ = false;

    std::vector<std::unique_ptr<Worker>> workers_;
    std::vector<std::unique_ptr<Thread>> threads_;
};
} // namespace MmdLab
