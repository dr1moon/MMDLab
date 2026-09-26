#pragma once

#include <wrl/client.h>

#include <d3d12.h>

namespace MmdLab
{
// Owns one D3D12 descriptor heap. The minimal renderer starts with a render-target-view
// (RTV) heap for clearing and presenting. CPU handles are computed manually to avoid a
// dependency on the d3dx12 helper header.
class Dx12DescriptorHeap final
{
public:
    Dx12DescriptorHeap(ID3D12Device* device, D3D12_DESCRIPTOR_HEAP_TYPE type, UINT count);

    Dx12DescriptorHeap(const Dx12DescriptorHeap&) = delete;
    Dx12DescriptorHeap& operator=(const Dx12DescriptorHeap&) = delete;

    [[nodiscard]] ID3D12DescriptorHeap* Get() const { return heap_.Get(); }
    [[nodiscard]] D3D12_CPU_DESCRIPTOR_HANDLE CpuHandle(UINT index) const;

private:
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> heap_;
    UINT descriptorSize_ = 0;
};
} // namespace MmdLab
