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

        // Compile: for a static level the draw list is the set of instances in the selected
        // level; the RhiThread's renderer resolves each instance to its model and draws it.
        // Forward the level and UI projection the RhiThread needs to build GPU resources and
        // draw the imgui overlay.
        const RenderFrame& renderFrame = frame.gameToRender;
        frame.renderToRhi.instances = renderFrame.instances;
        frame.renderToRhi.models = renderFrame.models;
        frame.renderToRhi.levels = renderFrame.levels;
        frame.renderToRhi.selectedLevel = renderFrame.selectedLevel;
        frame.renderToRhi.levelGeneration = renderFrame.levelGeneration;
        frame.renderToRhi.camera = renderFrame.camera;
        frame.renderToRhi.bonePalette = renderFrame.bonePalette;
        frame.renderToRhi.bonePaletteOffsets = renderFrame.bonePaletteOffsets;

        output_->Push(*index);
    }
    return 0;
}

void RenderThread::Stop()
{
    input_->Close();
}
} // namespace MmdLab
