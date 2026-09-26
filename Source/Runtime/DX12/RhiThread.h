#pragma once

#include "Runtime/Asset/ImageLoader.h"
#include "Runtime/Asset/MeshAsset.h"
#include "Runtime/Core/Channel.h"
#include "Runtime/Core/FrameResource.h"
#include "Runtime/Core/FrameResourcePool.h"
#include "Runtime/Core/Runnable.h"

#include <windows.h>

#include <cstdint>
#include <deque>
#include <memory>
#include <span>

namespace MmdLab
{
class Dx12Renderer;
class RenderDocCapture;

// The RhiThread role: owns all native D3D12 state (device, swap chain, mesh buffers,
// pipeline). It consumes a FrameIndex from the RenderThread, submits the frame's draw list,
// presents, and returns the frame to the pool. The renderer is created in Init(), on this
// thread.
class RhiThread final : public Runnable
{
public:
    RhiThread(
        Channel<FrameIndex, FrameResourcePool::kFrameCount>& input,
        FrameResourcePool& pool,
        HWND window,
        std::uint32_t width,
        std::uint32_t height,
        const MeshAsset& mesh,
        std::span<const Image> textures);

    ~RhiThread() override;

    bool Init() override;
    uint32_t Run() override;

    // Closes the input channel so Run() drains and returns. RequestStop() calls this.
    void Stop() override;

private:
    Channel<FrameIndex, FrameResourcePool::kFrameCount>* input_;
    FrameResourcePool* pool_;
    HWND window_;
    std::uint32_t width_;
    std::uint32_t height_;
    const MeshAsset* mesh_;
    std::span<const Image> textures_; // CPU textures, uploaded by the renderer in Init().
    std::unique_ptr<Dx12Renderer> renderer_;
    std::unique_ptr<RenderDocCapture> capture_;
    bool captureRequested_ = false;

    // Frame indices submitted to the GPU, oldest first, awaiting fence completion before
    // their frame resources are returned to the pool.
    std::deque<FrameIndex> pendingRetirement_;
};
} // namespace MmdLab
