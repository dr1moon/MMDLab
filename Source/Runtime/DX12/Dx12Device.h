#pragma once

#include "Runtime/DX12/GpuInfo.h"

#include <wrl/client.h>

#include <d3d12.h>
#include <dxgi1_4.h>

namespace MmdLab
{
// Owns one Direct3D 12 device and the DXGI adapter it was created on, and records the
// adapter's information in a GpuInfo record. Construction enumerates adapters, picks the
// best hardware adapter (falling back to software), creates the device, and queries the
// highest supported feature level. Throws std::runtime_error on failure.
//
// This is the first, minimal slice of Runtime/DX12: device only, no queues, fences,
// descriptor heaps, or command lists yet.
class Dx12Device final
{
public:
    Dx12Device();

    Dx12Device(const Dx12Device&) = delete;
    Dx12Device& operator=(const Dx12Device&) = delete;

    [[nodiscard]] const GpuInfo& Info() const { return info_; }
    [[nodiscard]] ID3D12Device* Get() const { return device_.Get(); }
    [[nodiscard]] IDXGIFactory4* GetFactory() const { return factory_.Get(); }

private:
    Microsoft::WRL::ComPtr<ID3D12Device> device_;
    Microsoft::WRL::ComPtr<IDXGIFactory4> factory_;
    Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter_;
    GpuInfo info_;
};
} // namespace MmdLab
