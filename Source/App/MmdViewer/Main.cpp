#include "App/MmdViewer/WindowsApplication.h"
#include "Runtime/Core/Channel.h"
#include "Runtime/Core/FrameResource.h"
#include "Runtime/Core/FrameResourcePool.h"
#include "Runtime/Core/Thread.h"
#include "Runtime/DX12/RhiThread.h"
#include "Runtime/Render/RenderThread.h"

#include <windows.h>

#include <cstdint>
#include <exception>
#include <iostream>

int wmain()
{
    try
    {
        MmdLab::WindowsApplication application;
        application.Initialize(GetModuleHandleW(nullptr), SW_SHOWDEFAULT);

        RECT clientRect{};
        GetClientRect(application.GetWindowHandle(), &clientRect);
        const std::uint32_t width = static_cast<std::uint32_t>(clientRect.right);
        const std::uint32_t height = static_cast<std::uint32_t>(clientRect.bottom);

        // The three-thread pipeline: GameThread (this thread) -> RenderThread -> RhiThread.
        MmdLab::FrameResourcePool pool;
        MmdLab::Channel<MmdLab::FrameIndex, MmdLab::FrameResourcePool::kFrameCount> gameToRender;
        MmdLab::Channel<MmdLab::FrameIndex, MmdLab::FrameResourcePool::kFrameCount> renderToRhi;

        MmdLab::RenderThread renderStage(gameToRender, renderToRhi, pool);
        MmdLab::RhiThread rhiStage(renderToRhi, pool, application.GetWindowHandle(), width, height);

        MmdLab::Thread renderThread(renderStage, L"RenderThread");
        MmdLab::Thread rhiThread(rhiStage, L"RhiThread");

        // GameThread role: produce one frame per iteration, throttled by the frame pool.
        MmdLab::FrameId frameId = 0;
        while (application.ProcessMessages())
        {
            const MmdLab::FrameIndex index = pool.Acquire();
            pool.Get(index).frameId = ++frameId;
            gameToRender.Push(index);
        }

        // Shut down upstream-first so the pipeline drains in order.
        renderThread.RequestStop();
        rhiThread.RequestStop();

        return EXIT_SUCCESS;
    }
    catch (const std::exception& exception)
    {
        std::cerr << "MMDLab Viewer failed: " << exception.what() << '\n';
        return EXIT_FAILURE;
    }
}
