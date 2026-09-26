#include "Runtime/DX12/Dx12GraphicsCommandList.h"

#include <stdexcept>

namespace MmdLab
{
Dx12GraphicsCommandList::Dx12GraphicsCommandList(ID3D12Device* device, ID3D12CommandAllocator* allocator)
{
    if (FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator, nullptr, IID_PPV_ARGS(&list_))))
    {
        throw std::runtime_error("Failed to create the D3D12 graphics command list.");
    }
}
} // namespace MmdLab
