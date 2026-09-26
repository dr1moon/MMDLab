#pragma once

#include <wrl/client.h>

#include <d3d12.h>

namespace MmdLab
{
// Owns one D3D12 command allocator, the backing memory for command-list recording. Reset
// and reused across frames. Owned by the RhiThread role.
class Dx12CommandAllocator final
{
public:
    explicit Dx12CommandAllocator(ID3D12Device* device);

    Dx12CommandAllocator(const Dx12CommandAllocator&) = delete;
    Dx12CommandAllocator& operator=(const Dx12CommandAllocator&) = delete;

    [[nodiscard]] ID3D12CommandAllocator* Get() const { return allocator_.Get(); }

private:
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocator_;
};
} // namespace MmdLab
