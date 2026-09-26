#pragma once

#include <wrl/client.h>

#include <d3d12.h>

namespace MmdLab
{
// Owns one D3D12 command queue. The minimal renderer uses a single DIRECT queue, which can
// record graphics, compute, and copy work. Owned by the RhiThread role.
class Dx12CommandQueue final
{
public:
    explicit Dx12CommandQueue(ID3D12Device* device);

    Dx12CommandQueue(const Dx12CommandQueue&) = delete;
    Dx12CommandQueue& operator=(const Dx12CommandQueue&) = delete;

    [[nodiscard]] ID3D12CommandQueue* Get() const { return queue_.Get(); }

private:
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue_;
};
} // namespace MmdLab
