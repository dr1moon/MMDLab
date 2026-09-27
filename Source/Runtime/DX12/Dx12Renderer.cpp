#include "Runtime/DX12/Dx12Renderer.h"

#include "Runtime/Asset/Model.h"
#include "Runtime/DX12/ShaderCompiler.h"
#include "Runtime/DX12/Shaders.h"
#include "Runtime/Scene/Camera.h"
#include "Runtime/Scene/WorldData.h"
#include "Runtime/Core/Log.h"

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

// Builds the world matrix for an instance: YXZ Euler rotation then translation, expressed with
// row-vector DirectXMath multiplication and stored column-major so HLSL mul(world, pos) reads it
// directly. Identity for the default zero transform.
DirectX::XMMATRIX ComputeWorldMatrix(const MmdLab::ModelInstance& instance)
{
    using namespace DirectX;
    const XMMATRIX rotation = XMMatrixRotationRollPitchYaw(
        XMConvertToRadians(instance.rotation[0]), // pitch (X).
        XMConvertToRadians(instance.rotation[1]), // yaw (Y).
        XMConvertToRadians(instance.rotation[2])); // roll (Z).
    const XMMATRIX translation = XMMatrixTranslation(
        instance.translation[0], instance.translation[1], instance.translation[2]);
    return XMMatrixMultiply(translation, rotation);
}

// Interleaved line-list vertex for the skeleton overlay: a 3D position and a flat color.
struct LineVertex
{
    float position[4];
    float color[4];
};
static_assert(sizeof(LineVertex) == 32);

// A per-vertex flat color, uploaded as a second vertex stream for the skinning-color view.
struct ColorVertex
{
    float color[4];
};
static_assert(sizeof(ColorVertex) == 16);

// Two line vertices per bone (head -> tail), colored by the shared per-bone debug palette so the
// skeleton overlay matches the skinning-color view.
std::vector<LineVertex> BuildSkeletonLines(const MmdLab::Skeleton& skeleton)
{
    std::vector<LineVertex> lines;
    lines.reserve(skeleton.bones.size() * 2);
    for (std::size_t i = 0; i < skeleton.bones.size(); ++i)
    {
        const MmdLab::Bone& bone = skeleton.bones[i];
        float rgb[3];
        MmdLab::BoneDebugColor(i, rgb);

        LineVertex vertex{};
        vertex.position[3] = 1.0f;
        vertex.color[0] = rgb[0];
        vertex.color[1] = rgb[1];
        vertex.color[2] = rgb[2];
        vertex.color[3] = 1.0f;

        vertex.position[0] = bone.position[0];
        vertex.position[1] = bone.position[1];
        vertex.position[2] = bone.position[2];
        lines.push_back(vertex);

        vertex.position[0] = bone.tail[0];
        vertex.position[1] = bone.tail[1];
        vertex.position[2] = bone.tail[2];
        lines.push_back(vertex);
    }
    return lines;
}

// One color per vertex: its dominant bone's palette color (gray when unskinned).
std::vector<ColorVertex> BuildSkinningColors(const std::vector<MmdLab::SkinningVertex>& skinning)
{
    std::vector<ColorVertex> colors;
    colors.reserve(skinning.size());
    for (const MmdLab::SkinningVertex& skin : skinning)
    {
        ColorVertex vertex{};
        const std::int32_t dominant = MmdLab::DominantBoneIndex(skin);
        if (dominant >= 0)
        {
            float rgb[3];
            MmdLab::BoneDebugColor(static_cast<std::size_t>(dominant), rgb);
            vertex.color[0] = rgb[0];
            vertex.color[1] = rgb[1];
            vertex.color[2] = rgb[2];
        }
        else
        {
            vertex.color[0] = 0.6f;
            vertex.color[1] = 0.6f;
            vertex.color[2] = 0.6f;
        }
        vertex.color[3] = 1.0f;
        colors.push_back(vertex);
    }
    return colors;
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
        if (residentModels_.find(instance.modelIndex) == residentModels_.end())
        {
            BuildGpuModel(instance.modelIndex, models[instance.modelIndex]);
        }
    }
}

void Dx12Renderer::BuildGpuModel(const std::size_t modelIndex, const Model& model)
{
    GpuModel gpuModel;
    gpuModel.materials = model.mesh.materials;

    // Project each runtime material into the pixel shader's per-material constant layout.
    gpuModel.materialParams.reserve(gpuModel.materials.size());
    for (const Material& material : gpuModel.materials)
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

    residentModels_.emplace(modelIndex, std::move(gpuModel));
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

    description.InputLayout.NumElements = 3;
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

    // Flat-color debug pipelines: the skeleton line overlay and the skinning-color view share
    // one vertex/pixel shader pair but differ in input layout, topology, and depth writes.
    const auto flatVertex = ShaderCompiler::Compile(FlatColorVertexShaderSource, "VSMain", "vs_5_1");
    const auto flatPixel = ShaderCompiler::Compile(FlatColorPixelShaderSource, "PSMain", "ps_5_1");
    const D3D12_SHADER_BYTECODE flatVsBytecode = { flatVertex->GetBufferPointer(), flatVertex->GetBufferSize() };
    const D3D12_SHADER_BYTECODE flatPsBytecode = { flatPixel->GetBufferPointer(), flatPixel->GetBufferSize() };

    const auto createFlatPipeline = [&](const D3D12_INPUT_ELEMENT_DESC* layout, const UINT elementCount,
                                        const D3D12_PRIMITIVE_TOPOLOGY_TYPE topology,
                                        const D3D12_DEPTH_WRITE_MASK depthWriteMask)
    {
        D3D12_GRAPHICS_PIPELINE_STATE_DESC description{};
        description.pRootSignature = rootSignature_.Get();
        description.VS = flatVsBytecode;
        description.PS = flatPsBytecode;
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
        description.DepthStencilState.DepthWriteMask = depthWriteMask;
        description.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
        description.DepthStencilState.StencilEnable = FALSE;
        description.DSVFormat = DXGI_FORMAT_D32_FLOAT;
        description.InputLayout.NumElements = elementCount;
        description.InputLayout.pInputElementDescs = layout;
        description.PrimitiveTopologyType = topology;
        description.NumRenderTargets = 1;
        description.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
        description.SampleDesc.Count = 1;

        Microsoft::WRL::ComPtr<ID3D12PipelineState> pipelineState;
        const HRESULT result = device_.Get()->CreateGraphicsPipelineState(&description, IID_PPV_ARGS(&pipelineState));
        if (FAILED(result))
        {
            DumpD3d12Messages(device_.Get());
            char message[128];
            std::snprintf(message, sizeof(message), "Failed to create a flat-color pipeline state (HRESULT 0x%08X).", static_cast<unsigned int>(result));
            throw std::runtime_error(message);
        }
        return pipelineState;
    };

    // Skeleton lines: position and color interleaved in one buffer, drawn as a line list with
    // depth reads on and depth writes off so the overlay is occluded but never occludes.
    {
        const D3D12_INPUT_ELEMENT_DESC layout[] = {
            { "POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
            { "COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 16, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        };
        pipelineStateLines_ = createFlatPipeline(layout, 2, D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE, D3D12_DEPTH_WRITE_MASK_ZERO);
    }

    // Skinning colors: position from the mesh vertex buffer (slot 0) plus a flat color from a
    // second per-vertex stream (slot 1), triangle-list, depth writes on like the toon mesh.
    {
        const D3D12_INPUT_ELEMENT_DESC layout[] = {
            { "POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
            { "COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        };
        pipelineStateSkinning_ = createFlatPipeline(layout, 2, D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE, D3D12_DEPTH_WRITE_MASK_ALL);
    }
}

void Dx12Renderer::CreateRenderTargetViews()
{
    D3D12_DESCRIPTOR_HEAP_DESC description{};
    description.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    description.NumDescriptors = kFrameCount;
    description.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    description.NodeMask = 0;

    if (FAILED(device_.Get()->CreateDescriptorHeap(&description, IID_PPV_ARGS(&rtvHeap_))))
    {
        throw std::runtime_error("Failed to create the render-target-view descriptor heap.");
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

    D3D12_DESCRIPTOR_HEAP_DESC heapDescription{};
    heapDescription.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    heapDescription.NumDescriptors = 1;
    heapDescription.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    heapDescription.NodeMask = 0;

    if (FAILED(device_.Get()->CreateDescriptorHeap(&heapDescription, IID_PPV_ARGS(&dsvHeap_))))
    {
        throw std::runtime_error("Failed to create the depth-stencil-view descriptor heap.");
    }

    device_.Get()->CreateDepthStencilView(
        depthBuffer_.Get(),
        nullptr,
        dsvHeap_->GetCPUDescriptorHandleForHeapStart());
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
    model.totalIndexCount = static_cast<std::uint32_t>(mesh.indices.size());

    // Skeleton line overlay: two interleaved position+color vertices per bone.
    const std::vector<LineVertex> skeletonLines = BuildSkeletonLines(cpuModel.skeleton);
    if (!skeletonLines.empty())
    {
        const std::size_t skeletonSize = skeletonLines.size() * sizeof(LineVertex);
        model.skeletonVertexBuffer = upload(skeletonLines.data(), skeletonSize, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
        model.skeletonVertexView.BufferLocation = model.skeletonVertexBuffer->GetGPUVirtualAddress();
        model.skeletonVertexView.StrideInBytes = sizeof(LineVertex);
        model.skeletonVertexView.SizeInBytes = static_cast<UINT>(skeletonSize);
        model.skeletonVertexCount = static_cast<std::uint32_t>(skeletonLines.size());
    }

    // Skinning-color second stream: one flat color per mesh vertex.
    const std::vector<ColorVertex> skinningColors = BuildSkinningColors(cpuModel.skinning);
    const std::size_t skinningSize = skinningColors.size() * sizeof(ColorVertex);
    model.skinningColorBuffer = upload(skinningColors.data(), skinningSize, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
    model.skinningColorView.BufferLocation = model.skinningColorBuffer->GetGPUVirtualAddress();
    model.skinningColorView.StrideInBytes = sizeof(ColorVertex);
    model.skinningColorView.SizeInBytes = static_cast<UINT>(skinningSize);

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

void Dx12Renderer::CreateTextures(GpuModel& model, const std::span<const Image> images)
{
    // One three-descriptor (base/toon/sphere) SRV bundle per material.
    D3D12_DESCRIPTOR_HEAP_DESC heapDescription{};
    heapDescription.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heapDescription.NumDescriptors = static_cast<UINT>(model.materials.size()) * 3;
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
        description.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
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
        const std::size_t sourceRowBytes = static_cast<std::size_t>(image.width) * 4;
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
    for (const Image& image : images)
    {
        model.textures.push_back(uploadTexture(image));
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

    const auto createSrv = [&](ID3D12Resource* texture)
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
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
    for (const Material& material : model.materials)
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
        createSrv(base);
        createSrv(toon);
        createSrv(sphere);
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
}

void Dx12Renderer::UpdateCameraConstants(const std::uint32_t frameIndex, const Camera& camera)
{
    using namespace DirectX;

    const XMMATRIX rotation = XMMatrixRotationRollPitchYaw(
        XMConvertToRadians(camera.rotation[0]),
        XMConvertToRadians(camera.rotation[1]),
        XMConvertToRadians(camera.rotation[2]));
    const XMMATRIX translation = XMMatrixTranslation(
        camera.position[0], camera.position[1], camera.position[2]);
    // Row-vector: apply rotation then translation. Inverting gives the view matrix.
    const XMMATRIX view = XMMatrixInverse(nullptr, XMMatrixMultiply(rotation, translation));

    const float aspect = static_cast<float>(width_) / static_cast<float>(height_);
    const XMMATRIX projection = XMMatrixPerspectiveFovLH(
        XMConvertToRadians(camera.fovDegrees), aspect, camera.nearPlane, camera.farPlane);

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

std::uint64_t Dx12Renderer::Render(
    const std::span<const ModelInstance> instances,
    const std::span<const Model> models,
    const Camera& camera,
    ImDrawData* const uiDrawData,
    const DebugViewOptions options)
{
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

    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = rtvHeap_->GetCPUDescriptorHandleForHeapStart();
    rtvHandle.ptr += static_cast<SIZE_T>(frameIndex) * rtvDescriptorSize_;
    const D3D12_CPU_DESCRIPTOR_HANDLE dsvHandle = dsvHeap_->GetCPUDescriptorHandleForHeapStart();
    commandList_->OMSetRenderTargets(1, &rtvHandle, FALSE, &dsvHandle);

    const float clearColor[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
    commandList_->ClearRenderTargetView(rtvHandle, clearColor, 0, nullptr);
    commandList_->ClearDepthStencilView(dsvHandle, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);

    if (!instances.empty())
    {
        UpdateCameraConstants(frameIndex, camera);

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

        commandList_->SetGraphicsRootSignature(rootSignature_.Get());
        commandList_->SetGraphicsRootConstantBufferView(0, constantBuffers_[frameIndex]->GetGPUVirtualAddress());
        commandList_->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

        for (const ModelInstance& instance : instances)
        {
            if (!instance.visible || instance.modelIndex >= models.size())
            {
                continue;
            }
            const auto resident = residentModels_.find(instance.modelIndex);
            if (resident == residentModels_.end())
            {
                continue; // Not resident; built on the level switch before this frame.
            }

            const Model& cpuModel = models[instance.modelIndex];
            const GpuModel& gpuModel = resident->second;

            // Per-instance world transform, stored column-major for HLSL mul(world, pos).
            DirectX::XMFLOAT4X4 worldStorage;
            DirectX::XMStoreFloat4x4(&worldStorage, ComputeWorldMatrix(instance));
            commandList_->SetGraphicsRoot32BitConstants(3, 16, &worldStorage, 0);

            if (options.showSkinningColors)
            {
                // Skinning-color view: shade the whole mesh by each vertex's dominant bone,
                // ignoring materials, so position comes from the mesh buffer and color from a
                // second per-vertex stream.
                ID3D12DescriptorHeap* descriptorHeaps[] = { gpuModel.srvHeap.Get() };
                commandList_->SetDescriptorHeaps(1, descriptorHeaps);
                D3D12_VERTEX_BUFFER_VIEW vertexViews[] = { gpuModel.vertexView, gpuModel.skinningColorView };
                commandList_->IASetVertexBuffers(0, 2, vertexViews);
                commandList_->IASetIndexBuffer(&gpuModel.indexView);
                commandList_->SetPipelineState(pipelineStateSkinning_.Get());
                commandList_->DrawIndexedInstanced(gpuModel.totalIndexCount, 1, 0, 0, 0);
            }
            else
            {
                ID3D12DescriptorHeap* descriptorHeaps[] = { gpuModel.srvHeap.Get() };
                commandList_->SetDescriptorHeaps(1, descriptorHeaps);
                commandList_->IASetVertexBuffers(0, 1, &gpuModel.vertexView);
                commandList_->IASetIndexBuffer(&gpuModel.indexView);

                for (const DrawPacket& packet : cpuModel.mesh.drawPackets)
                {
                    const Material& material = gpuModel.materials[packet.materialIndex];
                    commandList_->SetPipelineState(
                        (material.flags & 0x01) != 0 ? pipelineStateDoubleSided_.Get() : pipelineStateCulled_.Get());
                    commandList_->SetGraphicsRoot32BitConstants(1, 16, &gpuModel.materialParams[packet.materialIndex], 0);
                    commandList_->SetGraphicsRootDescriptorTable(2, gpuModel.materialSrvBundles[packet.materialIndex]);
                    commandList_->DrawIndexedInstanced(packet.indexCount, 1, packet.firstIndex, 0, 0);
                }
            }

            if (options.showSkeleton && gpuModel.skeletonVertexCount > 0)
            {
                commandList_->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_LINELIST);
                commandList_->IASetVertexBuffers(0, 1, &gpuModel.skeletonVertexView);
                commandList_->SetPipelineState(pipelineStateLines_.Get());
                commandList_->DrawInstanced(gpuModel.skeletonVertexCount, 1, 0, 0);
                commandList_->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            }
        }
    }

    if (uiDrawData != nullptr)
    {
        ID3D12DescriptorHeap* uiDescriptorHeaps[] = { imguiSrvHeap_.Get() };
        commandList_->SetDescriptorHeaps(1, uiDescriptorHeaps);
        ImGui_ImplDX12_RenderDrawData(uiDrawData, commandList_.Get());
    }

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
