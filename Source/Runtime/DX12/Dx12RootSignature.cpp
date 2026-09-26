#include "Runtime/DX12/Dx12RootSignature.h"

#include <stdexcept>

namespace MmdLab
{
Dx12RootSignature::Dx12RootSignature(ID3D12Device* device)
{
    D3D12_ROOT_SIGNATURE_DESC description{};
    description.NumParameters = 0;
    description.pParameters = nullptr;
    description.NumStaticSamplers = 0;
    description.pStaticSamplers = nullptr;
    description.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

    Microsoft::WRL::ComPtr<ID3DBlob> serialized;
    Microsoft::WRL::ComPtr<ID3DBlob> error;
    if (FAILED(D3D12SerializeRootSignature(
        &description,
        D3D_ROOT_SIGNATURE_VERSION_1,
        &serialized,
        &error)))
    {
        throw std::runtime_error("Failed to serialize the root signature.");
    }

    if (FAILED(device->CreateRootSignature(
        0,
        serialized->GetBufferPointer(),
        serialized->GetBufferSize(),
        IID_PPV_ARGS(&rootSignature_))))
    {
        throw std::runtime_error("Failed to create the root signature.");
    }
}
} // namespace MmdLab
