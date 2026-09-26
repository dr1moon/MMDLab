#include "App/MmdViewer/Scene.h"
#include "App/MmdViewer/WindowsApplication.h"
#include "Runtime/Core/Channel.h"
#include "Runtime/Core/FrameResource.h"
#include "Runtime/Core/FrameResourcePool.h"
#include "Runtime/Core/Thread.h"
#include "Runtime/Core/Ui.h"
#include "Runtime/DX12/RhiThread.h"
#include "Runtime/Render/RenderThread.h"

#include <windows.h>

#include <cstdint>
#include <exception>
#include <filesystem>
#include <iostream>
#include <string>

namespace
{
// The directory holding the running executable.
std::filesystem::path ExecutableDirectory()
{
    wchar_t buffer[MAX_PATH];
    const DWORD length = GetModuleFileNameW(nullptr, buffer, MAX_PATH);
    if (length == 0 || length >= MAX_PATH)
    {
        return std::filesystem::current_path();
    }
    return std::filesystem::path(std::wstring(buffer, length)).parent_path();
}

// Walks up from the executable directory looking for the repository's Project folder, so the
// editor finds Project/Models without a command-line argument when run from its build output.
// Returns an empty path when no Project folder is found.
std::filesystem::path FindProjectDirectory()
{
    std::filesystem::path current = ExecutableDirectory();
    for (int level = 0; level < 6 && !current.empty(); ++level)
    {
        const std::filesystem::path candidate = current / L"Project";
        if (std::filesystem::is_directory(candidate))
        {
            return candidate;
        }
        const std::filesystem::path parent = current.parent_path();
        if (parent == current)
        {
            break;
        }
        current = parent;
    }
    return {};
}
} // namespace

int wmain(const int argc, wchar_t* argv[])
{
    try
    {
        // The editor loads every .pmx in the scan directory at startup; the user switches
        // between them with the imgui combo. With no argument it scans the repository's
        // Project/Models folder (found by walking up from the executable); an optional
        // argument is a directory to scan, or a .pmx file whose parent directory is scanned.
        std::filesystem::path scanDirectory;
        if (argc >= 2)
        {
            const std::filesystem::path argument(argv[1]);
            scanDirectory = std::filesystem::is_directory(argument) ? argument : argument.parent_path();
        }
        else
        {
            const std::filesystem::path projectDirectory = FindProjectDirectory();
            scanDirectory = projectDirectory.empty() ? ExecutableDirectory() : projectDirectory / L"Models";
        }

        MmdLab::Scene scene;
        scene.LoadFromDirectory(scanDirectory);

        if (scene.ModelCount() == 0)
        {
            std::wcerr << L"No .pmx models found in " << scanDirectory.wstring() << L".\n";
        }

        MmdLab::WindowsApplication application;
        application.Initialize(GetModuleHandleW(nullptr), SW_SHOWDEFAULT);

        RECT clientRect{};
        GetClientRect(application.GetWindowHandle(), &clientRect);
        const std::uint32_t width = static_cast<std::uint32_t>(clientRect.right);
        const std::uint32_t height = static_cast<std::uint32_t>(clientRect.bottom);

        // The three-thread pipeline (GameThread -> RenderThread -> RhiThread) plus two UI
        // edges: Win32 input forward to the RhiThread, and selection changes back.
        MmdLab::FrameResourcePool pool;
        MmdLab::Channel<MmdLab::FrameIndex, MmdLab::FrameResourcePool::kFrameCount> gameToRender;
        MmdLab::Channel<MmdLab::FrameIndex, MmdLab::FrameResourcePool::kFrameCount> renderToRhi;
        MmdLab::Channel<MmdLab::Win32InputMessage, MmdLab::kWin32InputQueueCapacity> inputQueue;
        MmdLab::Channel<MmdLab::UiRequest, MmdLab::kUiRequestQueueCapacity> uiQueue;

        application.SetInputSink(&inputQueue);

        MmdLab::RenderThread renderStage(gameToRender, renderToRhi, pool);
        MmdLab::RhiThread rhiStage(renderToRhi, pool, application.GetWindowHandle(), width, height, inputQueue, uiQueue);

        MmdLab::Thread renderThread(renderStage, L"RenderThread");
        MmdLab::Thread rhiThread(rhiStage, L"RhiThread");

        // GameThread role: apply UI selection changes, project the scene into each frame, and
        // produce one frame per iteration, throttled by the frame pool.
        MmdLab::FrameId frameId = 0;
        while (application.ProcessMessages())
        {
            while (const auto request = uiQueue.TryPop())
            {
                scene.Select(request->selectedModel);
            }

            const MmdLab::FrameIndex index = pool.Acquire();
            MmdLab::FrameResource& frame = pool.Get(index);
            frame.frameId = ++frameId;

            MmdLab::RenderFrame& renderFrame = frame.gameToRender;
            renderFrame.mesh = scene.SelectedMesh();
            renderFrame.textures = scene.SelectedTextures();
            renderFrame.modelGeneration = scene.Generation();
            renderFrame.modelNames = scene.DisplayNames();
            renderFrame.selectedModel = static_cast<std::uint32_t>(scene.SelectedModel());

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
