#include "Runtime/DX12/RhiThread.h"

#include "Runtime/DX12/Dx12Renderer.h"
#include "Runtime/DX12/RenderDocCapture.h"

#include <exception>
#include <iostream>

namespace MmdLab
{
RhiThread::RhiThread(
    Channel<FrameIndex, FrameResourcePool::kFrameCount>& input,
    FrameResourcePool& pool,
    const HWND window,
    const std::uint32_t width,
    const std::uint32_t height,
    const MeshAsset& mesh,
    const std::span<const Image> textures)
    : input_(&input)
    , pool_(&pool)
    , window_(window)
    , width_(width)
    , height_(height)
    , mesh_(&mesh)
    , textures_(textures)
{
}

RhiThread::~RhiThread() = default;

bool RhiThread::Init()
{
    try
    {
        // Load RenderDoc before creating the device so it hooks device creation.
        capture_ = std::make_unique<RenderDocCapture>();
        captureRequested_ = GetEnvironmentVariableW(L"MMDLAB_CAPTURE", nullptr, 0) > 0;
        if (captureRequested_ && capture_->IsAvailable())
        {
            capture_->SetCapturePath("captures/mesh");
        }

        renderer_ = std::make_unique<Dx12Renderer>(window_, width_, height_, *mesh_, textures_);
        return true;
    }
    catch (const std::exception& exception)
    {
        std::cerr << "RhiThread init failed: " << exception.what() << '\n';
        return false;
    }
}

uint32_t RhiThread::Run()
{
    while (const auto index = input_->Pop())
    {
        const bool captureThisFrame =
            captureRequested_ && capture_ != nullptr && capture_->IsAvailable();

        if (captureThisFrame)
        {
            capture_->StartCapture();
        }

        FrameResource& frame = pool_->Get(*index);
        std::uint64_t fenceValue = 0;
        try
        {
            fenceValue = renderer_->Render(frame.renderToRhi.drawPackets);
        }
        catch (const std::exception& exception)
        {
            std::cerr << "RhiThread render failed: " << exception.what() << '\n';
        }
        frame.gpuFenceValue = fenceValue;

        if (captureThisFrame)
        {
            const bool captured = capture_->EndCapture();
            std::cerr << "capture: " << (captured ? "ok " : "failed ")
                      << capture_->LastCapturePath() << '\n';
            captureRequested_ = false;
        }

        pendingRetirement_.push_back(*index);

        // Retire frames whose GPU work has completed, oldest first.
        while (!pendingRetirement_.empty()
            && renderer_->IsFrameComplete(pool_->Get(pendingRetirement_.front()).gpuFenceValue))
        {
            pool_->Release(pendingRetirement_.front());
            pendingRetirement_.pop_front();
        }
    }

    // Drain any frames still awaiting GPU retirement before the thread exits.
    for (const FrameIndex index : pendingRetirement_)
    {
        pool_->Release(index);
    }
    pendingRetirement_.clear();

    return 0;
}

void RhiThread::Stop()
{
    input_->Close();
}
} // namespace MmdLab
