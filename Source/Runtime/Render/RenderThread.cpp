#include "Runtime/Render/RenderThread.h"

namespace MmdLab
{
RenderThread::RenderThread(
    Channel<FrameIndex, FrameResourcePool::kFrameCount>& input,
    Channel<FrameIndex, FrameResourcePool::kFrameCount>& output,
    FrameResourcePool& pool)
    : input_(&input)
    , output_(&output)
    , pool_(&pool)
{
}

uint32_t RenderThread::Run()
{
    while (const auto index = input_->Pop())
    {
        FrameResource& frame = pool_->Get(*index);

        // Compile: read frame.gameToRender (RenderFrame), write frame.renderToRhi
        // (RenderWorkBatch). A no-op for the hard-coded triangle.
        (void)frame;

        output_->Push(*index);
    }
    return 0;
}

void RenderThread::Stop()
{
    input_->Close();
}
} // namespace MmdLab
