#pragma once

#include <wrl/client.h>

#include <d3d12.h>
#include <dxgi1_4.h>

#include <cstdint>

namespace MmdLab
{
// Owns the DXGI swap chain that presents rendered frames to a window. Two back buffers are
// used, each with its own render-target view managed by the renderer.
class Dx12SwapChain final
{
public:
    Dx12SwapChain(
        IDXGIFactory4* factory,
        ID3D12CommandQueue* queue,
        HWND window,
        std::uint32_t width,
        std::uint32_t height);

    Dx12SwapChain(const Dx12SwapChain&) = delete;
    Dx12SwapChain& operator=(const Dx12SwapChain&) = delete;

    [[nodiscard]] IDXGISwapChain3* Get() const { return swapChain_.Get(); }
    [[nodiscard]] std::uint32_t BackBufferCount() const { return backBufferCount_; }

    // The index of the back buffer that the next frame should render into.
    [[nodiscard]] std::uint32_t CurrentBackBufferIndex() const;

private:
    Microsoft::WRL::ComPtr<IDXGISwapChain3> swapChain_;
    std::uint32_t backBufferCount_ = 0;
};
} // namespace MmdLab
