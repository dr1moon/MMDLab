#include "Runtime/Core/TestFramework.h"
#include "Runtime/DX12/Dx12CommandAllocator.h"
#include "Runtime/DX12/Dx12CommandQueue.h"
#include "Runtime/DX12/Dx12DescriptorHeap.h"
#include "Runtime/DX12/Dx12Device.h"
#include "Runtime/DX12/Dx12Fence.h"
#include "Runtime/DX12/Dx12GraphicsCommandList.h"
#include "Runtime/DX12/RenderDocCapture.h"

#include <windows.h>
#include <wrl/client.h>

#include <cstdio>
#include <stdexcept>

namespace
{
Microsoft::WRL::ComPtr<ID3D12Resource> CreateRenderTarget(ID3D12Device* device, const UINT width, const UINT height)
{
    D3D12_HEAP_PROPERTIES heapProperties{};
    heapProperties.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC description{};
    description.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    description.Width = width;
    description.Height = height;
    description.DepthOrArraySize = 1;
    description.MipLevels = 1;
    description.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    description.SampleDesc.Count = 1;
    description.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    description.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

    Microsoft::WRL::ComPtr<ID3D12Resource> resource;
    if (FAILED(device->CreateCommittedResource(
        &heapProperties,
        D3D12_HEAP_FLAG_NONE,
        &description,
        D3D12_RESOURCE_STATE_RENDER_TARGET,
        nullptr,
        IID_PPV_ARGS(&resource))))
    {
        throw std::runtime_error("Failed to create the render target.");
    }
    return resource;
}
} // namespace

MMDLAB_TEST(DX12.Dx12RenderDocCapture, CapturesAClear)
{
    // Load RenderDoc before creating the device so it hooks device creation.
    MmdLab::RenderDocCapture capture;

    if (!capture.IsAvailable())
    {
        std::printf("RenderDoc not available; skipping capture.\n");
        return;
    }

    capture.SetCapturePath("captures/clear");

    MmdLab::Dx12Device device;
    MmdLab::Dx12CommandQueue queue(device.Get());
    MmdLab::Dx12Fence fence(device.Get());
    MmdLab::Dx12CommandAllocator allocator(device.Get());
    MmdLab::Dx12GraphicsCommandList commandList(device.Get(), allocator.Get());
    MmdLab::Dx12DescriptorHeap rtvHeap(device.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1);

    const auto renderTarget = CreateRenderTarget(device.Get(), 256, 256);
    device.Get()->CreateRenderTargetView(renderTarget.Get(), nullptr, rtvHeap.CpuHandle(0));

    capture.StartCapture();

    commandList.Get()->Reset(allocator.Get(), nullptr);
    const float clearColor[4] = { 0.25f, 0.5f, 0.75f, 1.0f };
    commandList.Get()->ClearRenderTargetView(rtvHeap.CpuHandle(0), clearColor, 0, nullptr);
    commandList.Get()->Close();

    ID3D12CommandList* lists[] = { commandList.Get() };
    queue.Get()->ExecuteCommandLists(1, lists);
    queue.Get()->Signal(fence.Get(), 1);

    const HANDLE event = CreateEventW(nullptr, false, false, nullptr);
    if (fence.CompletedValue() < 1)
    {
        fence.Get()->SetEventOnCompletion(1, event);
        (void)WaitForSingleObject(event, 5000);
    }
    CloseHandle(event);

    MMDLAB_CHECK(capture.EndCapture());
}
