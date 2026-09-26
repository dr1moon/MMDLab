#include "Runtime/Render/RenderThread.h"

#include "Runtime/Asset/MeshAsset.h"

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

        // Compile: for a static mesh, the draw list is the selected model's immutable draw
        // packets. Forward the model and UI projection the RhiThread needs to (re)build GPU
        // resources and draw the imgui overlay.
        const RenderFrame& renderFrame = frame.gameToRender;
        if (renderFrame.mesh != nullptr)
        {
            frame.renderToRhi.drawPackets = std::span<const DrawPacket>(renderFrame.mesh->drawPackets);
        }
        else
        {
            frame.renderToRhi.drawPackets = {};
        }
        frame.renderToRhi.mesh = renderFrame.mesh;
        frame.renderToRhi.textures = renderFrame.textures;
        frame.renderToRhi.modelGeneration = renderFrame.modelGeneration;
        frame.renderToRhi.modelNames = renderFrame.modelNames;
        frame.renderToRhi.selectedModel = renderFrame.selectedModel;

        output_->Push(*index);
    }
    return 0;
}

void RenderThread::Stop()
{
    input_->Close();
}
} // namespace MmdLab
