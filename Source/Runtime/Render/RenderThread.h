#pragma once

#include "Runtime/Asset/MeshAsset.h"
#include "Runtime/Core/Channel.h"
#include "Runtime/Core/FrameResource.h"
#include "Runtime/Core/FrameResourcePool.h"
#include "Runtime/Core/Runnable.h"

namespace MmdLab
{
// The RenderThread role: consumes a FrameIndex from the GameThread, compiles the sealed
// RenderFrame into a RenderWorkBatch (the draw list), and forwards the index to the
// RhiThread. For a static mesh the draw list is the mesh asset's immutable draw packets;
// culling, sorting, and LOD land here later.
class RenderThread final : public Runnable
{
public:
    RenderThread(
        Channel<FrameIndex, FrameResourcePool::kFrameCount>& input,
        Channel<FrameIndex, FrameResourcePool::kFrameCount>& output,
        FrameResourcePool& pool,
        const MeshAsset& mesh);

    uint32_t Run() override;

    // Closes the input channel so Run() drains and returns. RequestStop() calls this.
    void Stop() override;

private:
    Channel<FrameIndex, FrameResourcePool::kFrameCount>* input_;
    Channel<FrameIndex, FrameResourcePool::kFrameCount>* output_;
    FrameResourcePool* pool_;
    const MeshAsset* mesh_;
};
} // namespace MmdLab
