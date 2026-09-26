#pragma once

#include <wrl/client.h>

#include <cstdint>
#include <d3d12.h>

namespace MmdLab
{
// Owns one D3D12 fence, used for CPU/GPU synchronization and frame-resource retirement.
class Dx12Fence final
{
public:
    explicit Dx12Fence(ID3D12Device* device);

    Dx12Fence(const Dx12Fence&) = delete;
    Dx12Fence& operator=(const Dx12Fence&) = delete;

    [[nodiscard]] ID3D12Fence* Get() const { return fence_.Get(); }

    // The highest fence value the GPU has reached so far.
    [[nodiscard]] uint64_t CompletedValue() const;

private:
    Microsoft::WRL::ComPtr<ID3D12Fence> fence_;
};
} // namespace MmdLab
