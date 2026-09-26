#include "Runtime/DX12/Dx12SwapChain.h"

#include <d3d12.h>

#include <stdexcept>

namespace MmdLab
{
Dx12SwapChain::Dx12SwapChain(
    IDXGIFactory4* factory,
    ID3D12CommandQueue* queue,
    const HWND window,
    const std::uint32_t width,
    const std::uint32_t height)
    : backBufferCount_(2)
{
    DXGI_SWAP_CHAIN_DESC1 description{};
    description.Width = width;
    description.Height = height;
    description.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    description.SampleDesc.Count = 1;
    description.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    description.BufferCount = backBufferCount_;
    description.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;

    Microsoft::WRL::ComPtr<IDXGISwapChain1> swapChain;
    if (FAILED(factory->CreateSwapChainForHwnd(queue, window, &description, nullptr, nullptr, &swapChain)))
    {
        throw std::runtime_error("Failed to create the DXGI swap chain.");
    }

    if (FAILED(swapChain.As(&swapChain_)))
    {
        throw std::runtime_error("Failed to query IDXGISwapChain3 from the swap chain.");
    }
}

std::uint32_t Dx12SwapChain::CurrentBackBufferIndex() const
{
    return swapChain_->GetCurrentBackBufferIndex();
}
} // namespace MmdLab
