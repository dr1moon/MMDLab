#include "Runtime/Core/TestFramework.h"
#include "Runtime/DX12/Dx12CommandQueue.h"
#include "Runtime/DX12/Dx12Device.h"
#include "Runtime/DX12/Dx12Fence.h"

#include <windows.h>

MMDLAB_TEST(DX12.Dx12CommandQueue, SubmitsASignalAndCompletes)
{
    MmdLab::Dx12Device device;
    MmdLab::Dx12CommandQueue queue(device.Get());
    MmdLab::Dx12Fence fence(device.Get());

    MMDLAB_CHECK(queue.Get() != nullptr);
    MMDLAB_CHECK(fence.Get() != nullptr);

    const HANDLE event = CreateEventW(nullptr, false, false, nullptr);
    MMDLAB_CHECK(event != nullptr);

    // Signal the fence to value 1 on the GPU queue, then wait for the GPU to reach it.
    queue.Get()->Signal(fence.Get(), 1);

    if (fence.CompletedValue() < 1)
    {
        fence.Get()->SetEventOnCompletion(1, event);
        const DWORD waitResult = WaitForSingleObject(event, 5000);
        MMDLAB_CHECK(waitResult == WAIT_OBJECT_0);
    }

    CloseHandle(event);

    MMDLAB_CHECK(fence.CompletedValue() >= 1);
}
