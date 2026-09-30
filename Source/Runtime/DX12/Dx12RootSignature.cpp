#include "Runtime/DX12/Dx12RootSignature.h"

#include <cstdio>
#include <stdexcept>
#include <string>

namespace MmdLab
{
Dx12RootSignature::Dx12RootSignature(ID3D12Device* device)
{
    D3D12_ROOT_PARAMETER rootParameters[9]{};

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

    // b2 (vertex): the per-instance world matrix, set per instance.
    rootParameters[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    rootParameters[3].Constants.ShaderRegister = 2;
    rootParameters[3].Constants.RegisterSpace = 0;
    rootParameters[3].Constants.Num32BitValues = 16;
    rootParameters[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;

    // t3 (vertex): the skinning bone matrices, bound per model as a single-SRV descriptor table.
    D3D12_DESCRIPTOR_RANGE boneRange{};
    boneRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    boneRange.NumDescriptors = 1;
    boneRange.BaseShaderRegister = 3;
    boneRange.RegisterSpace = 0;
    boneRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    rootParameters[4].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rootParameters[4].DescriptorTable.NumDescriptorRanges = 1;
    rootParameters[4].DescriptorTable.pDescriptorRanges = &boneRange;
    rootParameters[4].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;

    // t4 (vertex): the skin-reference-bone table (local u8 -> global u16), bound per model.
    D3D12_DESCRIPTOR_RANGE refBoneRange{};
    refBoneRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    refBoneRange.NumDescriptors = 1;
    refBoneRange.BaseShaderRegister = 4;
    refBoneRange.RegisterSpace = 0;
    refBoneRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    rootParameters[5].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rootParameters[5].DescriptorTable.NumDescriptorRanges = 1;
    rootParameters[5].DescriptorTable.pDescriptorRanges = &refBoneRange;
    rootParameters[5].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;

    // b3 (vertex): the per-submesh skin-reference-bone slice offset, set per draw.
    rootParameters[6].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    rootParameters[6].Constants.ShaderRegister = 3;
    rootParameters[6].Constants.RegisterSpace = 0;
    rootParameters[6].Constants.Num32BitValues = 1;
    rootParameters[6].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;

    // t5 (vertex): the per-vertex morph delta buffer (float3 per vertex), bound per model.
    D3D12_DESCRIPTOR_RANGE morphRange{};
    morphRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    morphRange.NumDescriptors = 1;
    morphRange.BaseShaderRegister = 5;
    morphRange.RegisterSpace = 0;
    morphRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    rootParameters[7].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rootParameters[7].DescriptorTable.NumDescriptorRanges = 1;
    rootParameters[7].DescriptorTable.pDescriptorRanges = &morphRange;
    rootParameters[7].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;

    // t6 (pixel): the reflection texture, bound as a single-SRV descriptor table. A texture SRV
    // cannot be a root descriptor, so it lives in the model's shader-visible SRV heap.
    D3D12_DESCRIPTOR_RANGE reflectionRange{};
    reflectionRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    reflectionRange.NumDescriptors = 1;
    reflectionRange.BaseShaderRegister = 6;
    reflectionRange.RegisterSpace = 0;
    reflectionRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    rootParameters[8].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rootParameters[8].DescriptorTable.NumDescriptorRanges = 1;
    rootParameters[8].DescriptorTable.pDescriptorRanges = &reflectionRange;
    rootParameters[8].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

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
    description.NumParameters = 9;
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
