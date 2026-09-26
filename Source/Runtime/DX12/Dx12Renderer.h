#pragma once

#include "Runtime/DX12/Dx12CommandQueue.h"
#include "Runtime/DX12/Dx12Device.h"
#include "Runtime/DX12/Dx12RootSignature.h"
#include "Runtime/DX12/Dx12SwapChain.h"

#include <wrl/client.h>

#include <d3d12.h>

#include <cstdint>

namespace MmdLab
{
// The minimal synchronous renderer: a two-back-buffer swap chain, an empty root signature,
// and a hard-coded triangle pipeline. Render() clears, draws the triangle, and presents one
// frame. This milestone runs entirely on the calling thread; the RhiThread split comes later.
class Dx12Renderer final
{
public:
    Dx12Renderer(HWND window, std::uint32_t width, std::uint32_t height);

    Dx12Renderer(const Dx12Renderer&) = delete;
    Dx12Renderer& operator=(const Dx12Renderer&) = delete;

    // Renders and presents one frame.
    void Render();

private:
    static constexpr std::uint32_t kFrameCount = 2;

    void WaitForPreviousFrame(std::uint32_t frameIndex);
    void CreatePipelineState();
    void CreateRenderTargetViews();

    Dx12Device device_;
    Dx12CommandQueue queue_;
    Dx12SwapChain swapChain_;
    Dx12RootSignature rootSignature_;

    Microsoft::WRL::ComPtr<ID3D12PipelineState> pipelineState_;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> rtvHeap_;
    Microsoft::WRL::ComPtr<ID3D12Resource> backBuffers_[kFrameCount];
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> commandAllocators_[kFrameCount];
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> commandList_;
    Microsoft::WRL::ComPtr<ID3D12Fence> fences_[kFrameCount];

    std::uint32_t width_ = 0;
    std::uint32_t height_ = 0;
    std::uint32_t rtvDescriptorSize_ = 0;
    std::uint64_t fenceValues_[kFrameCount] = {};
    std::uint64_t currentFence_ = 0;
};
} // namespace MmdLab
