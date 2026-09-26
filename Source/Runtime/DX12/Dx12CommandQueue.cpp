#include "Runtime/DX12/Dx12CommandQueue.h"

#include <stdexcept>

namespace MmdLab
{
Dx12CommandQueue::Dx12CommandQueue(ID3D12Device* device)
{
    D3D12_COMMAND_QUEUE_DESC description{};
    description.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    description.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
    description.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
    description.NodeMask = 0;

    if (FAILED(device->CreateCommandQueue(&description, IID_PPV_ARGS(&queue_))))
    {
        throw std::runtime_error("Failed to create the D3D12 command queue.");
    }
}
} // namespace MmdLab
