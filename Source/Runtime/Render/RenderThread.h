#pragma once

#include "Runtime/Core/Channel.h"
#include "Runtime/Core/FrameResource.h"
#include "Runtime/Core/FrameResourcePool.h"
#include "Runtime/Core/Runnable.h"

namespace MmdLab
{
// The RenderThread role: consumes a FrameIndex from the GameThread, compiles the sealed
// RenderFrame into a RenderWorkBatch, and forwards the index to the RhiThread. For the
// hard-coded triangle the compilation is a no-op; real work compilation lands here later.
class RenderThread final : public Runnable
{
public:
    RenderThread(
        Channel<FrameIndex, FrameResourcePool::kFrameCount>& input,
        Channel<FrameIndex, FrameResourcePool::kFrameCount>& output,
        FrameResourcePool& pool);

    uint32_t Run() override;

    // Closes the input channel so Run() drains and returns. RequestStop() calls this.
    void Stop() override;

private:
    Channel<FrameIndex, FrameResourcePool::kFrameCount>* input_;
    Channel<FrameIndex, FrameResourcePool::kFrameCount>* output_;
    FrameResourcePool* pool_;
};
} // namespace MmdLab
