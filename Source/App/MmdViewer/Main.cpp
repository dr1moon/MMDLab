#include "App/MmdViewer/World.h"
#include "Runtime/Asset/AssetIo.h"
#include "Runtime/Asset/ModelRegistry.h"
#include "App/MmdViewer/WindowsApplication.h"
#include "Runtime/Core/Channel.h"
#include "Runtime/Core/CpuBudget.h"
#include "Runtime/Core/FrameResource.h"
#include "Runtime/Core/FrameResourcePool.h"
#include "Runtime/Core/Log.h"
#include "Runtime/Core/Thread.h"
#include "Runtime/Core/Ui.h"
#include "Runtime/Core/Utf8.h"
#include "Runtime/DX12/RhiThread.h"
#include "Runtime/Render/RenderThread.h"

#include <windows.h>

#include <chrono>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <format>
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
    // Diagnostics are written as UTF-8; switch the console to UTF-8 so non-ASCII model names
    // render correctly instead of as the system ANSI code page (GBK on Chinese Windows).
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);

    // The main thread is the GameThread; give it a role name so its log lines read "[GameThread]"
    // instead of a raw OS thread id. Worker threads register themselves inside Thread::RunInternal.
    MmdLab::RegisterThreadName("GameThread");

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

        // The async asset-load edges are declared before the world so they outlive both it and
        // the I/O workers. The group starts its workers here so the startup enqueue has live
        // consumers; LoadFromDirectory hands the request queue to the world and enqueues every
        // level onto it.
        MmdLab::Channel<MmdLab::LoadRequest, MmdLab::kLoadRequestCapacity> loadRequestQueue;
        MmdLab::Channel<MmdLab::LoadResult, MmdLab::kLoadResultCapacity> loadResultQueue;
        MmdLab::IoThreadsGroup ioGroup(MmdLab::IoWorkerCount(), loadRequestQueue, loadResultQueue);

        MmdLab::ModelRegistry modelRegistry;
        MmdLab::World world;
        world.LoadFromDirectory(scanDirectory, modelRegistry, loadRequestQueue);

        MmdLab::LogInfo("App", std::format("Scanning {}: {} level(s)",
            MmdLab::WideToUtf8(scanDirectory.wstring()), world.LevelCount()));

        if (world.LevelCount() == 0)
        {
            MmdLab::LogError("Asset", std::format("No .pmx models found in {}", scanDirectory.string()));
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
        MmdLab::Channel<MmdLab::CameraInput, MmdLab::kCameraInputQueueCapacity> cameraQueue;

        application.SetInputSink(&inputQueue);

        MmdLab::RenderThread renderStage(gameToRender, renderToRhi, pool);
        MmdLab::RhiThread rhiStage(renderToRhi, pool, application.GetWindowHandle(), width, height, inputQueue, uiQueue, cameraQueue);

        MmdLab::Thread renderThread(renderStage, L"RenderThread");
        MmdLab::Thread rhiThread(rhiStage, L"RhiThread");

        // The RhiThread owns the device and can fail during Init() (no adapter, missing feature
        // level, ...). Bail before the game loop so a half-built pipeline cannot deadlock waiting
        // on a consumer that never runs.
        if (rhiThread.ExitCode() == MmdLab::Thread::InitFailureExitCode)
        {
            MmdLab::LogError("App", "RhiThread failed to initialize; shutting down.");
            renderThread.RequestStop();
            return EXIT_FAILURE;
        }

        // GameThread role: apply UI selection changes, project the scene into each frame, and
        // produce one frame per iteration, throttled by the frame pool.
        MmdLab::FrameId frameId = 0;
        auto previousTime = std::chrono::steady_clock::now();
        while (application.ProcessMessages())
        {
            const auto currentTime = std::chrono::steady_clock::now();
            const float deltaTime = std::chrono::duration<float>(currentTime - previousTime).count();
            previousTime = currentTime;

            while (const auto request = uiQueue.TryPop())
            {
                switch (request->command)
                {
                    case MmdLab::UiCommand::SelectLevel:
                        world.SelectLevel(request->index);
                        break;
                    case MmdLab::UiCommand::SetInstanceVisible:
                        world.SetInstanceVisible(request->index, request->visible);
                        break;
                }
            }
            while (const auto input = cameraQueue.TryPop())
            {
                world.GetCamera().Orbit(input->orbitDeltaX, input->orbitDeltaY);
                world.GetCamera().Pan(input->panDeltaX, input->panDeltaY);
                world.GetCamera().Zoom(input->zoomDelta);
            }
            world.GetCamera().Tick(deltaTime);

            // Install any completed async loads; a completion for the selected level frames the
            // camera and bumps the generation so the RhiThread builds its GPU resources.
            while (auto result = loadResultQueue.TryPop())
            {
                world.OnLoadResult(std::move(*result));
            }

            const MmdLab::FrameIndex index = pool.Acquire();
            MmdLab::FrameResource& frame = pool.Get(index);
            frame.frameId = ++frameId;

            MmdLab::RenderFrame& renderFrame = frame.gameToRender;
            // Copy the selected level's instances so the frame carries an immutable snapshot
            // of the mutable visibility flags, not a span into the world's live state.
            const auto selectedInstances = world.SelectedInstances();
            frame.instanceSnapshot.assign(selectedInstances.begin(), selectedInstances.end());
            renderFrame.instances = frame.instanceSnapshot;
            renderFrame.models = modelRegistry.Models();
            renderFrame.levels = world.Levels();
            renderFrame.selectedLevel = static_cast<std::uint32_t>(world.SelectedLevel());
            renderFrame.levelGeneration = world.LevelGeneration();
            renderFrame.camera = world.GetCamera();

            gameToRender.Push(index);
        }

        // Shut down upstream-first so the pipeline drains in order.
        ioGroup.Stop();
        renderThread.RequestStop();
        rhiThread.RequestStop();

        return EXIT_SUCCESS;
    }
    catch (const std::exception& exception)
    {
        MmdLab::LogError("App", std::format("MMDLab Viewer failed: {}", exception.what()));
        return EXIT_FAILURE;
    }
}
