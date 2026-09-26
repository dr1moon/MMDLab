#include "Runtime/Render/RenderThread.h"

namespace MmdLab
{
RenderThread::RenderThread(
    Channel<FrameIndex, FrameResourcePool::kFrameCount>& input,
    Channel<FrameIndex, FrameResourcePool::kFrameCount>& output,
    FrameResourcePool& pool,
    const MeshAsset& mesh)
    : input_(&input)
    , output_(&output)
    , pool_(&pool)
    , mesh_(&mesh)
{
}

uint32_t RenderThread::Run()
{
    while (const auto index = input_->Pop())
    {
        FrameResource& frame = pool_->Get(*index);

        // Compile: for a static mesh, the draw list is the mesh's immutable draw packets.
        frame.renderToRhi.drawPackets = std::span<const DrawPacket>(mesh_->drawPackets);

        output_->Push(*index);
    }
    return 0;
}

void RenderThread::Stop()
{
    input_->Close();
}
} // namespace MmdLab
