#include "Runtime/Core/Channel.h"
#include "Runtime/Core/FrameResource.h"
#include "Runtime/Core/FrameResourcePool.h"
#include "Runtime/Core/Runnable.h"
#include "Runtime/Core/TestFramework.h"
#include "Runtime/Core/Thread.h"

#include <atomic>
#include <cstdint>

namespace
{
using FrameChannel = MmdLab::Channel<MmdLab::FrameIndex, MmdLab::FrameResourcePool::kFrameCount>;

// Passes a FrameIndex from one channel to the next; stands in for a pipeline stage.
class PassthroughStage final : public MmdLab::Runnable
{
public:
    PassthroughStage(FrameChannel& input, FrameChannel& output)
        : input_(&input)
        , output_(&output)
    {
    }

    uint32_t Run() override
    {
        while (const auto index = input_->Pop())
        {
            ++processedCount;
            output_->Push(*index);
        }
        return 0;
    }

    void Stop() override { input_->Close(); }

    std::atomic<std::uint64_t> processedCount{ 0 };

private:
    FrameChannel* input_;
    FrameChannel* output_;
};

// Consumes a FrameIndex and returns it to the pool; stands in for the RhiThread role.
class ReleaseStage final : public MmdLab::Runnable
{
public:
    ReleaseStage(FrameChannel& input, MmdLab::FrameResourcePool& pool)
        : input_(&input)
        , pool_(&pool)
    {
    }

    uint32_t Run() override
    {
        while (const auto index = input_->Pop())
        {
            observedFrameIdSum.fetch_add(pool_->Get(*index).frameId);
            ++completedCount;
            pool_->Release(*index);
        }
        return 0;
    }

    void Stop() override { input_->Close(); }

    std::atomic<std::uint64_t> completedCount{ 0 };
    std::atomic<std::uint64_t> observedFrameIdSum{ 0 };

private:
    FrameChannel* input_;
    MmdLab::FrameResourcePool* pool_;
};
} // namespace

MMDLAB_TEST(Core.FrameLoop, RunsFramesThroughAllThreeStages)
{
    MmdLab::FrameResourcePool pool;
    FrameChannel gameToRender;
    FrameChannel renderToRhi;

    PassthroughStage renderStage(gameToRender, renderToRhi);
    ReleaseStage rhiStage(renderToRhi, pool);

    MmdLab::Thread renderThread(renderStage, L"RenderThread");
    MmdLab::Thread rhiThread(rhiStage, L"RhiThread");

    constexpr MmdLab::FrameId kProducedFrames = 25;
    for (MmdLab::FrameId frameId = 0; frameId < kProducedFrames; ++frameId)
    {
        const MmdLab::FrameIndex index = pool.Acquire();
        pool.Get(index).frameId = frameId;
        gameToRender.Push(index);
    }

    renderThread.RequestStop();
    rhiThread.RequestStop();

    MMDLAB_CHECK_EQUAL(kProducedFrames, renderStage.processedCount.load());
    MMDLAB_CHECK_EQUAL(kProducedFrames, rhiStage.completedCount.load());
    MMDLAB_CHECK_EQUAL(MmdLab::FrameResourcePool::kFrameCount, pool.FreeCount());

    const MmdLab::FrameId expectedSum = kProducedFrames * (kProducedFrames - 1) / 2;
    MMDLAB_CHECK_EQUAL(expectedSum, rhiStage.observedFrameIdSum.load());
}
