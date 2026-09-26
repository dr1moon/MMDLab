#pragma once

#include <wrl/client.h>

#include <d3d12.h>

namespace MmdLab
{
// Owns one D3D12 graphics command list, used to record GPU commands before submission.
// Created in the recording state; a frame loop follows Reset -> record -> Close. Owned by
// the RhiThread role.
class Dx12GraphicsCommandList final
{
public:
    Dx12GraphicsCommandList(ID3D12Device* device, ID3D12CommandAllocator* allocator);

    Dx12GraphicsCommandList(const Dx12GraphicsCommandList&) = delete;
    Dx12GraphicsCommandList& operator=(const Dx12GraphicsCommandList&) = delete;

    [[nodiscard]] ID3D12GraphicsCommandList* Get() const { return list_.Get(); }

private:
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> list_;
};
} // namespace MmdLab
