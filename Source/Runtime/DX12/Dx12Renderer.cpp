#include "Runtime/DX12/Dx12Renderer.h"

#include "Runtime/DX12/ShaderCompiler.h"
#include "Runtime/DX12/Shaders.h"

#include <windows.h>

#include <stdexcept>

namespace MmdLab
{
Dx12Renderer::Dx12Renderer(const HWND window, const std::uint32_t width, const std::uint32_t height)
    : device_()
    , queue_(device_.Get())
    , swapChain_(device_.GetFactory(), queue_.Get(), window, width, height)
    , rootSignature_(device_.Get())
    , width_(width)
    , height_(height)
{
    rtvDescriptorSize_ = device_.Get()->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    CreatePipelineState();
    CreateRenderTargetViews();

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

    if (FAILED(device_.Get()->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, commandAllocators_[0].Get(), nullptr, IID_PPV_ARGS(&commandList_))))
    {
        throw std::runtime_error("Failed to create the command list.");
    }

    // The list is created in the recording state; close it so the allocator can be reset
    // on the first frame (an allocator cannot reset while a recording list references it).
    if (FAILED(commandList_->Close()))
    {
        throw std::runtime_error("Failed to close the command list.");
    }
}

void Dx12Renderer::CreatePipelineState()
{
    const auto vertexShader = ShaderCompiler::Compile(TriangleVertexShaderSource, "VSMain", "vs_5_1");
    const auto pixelShader = ShaderCompiler::Compile(TrianglePixelShaderSource, "PSMain", "ps_5_1");

    const D3D12_SHADER_BYTECODE vertexBytecode = { vertexShader->GetBufferPointer(), vertexShader->GetBufferSize() };
    const D3D12_SHADER_BYTECODE pixelBytecode = { pixelShader->GetBufferPointer(), pixelShader->GetBufferSize() };

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
    description.DepthStencilState.DepthEnable = FALSE;
    description.DepthStencilState.StencilEnable = FALSE;
    description.InputLayout.NumElements = 0;
    description.InputLayout.pInputElementDescs = nullptr;
    description.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    description.NumRenderTargets = 1;
    description.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    description.SampleDesc.Count = 1;

    if (FAILED(device_.Get()->CreateGraphicsPipelineState(&description, IID_PPV_ARGS(&pipelineState_))))
    {
        throw std::runtime_error("Failed to create the graphics pipeline state.");
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

void Dx12Renderer::Render()
{
    const std::uint32_t frameIndex = swapChain_.CurrentBackBufferIndex();

    WaitForPreviousFrame(frameIndex);

    if (FAILED(commandAllocators_[frameIndex]->Reset()))
    {
        throw std::runtime_error("Failed to reset the command allocator.");
    }
    if (FAILED(commandList_->Reset(commandAllocators_[frameIndex].Get(), pipelineState_.Get())))
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
    commandList_->OMSetRenderTargets(1, &rtvHandle, FALSE, nullptr);

    const float clearColor[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
    commandList_->ClearRenderTargetView(rtvHandle, clearColor, 0, nullptr);

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
    commandList_->SetPipelineState(pipelineState_.Get());
    commandList_->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    commandList_->DrawInstanced(3, 1, 0, 0);

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
}
} // namespace MmdLab
