#include "Runtime/DX12/Dx12Renderer.h"

#include "Runtime/Asset/Model.h"
#include "Runtime/DX12/ShaderCompiler.h"
#include "Runtime/DX12/Shaders.h"
#include "Runtime/Scene/Camera.h"
#include "Runtime/Scene/WorldData.h"
#include "Runtime/Core/Log.h"

#include "tracy/Tracy.hpp"

#include "imgui_impl_dx12.h"

#include <windows.h>

#include <DirectXMath.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <format>
#include <stdexcept>
#include <vector>

namespace
{
// Dumps the D3D12 debug layer's stored messages to stderr (debug builds only).
void DumpD3d12Messages(ID3D12Device* device)
{
    Microsoft::WRL::ComPtr<ID3D12InfoQueue> infoQueue;
    if (FAILED(device->QueryInterface(IID_PPV_ARGS(&infoQueue))))
    {
        MmdLab::LogWarning("Dx12", "ID3D12InfoQueue unavailable");
        return;
    }
    const std::uint64_t count = infoQueue->GetNumStoredMessages();
    MmdLab::LogInfo("Dx12", std::format("InfoQueue has {} messages", count));
    for (std::uint64_t i = 0; i < count; ++i)
    {
        std::size_t length = 0;
        infoQueue->GetMessage(i, nullptr, &length);
        std::vector<std::uint8_t> buffer(length);
        auto* message = reinterpret_cast<D3D12_MESSAGE*>(buffer.data());
        infoQueue->GetMessage(i, message, &length);
        MmdLab::LogWarning("Dx12", std::format("D3D12: {}", message->pDescription));
    }
}

} // namespace

namespace MmdLab
{
Dx12Renderer::Dx12Renderer(
    const HWND window,
    const std::uint32_t width,
    const std::uint32_t height)
    : device_()
    , queue_(device_.Get())
    , swapChain_(device_.GetFactory(), queue_.Get(), window, width, height)
    , rootSignature_(device_.Get())
    , width_(width)
    , height_(height)
{
    rtvDescriptorSize_ = device_.Get()->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    srvDescriptorSize_ = device_.Get()->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    CreatePipelineState();
    CreateRenderTargetViews();
    CreateDepthBuffer();
    CreateReflectionTarget();
    CreateImGuiSrvHeap();
    CreateConstantBuffer();

    for (std::uint32_t i = 0; i < kFrameCount; ++i)
    {
        if (FAILED(device_.Get()->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&commandAllocators_[i]))))
        {
            throw std::runtime_error("Failed to create a command allocator.");
        }
        if (FAILED(device_.Get()->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fences_[i]))))
        {
            throw std::runtime_error("Failed to create a fence.");
        }
    }

    if (FAILED(device_.Get()->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&frameFence_))))
    {
        throw std::runtime_error("Failed to create the frame fence.");
    }

    if (FAILED(device_.Get()->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, commandAllocators_[0].Get(), nullptr, IID_PPV_ARGS(&commandList_))))
    {
        throw std::runtime_error("Failed to create the command list.");
    }
    commandList_->Close();

    // Created last, after every other throwing step, so a failed constructor cannot leak it.
    fenceEvent_ = CreateEventW(nullptr, false, false, nullptr);
    if (fenceEvent_ == nullptr)
    {
        throw std::runtime_error("Failed to create the fence wait event.");
    }
}

Dx12Renderer::~Dx12Renderer()
{
    if (fenceEvent_ != nullptr)
    {
        CloseHandle(fenceEvent_);
        fenceEvent_ = nullptr;
    }
}

void Dx12Renderer::EnsureModelsResident(
    const std::span<const ModelInstance> instances,
    const std::span<const Model> models)
{
    // Wait for all in-flight GPU work so the upload path below can reuse command allocator 0;
    // this runs only on a level switch, so a full idle is acceptable.
    WaitForGpuIdle();

    for (const ModelInstance& instance : instances)
    {
        if (!instance.visible || instance.modelIndex >= models.size())
        {
            continue;
        }
        if (instance.modelIndex >= residentModels_.size() || residentModels_[instance.modelIndex] == nullptr)
        {
            BuildGpuModel(instance.modelIndex, models[instance.modelIndex]);
        }
    }
}

void Dx12Renderer::Resize(const std::uint32_t width, const std::uint32_t height)
{
    if (width == 0 || height == 0 || (width == width_ && height == height_))
    {
        return;
    }

    WaitForGpuIdle();

    for (auto& backBuffer : backBuffers_)
    {
        backBuffer.Reset();
    }
    depthBuffer_.Reset();
    reflectionTarget_.Reset();

    swapChain_.Resize(width, height);
    width_ = width;
    height_ = height;

    CreateRenderTargetViews();
    CreateDepthBuffer();
    CreateReflectionTarget();

    // The reflection target was rebuilt; recreate each resident model's reflection SRV over the
    // new resource (their heap slots are unchanged).
    D3D12_SHADER_RESOURCE_VIEW_DESC reflectionSrv{};
    reflectionSrv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    reflectionSrv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    reflectionSrv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    reflectionSrv.Texture2D.MipLevels = 1;
    for (const auto& gpuModel : residentModels_)
    {
        if (gpuModel != nullptr)
        {
            device_.Get()->CreateShaderResourceView(reflectionTarget_.Get(), &reflectionSrv, gpuModel->reflectionSrvCpu);
        }
    }

    LogInfo("Dx12", std::format("Resized render targets to {}x{}", width_, height_));
}

void Dx12Renderer::BuildGpuModel(const std::size_t modelIndex, const Model& model)
{
    GpuModel gpuModel;
    gpuModel.materials = model.mesh.materials;

    // Project each runtime material into the pixel shader's per-material constant layout.
    gpuModel.materialParams.reserve(gpuModel.materials.size());
    for (const MMDToonMaterial& material : gpuModel.materials)
    {
        MaterialShaderParams params{};
        for (int c = 0; c < 4; ++c) { params.baseColor[c] = material.baseColor[c]; }
        for (int c = 0; c < 3; ++c) { params.ambient[c] = material.ambientColor[c]; }
        for (int c = 0; c < 3; ++c) { params.specular[c] = material.specularColor[c]; }
        params.shininess = material.specularStrength;
        params.sphereMode = static_cast<float>(material.sphereMode);
        gpuModel.materialParams.push_back(params);
    }

    CreateMeshBuffers(gpuModel, model);
    CreateTextures(gpuModel, model.textures);

    // Bone matrices: double-buffered default-heap structured buffers, each with an SRV in the
    // trailing slots of the model's SRV heap, plus a persistent upload staging buffer the CPU
    // writes before the GPU copies it into the default buffer each frame.
    const UINT materialSrvCount = static_cast<UINT>(gpuModel.materials.size()) * 3;
    const std::uint32_t boneCount = std::max<std::uint32_t>(1u, static_cast<std::uint32_t>(model.skeleton.bones.size()));
    D3D12_CPU_DESCRIPTOR_HANDLE srvCpu = gpuModel.srvHeap->GetCPUDescriptorHandleForHeapStart();
    srvCpu.ptr += static_cast<SIZE_T>(materialSrvCount) * srvDescriptorSize_;
    D3D12_GPU_DESCRIPTOR_HANDLE srvGpu = gpuModel.srvHeap->GetGPUDescriptorHandleForHeapStart();
    srvGpu.ptr += static_cast<SIZE_T>(materialSrvCount) * srvDescriptorSize_;

    D3D12_RESOURCE_DESC description{};
    description.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    description.Width = static_cast<UINT64>(boneCount) * sizeof(DirectX::XMFLOAT4X4);
    description.Height = 1;
    description.DepthOrArraySize = 1;
    description.MipLevels = 1;
    description.SampleDesc.Count = 1;
    description.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    for (std::uint32_t i = 0; i < kFrameCount; ++i)
    {
        // Default-heap destination (fast GPU reads), created in the SRV read state.
        D3D12_HEAP_PROPERTIES defaultHeap{};
        defaultHeap.Type = D3D12_HEAP_TYPE_DEFAULT;
        if (FAILED(device_.Get()->CreateCommittedResource(
            &defaultHeap,
            D3D12_HEAP_FLAG_NONE,
            &description,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
            nullptr,
            IID_PPV_ARGS(&gpuModel.boneMatricesBuffers[i]))))
        {
            throw std::runtime_error("Failed to create the bone-matrix buffer.");
        }

        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.Format = DXGI_FORMAT_UNKNOWN;
        srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Buffer.FirstElement = 0;
        srv.Buffer.NumElements = boneCount;
        srv.Buffer.StructureByteStride = sizeof(DirectX::XMFLOAT4X4);
        device_.Get()->CreateShaderResourceView(gpuModel.boneMatricesBuffers[i].Get(), &srv, srvCpu);
        gpuModel.boneMatricesSrv[i] = srvGpu;
        srvCpu.ptr += srvDescriptorSize_;
        srvGpu.ptr += srvDescriptorSize_;

        // Upload staging (CPU-writable), double-buffered so the CPU never overwrites a buffer
        // while the GPU is still copying from it.
        D3D12_HEAP_PROPERTIES uploadHeap{};
        uploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;
        if (FAILED(device_.Get()->CreateCommittedResource(
            &uploadHeap,
            D3D12_HEAP_FLAG_NONE,
            &description,
            D3D12_RESOURCE_STATE_GENERIC_READ,
            nullptr,
            IID_PPV_ARGS(&gpuModel.boneMatricesStaging[i]))))
        {
            throw std::runtime_error("Failed to create the bone-matrix staging buffer.");
        }
        gpuModel.boneMatricesStaging[i]->Map(0, nullptr, &gpuModel.boneMatricesStagingMapped[i]);
    }

    // Skin-reference-bone table SRV (static), placed after the bone-matrix SRVs.
    if (gpuModel.refBonesBuffer != nullptr)
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC refBoneSrv{};
        refBoneSrv.Format = DXGI_FORMAT_UNKNOWN;
        refBoneSrv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        refBoneSrv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        refBoneSrv.Buffer.FirstElement = 0;
        refBoneSrv.Buffer.NumElements = static_cast<UINT>(model.mesh.refBones.size());
        refBoneSrv.Buffer.StructureByteStride = sizeof(std::uint32_t);
        device_.Get()->CreateShaderResourceView(gpuModel.refBonesBuffer.Get(), &refBoneSrv, srvCpu);
        gpuModel.refBonesSrv = srvGpu;
        srvCpu.ptr += srvDescriptorSize_;
        srvGpu.ptr += srvDescriptorSize_;
    }

    // Morph deltas: double-buffered default-heap structured buffers (float3 per vertex), each with
    // an SRV plus a persistent upload staging buffer, mirroring the bone matrices. A model without
    // vertex morphs still gets the buffer, zero-filled by the per-frame upload, so the vertex
    // shader always reads a valid resource.
    const std::uint32_t morphVertexCount = std::max<std::uint32_t>(
        1u, static_cast<std::uint32_t>(model.mesh.vertices.size()));
    D3D12_RESOURCE_DESC morphDescription{};
    morphDescription.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    morphDescription.Width = static_cast<UINT64>(morphVertexCount) * 3 * sizeof(float);
    morphDescription.Height = 1;
    morphDescription.DepthOrArraySize = 1;
    morphDescription.MipLevels = 1;
    morphDescription.SampleDesc.Count = 1;
    morphDescription.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    for (std::uint32_t i = 0; i < kFrameCount; ++i)
    {
        D3D12_HEAP_PROPERTIES morphDefaultHeap{};
        morphDefaultHeap.Type = D3D12_HEAP_TYPE_DEFAULT;
        if (FAILED(device_.Get()->CreateCommittedResource(
            &morphDefaultHeap,
            D3D12_HEAP_FLAG_NONE,
            &morphDescription,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
            nullptr,
            IID_PPV_ARGS(&gpuModel.morphDeltaBuffers[i]))))
        {
            throw std::runtime_error("Failed to create the morph-delta buffer.");
        }

        D3D12_SHADER_RESOURCE_VIEW_DESC morphSrv{};
        morphSrv.Format = DXGI_FORMAT_UNKNOWN;
        morphSrv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        morphSrv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        morphSrv.Buffer.FirstElement = 0;
        morphSrv.Buffer.NumElements = morphVertexCount;
        morphSrv.Buffer.StructureByteStride = 3 * sizeof(float);
        device_.Get()->CreateShaderResourceView(gpuModel.morphDeltaBuffers[i].Get(), &morphSrv, srvCpu);
        gpuModel.morphDeltaSrv[i] = srvGpu;
        srvCpu.ptr += srvDescriptorSize_;
        srvGpu.ptr += srvDescriptorSize_;

        D3D12_HEAP_PROPERTIES morphUploadHeap{};
        morphUploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;
        if (FAILED(device_.Get()->CreateCommittedResource(
            &morphUploadHeap,
            D3D12_HEAP_FLAG_NONE,
            &morphDescription,
            D3D12_RESOURCE_STATE_GENERIC_READ,
            nullptr,
            IID_PPV_ARGS(&gpuModel.morphDeltaStaging[i]))))
        {
            throw std::runtime_error("Failed to create the morph-delta staging buffer.");
        }
        gpuModel.morphDeltaStaging[i]->Map(0, nullptr, &gpuModel.morphDeltaStagingMapped[i]);
    }

    // Reflection-texture SRV in the trailing slot, sampled by the reflective-floor pixel shader.
    // The view is recreated here and again on resize when the reflection target is rebuilt.
    D3D12_SHADER_RESOURCE_VIEW_DESC reflectionSrv{};
    reflectionSrv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    reflectionSrv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    reflectionSrv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    reflectionSrv.Texture2D.MipLevels = 1;
    device_.Get()->CreateShaderResourceView(reflectionTarget_.Get(), &reflectionSrv, srvCpu);
    gpuModel.reflectionSrvCpu = srvCpu;
    gpuModel.reflectionSrvGpu = srvGpu;

    if (residentModels_.size() <= modelIndex)
    {
        residentModels_.resize(modelIndex + 1);
    }
    residentModels_[modelIndex] = std::make_unique<GpuModel>(std::move(gpuModel));
}

void Dx12Renderer::CreatePipelineState()
{
    const auto vertexShader = ShaderCompiler::Compile(MeshVertexShaderSource, "VSMain", "vs_5_1");
    const auto pixelShader = ShaderCompiler::Compile(MeshPixelShaderSource, "PSMain", "ps_5_1");

    const D3D12_SHADER_BYTECODE vertexBytecode = { vertexShader->GetBufferPointer(), vertexShader->GetBufferSize() };
    const D3D12_SHADER_BYTECODE pixelBytecode = { pixelShader->GetBufferPointer(), pixelShader->GetBufferSize() };

    const D3D12_INPUT_ELEMENT_DESC inputLayout[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "NORMAL", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 16, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 32, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 1, DXGI_FORMAT_R32G32_FLOAT, 0, 40, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "BLENDINDICES", 0, DXGI_FORMAT_R8G8B8A8_UINT, 1, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "BLENDWEIGHT", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 4, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    };

    D3D12_GRAPHICS_PIPELINE_STATE_DESC description{};
    description.pRootSignature = rootSignature_.Get();
    description.VS = vertexBytecode;
    description.PS = pixelBytecode;

    description.BlendState.AlphaToCoverageEnable = FALSE;
    description.BlendState.IndependentBlendEnable = FALSE;
    for (UINT i = 0; i < D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT; ++i)
    {
        description.BlendState.RenderTarget[i].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    }

    description.SampleMask = UINT_MAX;
    description.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    description.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    description.RasterizerState.DepthClipEnable = TRUE;
    description.DepthStencilState.DepthEnable = TRUE;
    description.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    description.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
    description.DepthStencilState.StencilEnable = FALSE;
    description.DSVFormat = DXGI_FORMAT_D32_FLOAT;

    description.InputLayout.NumElements = 6;
    description.InputLayout.pInputElementDescs = inputLayout;

    description.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    description.NumRenderTargets = 1;
    description.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    description.SampleDesc.Count = 1;

    // Two pipeline states differ only by cull mode: single-sided materials backface-cull,
    // double-sided materials (PMX flag 0x01) render both faces.
    const auto createPipeline = [&](const D3D12_CULL_MODE cullMode)
    {
        description.RasterizerState.CullMode = cullMode;
        Microsoft::WRL::ComPtr<ID3D12PipelineState> pipelineState;
        const HRESULT result = device_.Get()->CreateGraphicsPipelineState(
            &description, IID_PPV_ARGS(&pipelineState));
        if (FAILED(result))
        {
            DumpD3d12Messages(device_.Get());
            char message[128];
            std::snprintf(message, sizeof(message), "Failed to create the graphics pipeline state (HRESULT 0x%08X).", static_cast<unsigned int>(result));
            throw std::runtime_error(message);
        }
        return pipelineState;
    };

    pipelineStateCulled_ = createPipeline(D3D12_CULL_MODE_BACK);
    pipelineStateDoubleSided_ = createPipeline(D3D12_CULL_MODE_NONE);

    // The reflective-floor pipeline: the same skinned vertex shader with a pixel shader that
    // samples the offscreen reflection texture at screen-space UV. Culling is disabled because the
    // floor is double-sided.
    {
        const auto reflectionPixelShader = ShaderCompiler::Compile(ReflectPixelShaderSource, "ReflectPSMain", "ps_5_1");
        D3D12_GRAPHICS_PIPELINE_STATE_DESC reflectionDescription = description;
        reflectionDescription.PS = { reflectionPixelShader->GetBufferPointer(), reflectionPixelShader->GetBufferSize() };
        reflectionDescription.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        const HRESULT result = device_.Get()->CreateGraphicsPipelineState(
            &reflectionDescription, IID_PPV_ARGS(&reflectionPso_));
        if (FAILED(result))
        {
            DumpD3d12Messages(device_.Get());
            char message[128];
            std::snprintf(message, sizeof(message), "Failed to create the reflection pipeline state (HRESULT 0x%08X).", static_cast<unsigned int>(result));
            throw std::runtime_error(message);
        }
    }
}

void Dx12Renderer::CreateRenderTargetViews()
{
    if (rtvHeap_ == nullptr)
    {
        D3D12_DESCRIPTOR_HEAP_DESC description{};
        description.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        // One RTV per back buffer plus the offscreen reflection target (created separately).
        description.NumDescriptors = kFrameCount + 1;
        description.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
        description.NodeMask = 0;

        if (FAILED(device_.Get()->CreateDescriptorHeap(&description, IID_PPV_ARGS(&rtvHeap_))))
        {
            throw std::runtime_error("Failed to create the render-target-view descriptor heap.");
        }
    }

    D3D12_CPU_DESCRIPTOR_HANDLE handle = rtvHeap_->GetCPUDescriptorHandleForHeapStart();
    for (std::uint32_t i = 0; i < kFrameCount; ++i)
    {
        if (FAILED(swapChain_.Get()->GetBuffer(i, IID_PPV_ARGS(&backBuffers_[i]))))
        {
            throw std::runtime_error("Failed to get a swap chain back buffer.");
        }
        device_.Get()->CreateRenderTargetView(backBuffers_[i].Get(), nullptr, handle);
        handle.ptr += rtvDescriptorSize_;
    }
}

void Dx12Renderer::CreateDepthBuffer()
{
    D3D12_HEAP_PROPERTIES heapProperties{};
    heapProperties.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC description{};
    description.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    description.Width = width_;
    description.Height = height_;
    description.DepthOrArraySize = 1;
    description.MipLevels = 1;
    description.Format = DXGI_FORMAT_D32_FLOAT;
    description.SampleDesc.Count = 1;
    description.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    description.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

    D3D12_CLEAR_VALUE clearValue{};
    clearValue.Format = DXGI_FORMAT_D32_FLOAT;
    clearValue.DepthStencil.Depth = 1.0f;
    clearValue.DepthStencil.Stencil = 0;

    if (FAILED(device_.Get()->CreateCommittedResource(
        &heapProperties,
        D3D12_HEAP_FLAG_NONE,
        &description,
        D3D12_RESOURCE_STATE_DEPTH_WRITE,
        &clearValue,
        IID_PPV_ARGS(&depthBuffer_))))
    {
        throw std::runtime_error("Failed to create the depth buffer.");
    }

    if (dsvHeap_ == nullptr)
    {
        D3D12_DESCRIPTOR_HEAP_DESC heapDescription{};
        heapDescription.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
        heapDescription.NumDescriptors = 1;
        heapDescription.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
        heapDescription.NodeMask = 0;

        if (FAILED(device_.Get()->CreateDescriptorHeap(&heapDescription, IID_PPV_ARGS(&dsvHeap_))))
        {
            throw std::runtime_error("Failed to create the depth-stencil-view descriptor heap.");
        }
    }

    device_.Get()->CreateDepthStencilView(
        depthBuffer_.Get(),
        nullptr,
        dsvHeap_->GetCPUDescriptorHandleForHeapStart());
}

void Dx12Renderer::CreateReflectionTarget()
{
    D3D12_HEAP_PROPERTIES heapProperties{};
    heapProperties.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC description{};
    description.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    description.Width = width_;
    description.Height = height_;
    description.DepthOrArraySize = 1;
    description.MipLevels = 1;
    description.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    description.SampleDesc.Count = 1;
    description.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    description.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

    D3D12_CLEAR_VALUE clearValue{};
    clearValue.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    clearValue.Color[0] = 0.0f;
    clearValue.Color[1] = 0.0f;
    clearValue.Color[2] = 0.0f;
    clearValue.Color[3] = 1.0f;

    if (FAILED(device_.Get()->CreateCommittedResource(
        &heapProperties,
        D3D12_HEAP_FLAG_NONE,
        &description,
        D3D12_RESOURCE_STATE_RENDER_TARGET,
        &clearValue,
        IID_PPV_ARGS(&reflectionTarget_))))
    {
        throw std::runtime_error("Failed to create the reflection render target.");
    }

    // Render-target view in the trailing slot of the RTV heap, after the swap-chain back buffers.
    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = rtvHeap_->GetCPUDescriptorHandleForHeapStart();
    rtvHandle.ptr += static_cast<SIZE_T>(kFrameCount) * rtvDescriptorSize_;
    device_.Get()->CreateRenderTargetView(reflectionTarget_.Get(), nullptr, rtvHandle);

    reflectionTargetIsSrv_ = false;
}

void Dx12Renderer::CreateMeshBuffers(GpuModel& model, const Model& cpuModel)
{
    const MeshAsset& mesh = cpuModel.mesh;

    if (FAILED(commandAllocators_[0]->Reset()))
    {
        throw std::runtime_error("Failed to reset the command allocator.");
    }
    if (FAILED(commandList_->Reset(commandAllocators_[0].Get(), nullptr)))
    {
        throw std::runtime_error("Failed to reset the command list.");
    }

    // Upload staging buffers must stay alive until the copy has executed, so they are kept
    // in this vector until after the fence wait below.
    std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>> uploadBuffers;

    const auto upload = [&](const void* data, const std::size_t size, const D3D12_RESOURCE_STATES finalState)
    {
        D3D12_HEAP_PROPERTIES defaultHeap{};
        defaultHeap.Type = D3D12_HEAP_TYPE_DEFAULT;

        D3D12_RESOURCE_DESC description{};
        description.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        description.Width = size;
        description.Height = 1;
        description.DepthOrArraySize = 1;
        description.MipLevels = 1;
        description.SampleDesc.Count = 1;
        description.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

        Microsoft::WRL::ComPtr<ID3D12Resource> buffer;
        if (FAILED(device_.Get()->CreateCommittedResource(
            &defaultHeap,
            D3D12_HEAP_FLAG_NONE,
            &description,
            D3D12_RESOURCE_STATE_COPY_DEST,
            nullptr,
            IID_PPV_ARGS(&buffer))))
        {
            throw std::runtime_error("Failed to create a mesh buffer.");
        }

        D3D12_HEAP_PROPERTIES uploadHeap{};
        uploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;
        Microsoft::WRL::ComPtr<ID3D12Resource> staging;
        if (FAILED(device_.Get()->CreateCommittedResource(
            &uploadHeap,
            D3D12_HEAP_FLAG_NONE,
            &description,
            D3D12_RESOURCE_STATE_GENERIC_READ,
            nullptr,
            IID_PPV_ARGS(&staging))))
        {
            throw std::runtime_error("Failed to create a mesh upload buffer.");
        }

        void* mapped = nullptr;
        staging->Map(0, nullptr, &mapped);
        std::memcpy(mapped, data, size);
        staging->Unmap(0, nullptr);

        commandList_->CopyBufferRegion(buffer.Get(), 0, staging.Get(), 0, size);

        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = buffer.Get();
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        barrier.Transition.StateAfter = finalState;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        commandList_->ResourceBarrier(1, &barrier);

        uploadBuffers.push_back(staging);
        return buffer;
    };

    const std::size_t vertexSize = mesh.vertices.size() * sizeof(MmdlVertex);
    model.vertexBuffer = upload(mesh.vertices.data(), vertexSize, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
    model.vertexView.BufferLocation = model.vertexBuffer->GetGPUVirtualAddress();
    model.vertexView.StrideInBytes = sizeof(MmdlVertex);
    model.vertexView.SizeInBytes = static_cast<UINT>(vertexSize);

    const std::size_t indexSize = mesh.indices.size() * sizeof(std::uint32_t);
    model.indexBuffer = upload(mesh.indices.data(), indexSize, D3D12_RESOURCE_STATE_INDEX_BUFFER);
    model.indexView.BufferLocation = model.indexBuffer->GetGPUVirtualAddress();
    model.indexView.Format = DXGI_FORMAT_R32_UINT;
    model.indexView.SizeInBytes = static_cast<UINT>(indexSize);

    // Skinning weights: a second per-vertex stream (BLENDINDICES/BLENDWEIGHT), parallel to the
    // position/normal/uv buffer and uploaded from the model's CPU-side skinning array.
    const std::size_t skinningSize = cpuModel.skinning.size() * sizeof(SkinningVertex);
    model.skinningBuffer = upload(cpuModel.skinning.data(), skinningSize, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
    model.skinningView.BufferLocation = model.skinningBuffer->GetGPUVirtualAddress();
    model.skinningView.StrideInBytes = sizeof(SkinningVertex);
    model.skinningView.SizeInBytes = static_cast<UINT>(skinningSize);

    // Skin-reference-bone table: widen the u16 global indices to u32 for the structured buffer
    // and upload once (it is static, unlike the per-frame bone matrices).
    if (!cpuModel.mesh.refBones.empty())
    {
        const std::vector<std::uint32_t> refBonesU32(cpuModel.mesh.refBones.begin(), cpuModel.mesh.refBones.end());
        const std::size_t refBonesSize = refBonesU32.size() * sizeof(std::uint32_t);
        model.refBonesBuffer = upload(refBonesU32.data(), refBonesSize, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }

    if (FAILED(commandList_->Close()))
    {
        throw std::runtime_error("Failed to close the command list.");
    }

    ID3D12CommandList* commandLists[] = { commandList_.Get() };
    queue_.Get()->ExecuteCommandLists(1, commandLists);

    fenceValues_[0] = ++currentFence_;
    queue_.Get()->Signal(fences_[0].Get(), fenceValues_[0]);
    WaitForPreviousFrame(0);

    if (FAILED(commandAllocators_[0]->Reset()))
    {
        throw std::runtime_error("Failed to reset the command allocator.");
    }
}

namespace
{
// Maps the runtime texture format to the DXGI block-compressed or uncompressed format.
DXGI_FORMAT ToDxgiFormat(const TextureFormat format)
{
    switch (format)
    {
    case TextureFormat::BC1: return DXGI_FORMAT_BC1_UNORM;
    case TextureFormat::BC7: return DXGI_FORMAT_BC7_UNORM;
    case TextureFormat::RGBA8:
    default: return DXGI_FORMAT_R8G8B8A8_UNORM;
    }
}
} // namespace

void Dx12Renderer::CreateTextures(GpuModel& model, const std::span<const Image> images)
{
    // One three-descriptor (base/toon/sphere) SRV bundle per material, plus the trailing slots
    // for the per-model skinning/morph SRVs and the shared reflection-texture SRV.
    D3D12_DESCRIPTOR_HEAP_DESC heapDescription{};
    heapDescription.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heapDescription.NumDescriptors = static_cast<UINT>(model.materials.size()) * 3 + 2 * kFrameCount + 2;
    heapDescription.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    heapDescription.NodeMask = 0;

    if (FAILED(device_.Get()->CreateDescriptorHeap(&heapDescription, IID_PPV_ARGS(&model.srvHeap))))
    {
        throw std::runtime_error("Failed to create the texture SRV descriptor heap.");
    }

    if (FAILED(commandAllocators_[0]->Reset()))
    {
        throw std::runtime_error("Failed to reset the command allocator.");
    }
    if (FAILED(commandList_->Reset(commandAllocators_[0].Get(), nullptr)))
    {
        throw std::runtime_error("Failed to reset the command list.");
    }

    // Upload staging buffers must stay alive until the copies have executed.
    std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>> stagingBuffers;

    const auto uploadTexture = [&](const Image& image)
    {
        D3D12_RESOURCE_DESC description{};
        description.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        description.Width = image.width;
        description.Height = image.height;
        description.DepthOrArraySize = 1;
        description.MipLevels = 1;
        description.Format = ToDxgiFormat(image.format);
        description.SampleDesc.Count = 1;
        description.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        description.Flags = D3D12_RESOURCE_FLAG_NONE;

        D3D12_HEAP_PROPERTIES defaultHeap{};
        defaultHeap.Type = D3D12_HEAP_TYPE_DEFAULT;

        Microsoft::WRL::ComPtr<ID3D12Resource> texture;
        if (FAILED(device_.Get()->CreateCommittedResource(
            &defaultHeap,
            D3D12_HEAP_FLAG_NONE,
            &description,
            D3D12_RESOURCE_STATE_COPY_DEST,
            nullptr,
            IID_PPV_ARGS(&texture))))
        {
            throw std::runtime_error("Failed to create a texture.");
        }

        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
        UINT numRows = 0;
        UINT64 rowSizeBytes = 0;
        UINT64 totalBytes = 0;
        device_.Get()->GetCopyableFootprints(
            &description, 0, 1, 0, &footprint, &numRows, &rowSizeBytes, &totalBytes);

        D3D12_HEAP_PROPERTIES uploadHeap{};
        uploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;

        D3D12_RESOURCE_DESC bufferDescription{};
        bufferDescription.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bufferDescription.Width = totalBytes;
        bufferDescription.Height = 1;
        bufferDescription.DepthOrArraySize = 1;
        bufferDescription.MipLevels = 1;
        bufferDescription.SampleDesc.Count = 1;
        bufferDescription.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

        Microsoft::WRL::ComPtr<ID3D12Resource> staging;
        if (FAILED(device_.Get()->CreateCommittedResource(
            &uploadHeap,
            D3D12_HEAP_FLAG_NONE,
            &bufferDescription,
            D3D12_RESOURCE_STATE_GENERIC_READ,
            nullptr,
            IID_PPV_ARGS(&staging))))
        {
            throw std::runtime_error("Failed to create a texture upload buffer.");
        }

        void* mapped = nullptr;
        staging->Map(0, nullptr, &mapped);
        const std::size_t sourceRowBytes = static_cast<std::size_t>(rowSizeBytes);
        auto* destination = static_cast<std::uint8_t*>(mapped);
        for (UINT row = 0; row < numRows; ++row)
        {
            std::memcpy(
                destination + row * footprint.Footprint.RowPitch,
                image.pixels.data() + row * sourceRowBytes,
                sourceRowBytes);
        }
        staging->Unmap(0, nullptr);

        D3D12_TEXTURE_COPY_LOCATION destinationLocation{};
        destinationLocation.pResource = texture.Get();
        destinationLocation.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        destinationLocation.SubresourceIndex = 0;

        D3D12_TEXTURE_COPY_LOCATION sourceLocation{};
        sourceLocation.pResource = staging.Get();
        sourceLocation.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        sourceLocation.PlacedFootprint = footprint;

        commandList_->CopyTextureRegion(&destinationLocation, 0, 0, 0, &sourceLocation, nullptr);

        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = texture.Get();
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        commandList_->ResourceBarrier(1, &barrier);

        stagingBuffers.push_back(staging);
        return texture;
    };

    model.textures.reserve(images.size());
    model.textureFormats.reserve(images.size());
    for (const Image& image : images)
    {
        model.textures.push_back(uploadTexture(image));
        model.textureFormats.push_back(ToDxgiFormat(image.format));
    }

    const std::uint8_t whitePixel[4] = { 255, 255, 255, 255 };
    Image whiteImage;
    whiteImage.width = 1;
    whiteImage.height = 1;
    whiteImage.pixels.assign(whitePixel, whitePixel + 4);
    model.whiteTexture = uploadTexture(whiteImage);

    // A 256x1 grayscale ramp used as the toon fallback for materials without a toon texture
    // (PMX shared toon, or none). Sampled along its length by the pixel shader's ramp coordinate.
    Image rampImage;
    rampImage.width = 256;
    rampImage.height = 1;
    rampImage.pixels.resize(256 * 4);
    for (std::uint32_t x = 0; x < 256; ++x)
    {
        const std::uint8_t value = static_cast<std::uint8_t>(x);
        rampImage.pixels[x * 4 + 0] = value;
        rampImage.pixels[x * 4 + 1] = value;
        rampImage.pixels[x * 4 + 2] = value;
        rampImage.pixels[x * 4 + 3] = 255;
    }
    model.toonRampTexture = uploadTexture(rampImage);

    if (FAILED(commandList_->Close()))
    {
        throw std::runtime_error("Failed to close the command list.");
    }

    ID3D12CommandList* commandLists[] = { commandList_.Get() };
    queue_.Get()->ExecuteCommandLists(1, commandLists);

    fenceValues_[0] = ++currentFence_;
    queue_.Get()->Signal(fences_[0].Get(), fenceValues_[0]);
    WaitForPreviousFrame(0);

    if (FAILED(commandAllocators_[0]->Reset()))
    {
        throw std::runtime_error("Failed to reset the command allocator.");
    }

    // Create the per-material SRV bundles (three descriptors each: base, toon, sphere).
    D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle = model.srvHeap->GetCPUDescriptorHandleForHeapStart();
    D3D12_GPU_DESCRIPTOR_HANDLE gpuHandle = model.srvHeap->GetGPUDescriptorHandleForHeapStart();

    const auto createSrv = [&](ID3D12Resource* texture, const DXGI_FORMAT format)
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.Format = format;
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Texture2D.MipLevels = 1;
        device_.Get()->CreateShaderResourceView(texture, &srv, cpuHandle);
        const D3D12_GPU_DESCRIPTOR_HANDLE handle = gpuHandle;
        cpuHandle.ptr += srvDescriptorSize_;
        gpuHandle.ptr += srvDescriptorSize_;
        return handle;
    };

    // Per-material three-SRV bundle (base, toon, sphere), each falling back to a neutral
    // texture when the material omits one.
    model.materialSrvBundles.reserve(model.materials.size());
    const auto texFormat = [&](const std::int32_t index)
    {
        return index >= 0
            ? model.textureFormats[static_cast<std::size_t>(index)]
            : DXGI_FORMAT_R8G8B8A8_UNORM;
    };
    for (const MMDToonMaterial& material : model.materials)
    {
        ID3D12Resource* base = material.baseColorTexture >= 0
            ? model.textures[static_cast<std::size_t>(material.baseColorTexture)].Get()
            : model.whiteTexture.Get();
        ID3D12Resource* toon = material.toonTexture >= 0
            ? model.textures[static_cast<std::size_t>(material.toonTexture)].Get()
            : model.toonRampTexture.Get();
        ID3D12Resource* sphere = material.sphereTexture >= 0
            ? model.textures[static_cast<std::size_t>(material.sphereTexture)].Get()
            : model.whiteTexture.Get();

        const D3D12_GPU_DESCRIPTOR_HANDLE bundleStart = gpuHandle;
        createSrv(base, texFormat(material.baseColorTexture));
        createSrv(toon, texFormat(material.toonTexture));
        createSrv(sphere, texFormat(material.sphereTexture));
        model.materialSrvBundles.push_back(bundleStart);
    }
}

void Dx12Renderer::CreateConstantBuffer()
{
    D3D12_HEAP_PROPERTIES uploadHeap{};
    uploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC description{};
    description.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    description.Width = 256;
    description.Height = 1;
    description.DepthOrArraySize = 1;
    description.MipLevels = 1;
    description.SampleDesc.Count = 1;
    description.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    for (std::uint32_t i = 0; i < kFrameCount; ++i)
    {
        if (FAILED(device_.Get()->CreateCommittedResource(
            &uploadHeap,
            D3D12_HEAP_FLAG_NONE,
            &description,
            D3D12_RESOURCE_STATE_GENERIC_READ,
            nullptr,
            IID_PPV_ARGS(&constantBuffers_[i]))))
        {
            throw std::runtime_error("Failed to create a camera constant buffer.");
        }
        // Upload-heap buffers stay persistently mapped; UpdateCameraConstants() writes each frame.
        constantBuffers_[i]->Map(0, nullptr, &constantBufferMapped_[i]);
    }

    // The reflection pass's camera buffer, allocated alongside the main one for the same size and
    // lifetime; UpdateReflectionConstants() writes it.
    for (std::uint32_t i = 0; i < kFrameCount; ++i)
    {
        if (FAILED(device_.Get()->CreateCommittedResource(
            &uploadHeap,
            D3D12_HEAP_FLAG_NONE,
            &description,
            D3D12_RESOURCE_STATE_GENERIC_READ,
            nullptr,
            IID_PPV_ARGS(&reflectionConstantBuffers_[i]))))
        {
            throw std::runtime_error("Failed to create the reflection camera constant buffer.");
        }
        reflectionConstantBuffers_[i]->Map(0, nullptr, &reflectionConstantBufferMapped_[i]);
    }
}

void Dx12Renderer::UpdateCameraConstants(const std::uint32_t frameIndex, const Camera& camera)
{
    using namespace DirectX;

    const XMMATRIX view = camera.ViewMatrix();
    const float aspect = static_cast<float>(width_) / static_cast<float>(height_);
    const XMMATRIX projection = camera.ProjectionMatrix(aspect);

    // HLSL mul(matrix, pos) treats pos as a column vector; DirectXMath is row-vector, and
    // XMStoreFloat4x4 lays each matrix out in the column-major order HLSL expects, so both
    // matrices are stored directly (no transpose). See CameraConstants in Shaders.h.
    CameraConstants constants{};
    XMFLOAT4X4 storage;
    XMStoreFloat4x4(&storage, XMMatrixMultiply(view, projection));
    std::memcpy(constants.viewProjection, &storage, sizeof(storage));
    XMStoreFloat4x4(&storage, view);
    std::memcpy(constants.view, &storage, sizeof(storage));

    constants.lightDirection[0] = -0.3f; // Fixed key light (the shader normalizes it).
    constants.lightDirection[1] = -0.8f;
    constants.lightDirection[2] = -0.6f;

    // Camera forward (local +Z in world space), for the pixel shader's view direction.
    const XMVECTOR forward = XMVector3Rotate(
        XMVectorSet(0.0f, 0.0f, 1.0f, 0.0f),
        XMQuaternionRotationRollPitchYaw(
            XMConvertToRadians(camera.rotation[0]),
            XMConvertToRadians(camera.rotation[1]),
            XMConvertToRadians(camera.rotation[2])));
    constants.cameraDirection[0] = XMVectorGetX(forward);
    constants.cameraDirection[1] = XMVectorGetY(forward);
    constants.cameraDirection[2] = XMVectorGetZ(forward);
    constants.cameraDirection[3] = 0.0f;

    std::memcpy(constantBufferMapped_[frameIndex], &constants, sizeof(constants));
}

void Dx12Renderer::UpdateReflectionConstants(
    const std::uint32_t frameIndex,
    const Camera& camera,
    const float planePoint[3],
    const float planeNormal[3])
{
    using namespace DirectX;

    // The main camera's view matrix (the same construction UpdateCameraConstants uses).
    const XMMATRIX rotation = XMMatrixRotationRollPitchYaw(
        XMConvertToRadians(camera.rotation[0]),
        XMConvertToRadians(camera.rotation[1]),
        XMConvertToRadians(camera.rotation[2]));
    const XMMATRIX translation = XMMatrixTranslation(
        camera.position[0], camera.position[1], camera.position[2]);
    const XMMATRIX view = XMMatrixInverse(nullptr, XMMatrixMultiply(rotation, translation));

    const float aspect = static_cast<float>(width_) / static_cast<float>(height_);
    const XMMATRIX projection = XMMatrixPerspectiveFovLH(
        XMConvertToRadians(camera.fovDegrees), aspect, camera.nearPlane, camera.farPlane);

    // Reflect the world across the floor plane, then view it: geometry above the plane appears
    // mirrored below it, which is what a mirror shows. XMMatrixReflect takes the coefficients
    // (a, b, c, d) of the plane ax + by + cz + d = 0.
    const XMVECTOR normal = XMVector3Normalize(
        XMVectorSet(planeNormal[0], planeNormal[1], planeNormal[2], 0.0f));
    const float planeOffset = XMVectorGetX(XMVector3Dot(
        normal, XMVectorSet(planePoint[0], planePoint[1], planePoint[2], 0.0f)));
    const XMMATRIX reflect = XMMatrixReflect(XMVectorSet(
        XMVectorGetX(normal), XMVectorGetY(normal), XMVectorGetZ(normal), -planeOffset));
    // Row-vector: reflect the point first, then apply the view. Its determinant is negative, so
    // the reflection pass disables back-face culling to compensate for the flipped winding.
    const XMMATRIX reflectedView = XMMatrixMultiply(reflect, view);

    CameraConstants constants{};
    XMFLOAT4X4 storage;
    XMStoreFloat4x4(&storage, XMMatrixMultiply(reflectedView, projection));
    std::memcpy(constants.viewProjection, &storage, sizeof(storage));
    XMStoreFloat4x4(&storage, reflectedView);
    std::memcpy(constants.view, &storage, sizeof(storage));

    constants.lightDirection[0] = -0.3f; // Same fixed key light as the main camera.
    constants.lightDirection[1] = -0.8f;
    constants.lightDirection[2] = -0.6f;

    // View direction for the specular term, reflected across the plane to match the mirrored view.
    const XMVECTOR forward = XMVector3Rotate(
        XMVectorSet(0.0f, 0.0f, 1.0f, 0.0f),
        XMQuaternionRotationRollPitchYaw(
            XMConvertToRadians(camera.rotation[0]),
            XMConvertToRadians(camera.rotation[1]),
            XMConvertToRadians(camera.rotation[2])));
    const XMVECTOR reflectedForward = XMVectorSubtract(
        forward, XMVectorScale(normal, 2.0f * XMVectorGetX(XMVector3Dot(normal, forward))));
    constants.cameraDirection[0] = XMVectorGetX(reflectedForward);
    constants.cameraDirection[1] = XMVectorGetY(reflectedForward);
    constants.cameraDirection[2] = XMVectorGetZ(reflectedForward);
    constants.cameraDirection[3] = 0.0f;

    std::memcpy(reflectionConstantBufferMapped_[frameIndex], &constants, sizeof(constants));
}

void Dx12Renderer::WaitForPreviousFrame(const std::uint32_t frameIndex)
{
    if (fences_[frameIndex]->GetCompletedValue() < fenceValues_[frameIndex])
    {
        fences_[frameIndex]->SetEventOnCompletion(fenceValues_[frameIndex], fenceEvent_);
        WaitForSingleObject(fenceEvent_, INFINITE);
    }
}

void Dx12Renderer::WaitForGpuIdle()
{
    if (frameFenceValue_ == 0)
    {
        return; // Nothing submitted yet.
    }
    if (frameFence_->GetCompletedValue() < frameFenceValue_)
    {
        frameFence_->SetEventOnCompletion(frameFenceValue_, fenceEvent_);
        WaitForSingleObject(fenceEvent_, INFINITE);
    }
}

void Dx12Renderer::CreateImGuiSrvHeap()
{
    imguiSrvDescriptorSize_ = device_.Get()->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    D3D12_DESCRIPTOR_HEAP_DESC description{};
    description.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    description.NumDescriptors = kImGuiSrvCount;
    description.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    description.NodeMask = 0;

    if (FAILED(device_.Get()->CreateDescriptorHeap(&description, IID_PPV_ARGS(&imguiSrvHeap_))))
    {
        throw std::runtime_error("Failed to create the imgui SRV descriptor heap.");
    }

    imguiSrvHeapCpuStart_ = imguiSrvHeap_->GetCPUDescriptorHandleForHeapStart();
    imguiSrvHeapGpuStart_ = imguiSrvHeap_->GetGPUDescriptorHandleForHeapStart();

    imguiSrvFreeIndices_.reserve(kImGuiSrvCount);
    for (std::uint32_t i = kImGuiSrvCount; i > 0; --i)
    {
        imguiSrvFreeIndices_.push_back(i - 1);
    }
}

void Dx12Renderer::AllocImGuiSrvDescriptor(D3D12_CPU_DESCRIPTOR_HANDLE* const cpu, D3D12_GPU_DESCRIPTOR_HANDLE* const gpu)
{
    const std::uint32_t index = imguiSrvFreeIndices_.back();
    imguiSrvFreeIndices_.pop_back();
    cpu->ptr = imguiSrvHeapCpuStart_.ptr + static_cast<SIZE_T>(index) * imguiSrvDescriptorSize_;
    gpu->ptr = imguiSrvHeapGpuStart_.ptr + static_cast<SIZE_T>(index) * imguiSrvDescriptorSize_;
}

void Dx12Renderer::FreeImGuiSrvDescriptor(const D3D12_CPU_DESCRIPTOR_HANDLE cpu, const D3D12_GPU_DESCRIPTOR_HANDLE gpu)
{
    (void)gpu; // The CPU handle alone identifies the slot; gpu mirrors it.
    const std::uint32_t index = static_cast<std::uint32_t>((cpu.ptr - imguiSrvHeapCpuStart_.ptr) / imguiSrvDescriptorSize_);
    imguiSrvFreeIndices_.push_back(index);
}

void Dx12Renderer::UploadPerFrameBuffers(
    const std::uint32_t frameIndex,
    const std::span<const ModelInstance> instances,
    const std::span<const Model> models,
    const std::span<const DirectX::XMFLOAT4X4> bonePalette,
    const std::span<const std::uint32_t> bonePaletteOffsets,
    const std::span<const float> morphDeltas,
    const std::span<const std::uint32_t> morphDeltaOffsets)
{
    for (const ModelInstance& instance : instances)
    {
        if (!instance.visible || instance.modelIndex >= models.size())
        {
            continue;
        }
        if (instance.modelIndex >= residentModels_.size() || residentModels_[instance.modelIndex] == nullptr)
        {
            continue;
        }

        const GpuModel& gpuModel = *residentModels_[instance.modelIndex];
        const std::uint32_t modelIndex = static_cast<std::uint32_t>(instance.modelIndex);

        // Upload this model's skinning palette: write the staging buffer, copy it into the
        // default-heap bone buffer on the GPU, and transition the buffer back to SRV read.
        if (modelIndex + 1 < bonePaletteOffsets.size())
        {
            const std::uint32_t first = bonePaletteOffsets[modelIndex];
            const std::uint32_t count = bonePaletteOffsets[modelIndex + 1] - first;
            if (count > 0 && first + count <= bonePalette.size())
            {
                std::memcpy(
                    gpuModel.boneMatricesStagingMapped[frameIndex],
                    bonePalette.data() + first,
                    static_cast<std::size_t>(count) * sizeof(DirectX::XMFLOAT4X4));

                D3D12_RESOURCE_BARRIER boneBarrier{};
                boneBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                boneBarrier.Transition.pResource = gpuModel.boneMatricesBuffers[frameIndex].Get();
                boneBarrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                boneBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
                boneBarrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
                commandList_->ResourceBarrier(1, &boneBarrier);

                commandList_->CopyBufferRegion(
                    gpuModel.boneMatricesBuffers[frameIndex].Get(), 0,
                    gpuModel.boneMatricesStaging[frameIndex].Get(), 0,
                    static_cast<std::uint64_t>(count) * sizeof(DirectX::XMFLOAT4X4));

                boneBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
                boneBarrier.Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
                commandList_->ResourceBarrier(1, &boneBarrier);
            }
        }

        // Upload this model's morph deltas (zero when no active vertex morphs).
        if (modelIndex + 1 < morphDeltaOffsets.size())
        {
            const std::uint32_t deltaFirst = morphDeltaOffsets[modelIndex];
            const std::uint32_t deltaCount = morphDeltaOffsets[modelIndex + 1] - deltaFirst;
            if (deltaCount > 0 && deltaFirst + deltaCount <= morphDeltas.size())
            {
                std::memcpy(
                    gpuModel.morphDeltaStagingMapped[frameIndex],
                    morphDeltas.data() + deltaFirst,
                    static_cast<std::size_t>(deltaCount) * sizeof(float));

                D3D12_RESOURCE_BARRIER morphBarrier{};
                morphBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                morphBarrier.Transition.pResource = gpuModel.morphDeltaBuffers[frameIndex].Get();
                morphBarrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                morphBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
                morphBarrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
                commandList_->ResourceBarrier(1, &morphBarrier);

                commandList_->CopyBufferRegion(
                    gpuModel.morphDeltaBuffers[frameIndex].Get(), 0,
                    gpuModel.morphDeltaStaging[frameIndex].Get(), 0,
                    static_cast<std::uint64_t>(deltaCount) * sizeof(float));

                morphBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
                morphBarrier.Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
                commandList_->ResourceBarrier(1, &morphBarrier);
            }
        }
    }
}

void Dx12Renderer::RenderSceneView(
    const std::uint32_t frameIndex,
    const std::span<const ModelInstance> instances,
    const std::span<const Model> models,
    const D3D12_GPU_VIRTUAL_ADDRESS cameraCbv,
    const bool skipReflective)
{
    commandList_->SetGraphicsRootSignature(rootSignature_.Get());
    commandList_->SetGraphicsRootConstantBufferView(0, cameraCbv);
    commandList_->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    for (const ModelInstance& instance : instances)
    {
        if (!instance.visible || instance.modelIndex >= models.size())
        {
            continue;
        }
        if (skipReflective && instance.reflective)
        {
            continue; // The reflective surface is drawn in the main pass, not its own reflection.
        }
        if (instance.modelIndex >= residentModels_.size() || residentModels_[instance.modelIndex] == nullptr)
        {
            continue; // Not resident; built on the level switch before this frame.
        }

        const Model& cpuModel = models[instance.modelIndex];
        const GpuModel& gpuModel = *residentModels_[instance.modelIndex];

        // Per-instance world transform, stored column-major for HLSL mul(world, pos).
        DirectX::XMFLOAT4X4 worldStorage;
        DirectX::XMStoreFloat4x4(&worldStorage, InstanceWorldMatrix(instance));
        commandList_->SetGraphicsRoot32BitConstants(3, 16, &worldStorage, 0);

        ID3D12DescriptorHeap* descriptorHeaps[] = { gpuModel.srvHeap.Get() };
        commandList_->SetDescriptorHeaps(1, descriptorHeaps);
        commandList_->SetGraphicsRootDescriptorTable(4, gpuModel.boneMatricesSrv[frameIndex]);
        commandList_->SetGraphicsRootDescriptorTable(5, gpuModel.refBonesSrv);
        commandList_->SetGraphicsRootDescriptorTable(7, gpuModel.morphDeltaSrv[frameIndex]);

        D3D12_VERTEX_BUFFER_VIEW vertexViews[] = { gpuModel.vertexView, gpuModel.skinningView };
        commandList_->IASetVertexBuffers(0, 2, vertexViews);
        commandList_->IASetIndexBuffer(&gpuModel.indexView);

        if (instance.reflective)
        {
            // The reflection texture SRV lives in this model's own SRV heap (already bound above).
            commandList_->SetGraphicsRootDescriptorTable(8, gpuModel.reflectionSrvGpu);
        }

        for (const DrawPacket& packet : cpuModel.mesh.drawPackets)
        {
            const MMDToonMaterial& material = gpuModel.materials[packet.materialIndex];
            ID3D12PipelineState* pipelineState = nullptr;
            if (instance.reflective)
            {
                pipelineState = reflectionPso_.Get();
            }
            else if (skipReflective)
            {
                pipelineState = pipelineStateDoubleSided_.Get(); // Mirrored view flips winding.
            }
            else
            {
                pipelineState = (material.flags & 0x01) != 0 ? pipelineStateDoubleSided_.Get() : pipelineStateCulled_.Get();
            }
            commandList_->SetPipelineState(pipelineState);
            commandList_->SetGraphicsRoot32BitConstants(1, 16, &gpuModel.materialParams[packet.materialIndex], 0);
            commandList_->SetGraphicsRootDescriptorTable(2, gpuModel.materialSrvBundles[packet.materialIndex]);
            commandList_->SetGraphicsRoot32BitConstant(6, packet.refBoneOffset, 0);
            commandList_->DrawIndexedInstanced(packet.indexCount, 1, packet.firstIndex, 0, 0);
        }
    }
}

std::uint64_t Dx12Renderer::Render(
    const std::span<const ModelInstance> instances,
    const std::span<const Model> models,
    const std::span<const DirectX::XMFLOAT4X4> bonePalette,
    const std::span<const std::uint32_t> bonePaletteOffsets,
    const std::span<const float> morphDeltas,
    const std::span<const std::uint32_t> morphDeltaOffsets,
    const Camera& camera,
    ImDrawData* const uiDrawData)
{
    ZoneScopedN("Dx12Renderer::Render");
    const std::uint32_t frameIndex = swapChain_.CurrentBackBufferIndex();

    WaitForPreviousFrame(frameIndex);

    if (FAILED(commandAllocators_[frameIndex]->Reset()))
    {
        throw std::runtime_error("Failed to reset the command allocator.");
    }
    if (FAILED(commandList_->Reset(commandAllocators_[frameIndex].Get(), nullptr)))
    {
        throw std::runtime_error("Failed to reset the command list.");
    }

    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    barrier.Transition.pResource = backBuffers_[frameIndex].Get();
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    commandList_->ResourceBarrier(1, &barrier);

    D3D12_CPU_DESCRIPTOR_HANDLE backBufferRtv = rtvHeap_->GetCPUDescriptorHandleForHeapStart();
    backBufferRtv.ptr += static_cast<SIZE_T>(frameIndex) * rtvDescriptorSize_;
    D3D12_CPU_DESCRIPTOR_HANDLE reflectionRtv = rtvHeap_->GetCPUDescriptorHandleForHeapStart();
    reflectionRtv.ptr += static_cast<SIZE_T>(kFrameCount) * rtvDescriptorSize_;
    const D3D12_CPU_DESCRIPTOR_HANDLE dsvHandle = dsvHeap_->GetCPUDescriptorHandleForHeapStart();
    const float clearColor[4] = { 0.0f, 0.0f, 0.0f, 1.0f };

    D3D12_VIEWPORT viewport{};
    viewport.Width = static_cast<float>(width_);
    viewport.Height = static_cast<float>(height_);
    viewport.MinDepth = 0.0f;
    viewport.MaxDepth = 1.0f;
    commandList_->RSSetViewports(1, &viewport);

    D3D12_RECT scissor{};
    scissor.right = static_cast<LONG>(width_);
    scissor.bottom = static_cast<LONG>(height_);
    commandList_->RSSetScissorRects(1, &scissor);

    if (!instances.empty())
    {
        // Derive the reflection plane from the reflective floor instance: the point is the
        // instance translation and the normal is its local +Y rotated into world space (identity
        // rotation for the floor, so a horizontal plane).
        float planePoint[3] = { 0.0f, 0.0f, 0.0f };
        float planeNormal[3] = { 0.0f, 1.0f, 0.0f };
        bool hasReflective = false;
        for (const ModelInstance& instance : instances)
        {
            if (!instance.reflective || !instance.visible)
            {
                continue;
            }
            planePoint[0] = instance.translation[0];
            planePoint[1] = instance.translation[1];
            planePoint[2] = instance.translation[2];
            const DirectX::XMVECTOR up = DirectX::XMVector3Normalize(DirectX::XMVector3Rotate(
                DirectX::XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f),
                DirectX::XMQuaternionRotationRollPitchYaw(
                    DirectX::XMConvertToRadians(instance.rotation[0]),
                    DirectX::XMConvertToRadians(instance.rotation[1]),
                    DirectX::XMConvertToRadians(instance.rotation[2]))));
            planeNormal[0] = DirectX::XMVectorGetX(up);
            planeNormal[1] = DirectX::XMVectorGetY(up);
            planeNormal[2] = DirectX::XMVectorGetZ(up);
            hasReflective = true;
            break;
        }

        UpdateCameraConstants(frameIndex, camera);
        UploadPerFrameBuffers(frameIndex, instances, models, bonePalette, bonePaletteOffsets, morphDeltas, morphDeltaOffsets);

        if (hasReflective)
        {
            UpdateReflectionConstants(frameIndex, camera, planePoint, planeNormal);

            // Pass 1: render everything but the reflective floor into the offscreen reflection
            // target from the mirrored camera.
            if (reflectionTargetIsSrv_)
            {
                barrier.Transition.pResource = reflectionTarget_.Get();
                barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
                barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
                commandList_->ResourceBarrier(1, &barrier);
                reflectionTargetIsSrv_ = false;
            }
            commandList_->OMSetRenderTargets(1, &reflectionRtv, FALSE, &dsvHandle);
            commandList_->ClearRenderTargetView(reflectionRtv, clearColor, 0, nullptr);
            commandList_->ClearDepthStencilView(dsvHandle, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
            RenderSceneView(frameIndex, instances, models, reflectionConstantBuffers_[frameIndex]->GetGPUVirtualAddress(), /*skipReflective=*/true);

            barrier.Transition.pResource = reflectionTarget_.Get();
            barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
            barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
            commandList_->ResourceBarrier(1, &barrier);
            reflectionTargetIsSrv_ = true;
        }
    }

    // Pass 2: the main scene into the swap-chain back buffer, reflective floor included.
    commandList_->OMSetRenderTargets(1, &backBufferRtv, FALSE, &dsvHandle);
    commandList_->ClearRenderTargetView(backBufferRtv, clearColor, 0, nullptr);
    commandList_->ClearDepthStencilView(dsvHandle, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
    if (!instances.empty())
    {
        RenderSceneView(frameIndex, instances, models, constantBuffers_[frameIndex]->GetGPUVirtualAddress(), /*skipReflective=*/false);
    }

    if (uiDrawData != nullptr)
    {
        ID3D12DescriptorHeap* uiDescriptorHeaps[] = { imguiSrvHeap_.Get() };
        commandList_->SetDescriptorHeaps(1, uiDescriptorHeaps);
        ImGui_ImplDX12_RenderDrawData(uiDrawData, commandList_.Get());
    }

    barrier.Transition.pResource = backBuffers_[frameIndex].Get();
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    commandList_->ResourceBarrier(1, &barrier);

    if (FAILED(commandList_->Close()))
    {
        throw std::runtime_error("Failed to close the command list.");
    }

    ID3D12CommandList* commandLists[] = { commandList_.Get() };
    queue_.Get()->ExecuteCommandLists(1, commandLists);

    swapChain_.Get()->Present(1, 0);

    fenceValues_[frameIndex] = ++currentFence_;
    queue_.Get()->Signal(fences_[frameIndex].Get(), fenceValues_[frameIndex]);

    // Signal the frame fence too, so the caller can gate frame-resource reuse on it.
    queue_.Get()->Signal(frameFence_.Get(), ++frameFenceValue_);
    return frameFenceValue_;
}

bool Dx12Renderer::IsFrameComplete(const std::uint64_t fenceValue) const
{
    return frameFence_->GetCompletedValue() >= fenceValue;
}
} // namespace MmdLab
