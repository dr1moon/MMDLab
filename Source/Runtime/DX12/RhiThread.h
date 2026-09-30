#pragma once

#include "Runtime/Core/Channel.h"
#include "Runtime/Core/FrameResource.h"
#include "Runtime/Core/FrameResourcePool.h"
#include "Runtime/Core/Runnable.h"
#include "Runtime/Core/Ui.h"
#include "Runtime/Scene/PhysicsDebugDraw.h"

#include <windows.h>

#include <cstdint>
#include <deque>
#include <memory>
#include <vector>

struct ImGuiContext;

namespace MmdLab
{
class Dx12Renderer;
class RenderDocCapture;

// The RhiThread role: owns all native D3D12 state (device, swap chain, mesh buffers,
// pipeline) and the imgui context plus its Win32/DX12 backends. It consumes a FrameIndex from
// the RenderThread, drains forwarded Win32 input into imgui, submits the frame's draw list and
// the imgui overlay, presents, and returns the frame to the pool. It builds GPU model
// resources when the selected level's generation changes.
class RhiThread final : public Runnable
{
public:
    RhiThread(
        Channel<FrameIndex, FrameResourcePool::kFrameCount>& input,
        FrameResourcePool& pool,
        HWND window,
        std::uint32_t width,
        std::uint32_t height,
        Channel<Win32InputMessage, kWin32InputQueueCapacity>& inputQueue,
        Channel<UiRequest, kUiRequestQueueCapacity>& uiQueue,
        Channel<CameraInput, kCameraInputQueueCapacity>& cameraQueue);

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
    Channel<Win32InputMessage, kWin32InputQueueCapacity>* inputQueue_;
    Channel<UiRequest, kUiRequestQueueCapacity>* uiQueue_;
    Channel<CameraInput, kCameraInputQueueCapacity>* cameraQueue_;
    std::unique_ptr<Dx12Renderer> renderer_;
    std::unique_ptr<RenderDocCapture> capture_;
    bool captureRequested_ = false;
    bool minimized_ = false;
    ImGuiContext* imguiContext_ = nullptr;
    // Scratch for the physics debug overlay, reused across frames.
    std::vector<DebugLine2D> debugLines_;
    // Level generation last applied to the renderer; the sentinel forces the first build.
    std::uint32_t lastLevelGeneration_ = 0xFFFFFFFFu;

    // Frame indices submitted to the GPU, oldest first, awaiting fence completion before
    // their frame resources are returned to the pool.
    std::deque<FrameIndex> pendingRetirement_;
};
} // namespace MmdLab
