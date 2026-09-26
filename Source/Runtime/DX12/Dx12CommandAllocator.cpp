#include "Runtime/DX12/Dx12CommandAllocator.h"

#include <stdexcept>

namespace MmdLab
{
Dx12CommandAllocator::Dx12CommandAllocator(ID3D12Device* device)
{
    if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator_))))
    {
        throw std::runtime_error("Failed to create the D3D12 command allocator.");
    }
}
} // namespace MmdLab
