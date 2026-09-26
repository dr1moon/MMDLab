#pragma once

#include <wrl/client.h>

#include <d3d12.h>

namespace MmdLab
{
// Owns the root signature that describes the shader interface. The hard-coded triangle
// uses an empty signature (its vertex shader reads only SV_VertexID and no constants).
class Dx12RootSignature final
{
public:
    explicit Dx12RootSignature(ID3D12Device* device);

    Dx12RootSignature(const Dx12RootSignature&) = delete;
    Dx12RootSignature& operator=(const Dx12RootSignature&) = delete;

    [[nodiscard]] ID3D12RootSignature* Get() const { return rootSignature_.Get(); }

private:
    Microsoft::WRL::ComPtr<ID3D12RootSignature> rootSignature_;
};
} // namespace MmdLab
