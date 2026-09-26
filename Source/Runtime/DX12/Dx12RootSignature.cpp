#include "Runtime/DX12/Dx12RootSignature.h"

#include <cstdio>
#include <stdexcept>
#include <string>

namespace MmdLab
{
Dx12RootSignature::Dx12RootSignature(ID3D12Device* device)
{
    D3D12_ROOT_PARAMETER rootParameters[3]{};

    // b0 (vertex + pixel): the camera constant buffer (view-projection, light, view dir).
    rootParameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    rootParameters[0].Descriptor.ShaderRegister = 0;
    rootParameters[0].Descriptor.RegisterSpace = 0;
    rootParameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    // b1 (pixel): the per-material shading parameters, set per draw.
    rootParameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    rootParameters[1].Constants.ShaderRegister = 1;
    rootParameters[1].Constants.RegisterSpace = 0;
    rootParameters[1].Constants.Num32BitValues = 16;
    rootParameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    // t0..t2 (pixel): the material's base, toon, and sphere textures, bound per draw as a
    // contiguous three-descriptor bundle.
    D3D12_DESCRIPTOR_RANGE textureRange{};
    textureRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    textureRange.NumDescriptors = 3;
    textureRange.BaseShaderRegister = 0;
    textureRange.RegisterSpace = 0;
    textureRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    rootParameters[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rootParameters[2].DescriptorTable.NumDescriptorRanges = 1;
    rootParameters[2].DescriptorTable.pDescriptorRanges = &textureRange;
    rootParameters[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    // s0 (pixel): the texture sampler.
    D3D12_STATIC_SAMPLER_DESC sampler{};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    sampler.MipLODBias = 0.0f;
    sampler.MaxAnisotropy = 1;
    sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
    sampler.BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_BLACK;
    sampler.MinLOD = 0.0f;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    sampler.ShaderRegister = 0;
    sampler.RegisterSpace = 0;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC description{};
    description.NumParameters = 3;
    description.pParameters = rootParameters;
    description.NumStaticSamplers = 1;
    description.pStaticSamplers = &sampler;
    description.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    Microsoft::WRL::ComPtr<ID3DBlob> serialized;
    Microsoft::WRL::ComPtr<ID3DBlob> error;
    if (FAILED(D3D12SerializeRootSignature(
        &description,
        D3D_ROOT_SIGNATURE_VERSION_1,
        &serialized,
        &error)))
    {
        const char* message = error != nullptr
            ? static_cast<const char*>(error->GetBufferPointer())
            : "unknown root signature error";
        throw std::runtime_error(std::string("Failed to serialize the root signature: ") + message);
    }

    const HRESULT createResult = device->CreateRootSignature(
        0,
        serialized->GetBufferPointer(),
        serialized->GetBufferSize(),
        IID_PPV_ARGS(&rootSignature_));
    if (FAILED(createResult))
    {
        char message[128];
        std::snprintf(message, sizeof(message), "Failed to create the root signature (HRESULT 0x%08X).", static_cast<unsigned int>(createResult));
        throw std::runtime_error(message);
    }
}
} // namespace MmdLab
