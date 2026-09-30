#include "App/MmdViewer/ModelFrame.h"
#include "App/MmdViewer/World.h"
#include "Runtime/Physics/PhysicsStepPolicy.h"
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

#include "tracy/Tracy.hpp"

#include <windows.h>

#include <chrono>
#include <cstdint>
#include <cwchar>
#include <exception>
#include <filesystem>
#include <format>
#include <span>
#include <string>
#include <vector>

int wmain(const int argc, wchar_t* argv[])
{
    // Diagnostics are written as UTF-8; switch the console to UTF-8 so non-ASCII model names
    // render correctly instead of as the system ANSI code page (GBK on Chinese Windows).
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);

    // The main thread is the GameThread; give it a role name so its log lines read "[GameThread]"
    // instead of a raw OS thread id. Worker threads register themselves inside Thread::RunInternal.
    MmdLab::RegisterThreadName("GameThread");
    tracy::SetThreadName("GameThread");

    try
    {
        // The working directory is the project (see Project/Project.md): the editor loads every
        // .pmx under Models/ at startup, and the user switches between them with the imgui combo,
        // and plays the .vmd files under Motions/. `--frames N` bounds the run to N produced
        // frames (for automated profiling); any other argument overrides the model scan directory
        // (or names a .pmx file whose parent directory is scanned).
        const std::filesystem::path projectDirectory = std::filesystem::current_path();
        std::filesystem::path scanDirectory = projectDirectory / L"Models";
        std::uint32_t frameLimit = 0;
        for (int i = 1; i < argc; ++i)
        {
            if (std::wcscmp(argv[i], L"--frames") == 0 && i + 1 < argc)
            {
                const long parsed = std::wcstol(argv[i + 1], nullptr, 10);
                if (parsed > 0)
                {
                    frameLimit = static_cast<std::uint32_t>(parsed);
                }
                ++i;
                continue;
            }
            const std::filesystem::path argument(argv[i]);
            scanDirectory = std::filesystem::is_directory(argument) ? argument : argument.parent_path();
        }
        MmdLab::LogInfo("App", std::format("Project directory {}", MmdLab::WideToUtf8(projectDirectory.wstring())));

        // Create the window before the async asset load so a future splash/logo can render
        // while the models stream in. The client size is read here and handed to the RhiThread
        // below; the RhiThread also reads the window handle at construction.
        MmdLab::WindowsApplication application;
        application.Initialize(GetModuleHandleW(nullptr), SW_SHOWDEFAULT);

        RECT clientRect{};
        GetClientRect(application.GetWindowHandle(), &clientRect);
        const std::uint32_t width = static_cast<std::uint32_t>(clientRect.right);
        const std::uint32_t height = static_cast<std::uint32_t>(clientRect.bottom);

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

        // Scan every VMD motion in the project's Motions/ and auto-select the first so the loaded
        // models are posed once they finish loading. Motions are optional; with none the viewer
        // stays in the bind pose. The Motion tab switches and scrubs playback.
        world.LoadMotionsFromDirectory(projectDirectory / L"Motions");
        if (!world.Motions().empty())
        {
            world.SelectMotion(0);
        }

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
        // Per-model evaluation scratch, parallel to the registry models and reused across frames.
        std::vector<MmdLab::ModelFrameScratch> modelScratch;
        std::vector<MmdLab::ModelFrameStats> modelStats;
        std::uint32_t lastPoseGeneration = 0;
        std::uint32_t lastMotionGeneration = 0;
        float lastMotionFrames = 0.0f;
        if (frameLimit > 0)
        {
            MmdLab::LogInfo("App", std::format("Running {} frame(s), then exiting", frameLimit));
        }
        auto previousTime = std::chrono::steady_clock::now();
        while (application.ProcessMessages())
        {
            FrameMark;
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
                    case MmdLab::UiCommand::SelectMotion:
                        world.SelectMotion(request->index);
                        break;
                    case MmdLab::UiCommand::SetMotionPlaying:
                        world.SetMotionPlaying(request->playing);
                        break;
                    case MmdLab::UiCommand::SeekMotion:
                        world.SeekMotion(request->seekFrames);
                        break;
                    case MmdLab::UiCommand::SetCameraFov:
                        world.GetCamera().SetFovDegrees(request->fovDegrees);
                        break;
                    case MmdLab::UiCommand::SetPhysicsEnabled:
                        world.SetPhysicsEnabled(request->visible);
                        break;
                    case MmdLab::UiCommand::SetPhysicsDebugDraw:
                        world.SetPhysicsDebugDraw(request->visible);
                        break;
                    case MmdLab::UiCommand::SetPhysicsGround:
                        world.SetPhysicsGround(request->visible);
                        break;
                    case MmdLab::UiCommand::ResetPhysics:
                        world.RequestPhysicsReset();
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
            world.Animator().Advance(deltaTime);

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
            renderFrame.motions = world.Motions();
            renderFrame.selectedMotion = world.SelectedMotion();
            renderFrame.motionPlaying = world.Animator().IsPlaying();
            renderFrame.motionTimeFrames = world.Animator().TimeFrames();
            renderFrame.motionDurationFrames = world.Animator().DurationFrames();
            renderFrame.camera = world.GetCamera();

            // Evaluate each model's skinning palette and concatenate them in model-index order so
            // the renderer slices per-model ranges from one frame-local snapshot. Every model's
            // slice is sized up front (bone, vertex, and body counts are fixed per model), so each
            // evaluation writes only its own range and touches only its own scratch.
            const auto& models = modelRegistry.Models();
            MmdLab::PhysicsStats physicsStats;
            physicsStats.enabled = world.PhysicsEnabled();
            physicsStats.debugDraw = world.PhysicsDebugDraw();
            physicsStats.ground = world.PhysicsGround();
            frame.bonePaletteOffsetSnapshot.assign(models.size() + 1, 0);
            frame.morphDeltaOffsetSnapshot.assign(models.size() + 1, 0);
            frame.physicsBodyOffsetSnapshot.assign(models.size() + 1, 0);
            for (std::size_t m = 0; m < models.size(); ++m)
            {
                const MmdLab::Model& model = models[m];
                const MmdLab::PhysicsScene* physics = world.PhysicsFor(m);
                const std::size_t bodies = physics != nullptr && physicsStats.debugDraw ? physics->BodyCount() : 0;
                frame.bonePaletteOffsetSnapshot[m + 1] = frame.bonePaletteOffsetSnapshot[m]
                    + static_cast<std::uint32_t>(model.skeleton.bones.size());
                frame.morphDeltaOffsetSnapshot[m + 1] = frame.morphDeltaOffsetSnapshot[m]
                    + static_cast<std::uint32_t>(model.mesh.vertices.size() * 3);
                frame.physicsBodyOffsetSnapshot[m + 1] = frame.physicsBodyOffsetSnapshot[m]
                    + static_cast<std::uint32_t>(bodies);
                if (physics != nullptr)
                {
                    physicsStats.bodyCount += static_cast<std::uint32_t>(physics->BodyCount());
                    physicsStats.constraintCount += static_cast<std::uint32_t>(model.physics.constraints.size());
                }
            }
            frame.bonePaletteSnapshot.resize(frame.bonePaletteOffsetSnapshot.back());
            frame.morphDeltaSnapshot.resize(frame.morphDeltaOffsetSnapshot.back());
            frame.physicsBodySnapshot.resize(frame.physicsBodyOffsetSnapshot.back());
            modelScratch.resize(models.size());
            modelStats.assign(models.size(), {});

            // Physics freezes while paused, follows a short timeline scrub, and resets on a new
            // motion or a long jump instead of simulating it.
            const MmdLab::VmdAnimator& animator = world.Animator();
            const bool motionChanged = animator.MotionGeneration() != lastMotionGeneration;
            const bool seeked = !motionChanged && animator.PoseGeneration() != lastPoseGeneration;
            MmdLab::ModelFrameInput modelInput;
            modelInput.animator = &animator;
            modelInput.physicsStep = MmdLab::ResolvePhysicsStep(motionChanged, seeked,
                !animator.HasMotion() || animator.IsPlaying(), lastMotionFrames, animator.TimeFrames(), deltaTime);
            modelInput.physicsStep.reset = world.ConsumePhysicsReset() || modelInput.physicsStep.reset;
            modelInput.physicsEnabled = physicsStats.enabled;
            modelInput.physicsDebugDraw = physicsStats.debugDraw;
            lastMotionGeneration = animator.MotionGeneration();
            lastPoseGeneration = animator.PoseGeneration();
            lastMotionFrames = animator.TimeFrames();

            const auto evaluateModel = [&](const std::size_t m)
            {
                const auto slice = [](auto& snapshot, const std::vector<std::uint32_t>& offsets, const std::size_t i)
                {
                    return std::span(snapshot).subspan(offsets[i], offsets[i + 1] - offsets[i]);
                };
                MmdLab::ModelFrameOutput output;
                output.palette = slice(frame.bonePaletteSnapshot, frame.bonePaletteOffsetSnapshot, m);
                output.morphDeltas = slice(frame.morphDeltaSnapshot, frame.morphDeltaOffsetSnapshot, m);
                output.physicsBodies = slice(frame.physicsBodySnapshot, frame.physicsBodyOffsetSnapshot, m);
                modelStats[m] = MmdLab::EvaluateModelFrame(
                    models[m], world.PhysicsFor(m), modelInput, modelScratch[m], output);
            };
            {
                ZoneScopedN("GameThread.EvaluateModels");
                for (std::size_t m = 0; m < models.size(); ++m)
                {
                    evaluateModel(m);
                }
            }
            for (const MmdLab::ModelFrameStats& stats : modelStats)
            {
                physicsStats.simulateMilliseconds += stats.simulateMilliseconds;
            }
            renderFrame.bonePalette = frame.bonePaletteSnapshot;
            renderFrame.bonePaletteOffsets = frame.bonePaletteOffsetSnapshot;
            renderFrame.morphDeltas = frame.morphDeltaSnapshot;
            renderFrame.morphDeltaOffsets = frame.morphDeltaOffsetSnapshot;
            renderFrame.physicsBodies = frame.physicsBodySnapshot;
            renderFrame.physicsBodyOffsets = frame.physicsBodyOffsetSnapshot;
            renderFrame.physicsStats = physicsStats;

            gameToRender.Push(index);

            // Bound the run to `frameLimit` produced frames (0 = unlimited) so an automated
            // profiling session captures a deterministic frame count and shuts down cleanly.
            if (frameLimit > 0 && frameId >= frameLimit)
            {
                break;
            }
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
