#include "Runtime/DX12/Dx12Renderer.h"

#include "Runtime/DX12/ShaderCompiler.h"
#include "Runtime/DX12/Shaders.h"

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <iostream>
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
        std::cerr << "(ID3D12InfoQueue unavailable)\n";
        return;
    }
    const std::uint64_t count = infoQueue->GetNumStoredMessages();
    std::cerr << "(InfoQueue has " << count << " messages)\n";
    for (std::uint64_t i = 0; i < count; ++i)
    {
        std::size_t length = 0;
        infoQueue->GetMessage(i, nullptr, &length);
        std::vector<std::uint8_t> buffer(length);
        auto* message = reinterpret_cast<D3D12_MESSAGE*>(buffer.data());
        infoQueue->GetMessage(i, message, &length);
        std::cerr << "D3D12: " << message->pDescription << '\n';
    }
}
} // namespace

namespace MmdLab
{
Dx12Renderer::Dx12Renderer(
    const HWND window,
    const std::uint32_t width,
    const std::uint32_t height,
    const MeshAsset& mesh,
    const std::span<const Image> textures)
    : device_()
    , queue_(device_.Get())
    , swapChain_(device_.GetFactory(), queue_.Get(), window, width, height)
    , rootSignature_(device_.Get())
    , materials_(mesh.materials)
    , width_(width)
    , height_(height)
{
    rtvDescriptorSize_ = device_.Get()->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    // Project each runtime material into the pixel shader's per-material constant layout.
    materialParams_.reserve(materials_.size());
    for (const Material& material : materials_)
    {
        MaterialShaderParams params{};
        for (int c = 0; c < 4; ++c) { params.baseColor[c] = material.baseColor[c]; }
        for (int c = 0; c < 3; ++c) { params.ambient[c] = material.ambientColor[c]; }
        for (int c = 0; c < 3; ++c) { params.specular[c] = material.specularColor[c]; }
        params.shininess = material.specularStrength;
        params.sphereMode = static_cast<float>(material.sphereMode);
        materialParams_.push_back(params);
    }

    CreatePipelineState();
    CreateRenderTargetViews();
    CreateDepthBuffer();

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

    CreateMeshBuffers(mesh);
    CreateTextures(textures);
    CreateConstantBuffer(mesh);
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

void Dx12Renderer::CreateMeshBuffers(const MeshAsset& mesh)
{
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
    vertexBuffer_ = upload(mesh.vertices.data(), vertexSize, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
    vertexBufferView_.BufferLocation = vertexBuffer_->GetGPUVirtualAddress();
    vertexBufferView_.StrideInBytes = sizeof(MmdlVertex);
    vertexBufferView_.SizeInBytes = static_cast<UINT>(vertexSize);

    const std::size_t indexSize = mesh.indices.size() * sizeof(std::uint32_t);
    indexBuffer_ = upload(mesh.indices.data(), indexSize, D3D12_RESOURCE_STATE_INDEX_BUFFER);
    indexBufferView_.BufferLocation = indexBuffer_->GetGPUVirtualAddress();
    indexBufferView_.Format = DXGI_FORMAT_R32_UINT;
    indexBufferView_.SizeInBytes = static_cast<UINT>(indexSize);

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

void Dx12Renderer::CreateTextures(const std::span<const Image> images)
{
    srvDescriptorSize_ = device_.Get()->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    // One three-descriptor (base/toon/sphere) SRV bundle per material.
    D3D12_DESCRIPTOR_HEAP_DESC heapDescription{};
    heapDescription.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heapDescription.NumDescriptors = static_cast<UINT>(materials_.size()) * 3;
    heapDescription.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    heapDescription.NodeMask = 0;

    if (FAILED(device_.Get()->CreateDescriptorHeap(&heapDescription, IID_PPV_ARGS(&srvHeap_))))
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

    textures_.reserve(images.size());
    for (const Image& image : images)
    {
        textures_.push_back(uploadTexture(image));
    }

    const std::uint8_t whitePixel[4] = { 255, 255, 255, 255 };
    Image whiteImage;
    whiteImage.width = 1;
    whiteImage.height = 1;
    whiteImage.pixels.assign(whitePixel, whitePixel + 4);
    whiteTexture_ = uploadTexture(whiteImage);

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
    toonRampTexture_ = uploadTexture(rampImage);

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
    D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle = srvHeap_->GetCPUDescriptorHandleForHeapStart();
    D3D12_GPU_DESCRIPTOR_HANDLE gpuHandle = srvHeap_->GetGPUDescriptorHandleForHeapStart();

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
    materialSrvBundles_.reserve(materials_.size());
    for (const Material& material : materials_)
    {
        ID3D12Resource* base = material.baseColorTexture >= 0
            ? textures_[static_cast<std::size_t>(material.baseColorTexture)].Get()
            : whiteTexture_.Get();
        ID3D12Resource* toon = material.toonTexture >= 0
            ? textures_[static_cast<std::size_t>(material.toonTexture)].Get()
            : toonRampTexture_.Get();
        ID3D12Resource* sphere = material.sphereTexture >= 0
            ? textures_[static_cast<std::size_t>(material.sphereTexture)].Get()
            : whiteTexture_.Get();

        const D3D12_GPU_DESCRIPTOR_HANDLE bundleStart = gpuHandle;
        createSrv(base);
        createSrv(toon);
        createSrv(sphere);
        materialSrvBundles_.push_back(bundleStart);
    }
}

void Dx12Renderer::CreateConstantBuffer(const MeshAsset& mesh)
{
    float boundsMin[3] = { 3.4e38f, 3.4e38f, 3.4e38f };
    float boundsMax[3] = { -3.4e38f, -3.4e38f, -3.4e38f };
    for (const MmdlVertex& vertex : mesh.vertices)
    {
        for (int axis = 0; axis < 3; ++axis)
        {
            boundsMin[axis] = std::min(boundsMin[axis], vertex.position[axis]);
            boundsMax[axis] = std::max(boundsMax[axis], vertex.position[axis]);
        }
    }

    float center[3];
    float extent = 0.0f;
    for (int axis = 0; axis < 3; ++axis)
    {
        center[axis] = (boundsMin[axis] + boundsMax[axis]) * 0.5f;
        extent = std::max(extent, boundsMax[axis] - boundsMin[axis]);
    }
    const float scale = 1.5f / extent;
    const float zScale = 1.0f / (boundsMax[2] - boundsMin[2]);

    // An orthographic "fit to clip space" view-projection, column-major. X/Y use a uniform
    // scale so proportions are preserved; Z is remapped to [0, 1] so nothing is clipped.
    float viewProjection[16] = {};
    viewProjection[0] = scale;
    viewProjection[5] = scale;
    viewProjection[10] = zScale;
    viewProjection[12] = -center[0] * scale;
    viewProjection[13] = -center[1] * scale;
    viewProjection[14] = -boundsMin[2] * zScale;
    viewProjection[15] = 1.0f;

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

    if (FAILED(device_.Get()->CreateCommittedResource(
        &uploadHeap,
        D3D12_HEAP_FLAG_NONE,
        &description,
        D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr,
        IID_PPV_ARGS(&constantBuffer_))))
    {
        throw std::runtime_error("Failed to create the camera constant buffer.");
    }

    // Camera constants: viewProjection (16 floats) + light direction + camera direction
    // (24 floats = 96 bytes), matching cbuffer CameraConstants in Shaders.h.
    float cameraConstants[24] = {};
    std::memcpy(cameraConstants, viewProjection, sizeof(viewProjection));
    cameraConstants[16] = -0.3f; // lightDirection.xyz (provisional diagonal key light).
    cameraConstants[17] = -0.8f;
    cameraConstants[18] = -0.6f;
    cameraConstants[20] = 0.0f; // cameraDirection.xyz (orthographic view forward, -Z).
    cameraConstants[21] = 0.0f;
    cameraConstants[22] = -1.0f;

    void* mapped = nullptr;
    constantBuffer_->Map(0, nullptr, &mapped);
    std::memcpy(mapped, cameraConstants, sizeof(cameraConstants));
    constantBuffer_->Unmap(0, nullptr);

    constantBufferAddress_ = constantBuffer_->GetGPUVirtualAddress();
}

void Dx12Renderer::WaitForPreviousFrame(const std::uint32_t frameIndex)
{
    if (fences_[frameIndex]->GetCompletedValue() < fenceValues_[frameIndex])
    {
        const HANDLE event = CreateEventW(nullptr, false, false, nullptr);
        fences_[frameIndex]->SetEventOnCompletion(fenceValues_[frameIndex], event);
        WaitForSingleObject(event, INFINITE);
        CloseHandle(event);
    }
}

std::uint64_t Dx12Renderer::Render(const std::span<const DrawPacket> drawPackets)
{
    const std::uint32_t frameIndex = swapChain_.CurrentBackBufferIndex();

    WaitForPreviousFrame(frameIndex);

    if (FAILED(commandAllocators_[frameIndex]->Reset()))
    {
        throw std::runtime_error("Failed to reset the command allocator.");
    }
    if (FAILED(commandList_->Reset(commandAllocators_[frameIndex].Get(), pipelineStateCulled_.Get())))
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
    commandList_->SetGraphicsRootConstantBufferView(0, constantBufferAddress_);
    ID3D12DescriptorHeap* descriptorHeaps[] = { srvHeap_.Get() };
    commandList_->SetDescriptorHeaps(1, descriptorHeaps);
    commandList_->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    commandList_->IASetVertexBuffers(0, 1, &vertexBufferView_);
    commandList_->IASetIndexBuffer(&indexBufferView_);

    for (const DrawPacket& packet : drawPackets)
    {
        const Material& material = materials_[packet.materialIndex];
        commandList_->SetPipelineState(
            (material.flags & 0x01) != 0 ? pipelineStateDoubleSided_.Get() : pipelineStateCulled_.Get());
        commandList_->SetGraphicsRoot32BitConstants(1, 16, &materialParams_[packet.materialIndex], 0);
        commandList_->SetGraphicsRootDescriptorTable(2, materialSrvBundles_[packet.materialIndex]);
        commandList_->DrawIndexedInstanced(packet.indexCount, 1, packet.firstIndex, 0, 0);
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
