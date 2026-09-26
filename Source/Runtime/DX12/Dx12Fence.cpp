#include "Runtime/DX12/Dx12Fence.h"

#include <stdexcept>

namespace MmdLab
{
Dx12Fence::Dx12Fence(ID3D12Device* device)
{
    if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_))))
    {
        throw std::runtime_error("Failed to create the D3D12 fence.");
    }
}

uint64_t Dx12Fence::CompletedValue() const
{
    return fence_->GetCompletedValue();
}
} // namespace MmdLab
