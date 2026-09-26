#include "Runtime/DX12/Dx12DescriptorHeap.h"

#include <stdexcept>

namespace MmdLab
{
Dx12DescriptorHeap::Dx12DescriptorHeap(ID3D12Device* device, const D3D12_DESCRIPTOR_HEAP_TYPE type, const UINT count)
{
    D3D12_DESCRIPTOR_HEAP_DESC description{};
    description.Type = type;
    description.NumDescriptors = count;
    description.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    description.NodeMask = 0;

    if (FAILED(device->CreateDescriptorHeap(&description, IID_PPV_ARGS(&heap_))))
    {
        throw std::runtime_error("Failed to create the D3D12 descriptor heap.");
    }

    descriptorSize_ = device->GetDescriptorHandleIncrementSize(type);
}

D3D12_CPU_DESCRIPTOR_HANDLE Dx12DescriptorHeap::CpuHandle(const UINT index) const
{
    D3D12_CPU_DESCRIPTOR_HANDLE handle = heap_->GetCPUDescriptorHandleForHeapStart();
    handle.ptr += static_cast<SIZE_T>(index) * descriptorSize_;
    return handle;
}
} // namespace MmdLab
