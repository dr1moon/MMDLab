#include "Runtime/DX12/RhiThread.h"

#include "Runtime/Asset/Model.h"
#include "Runtime/DX12/Dx12Renderer.h"
#include "Runtime/DX12/RenderDocCapture.h"
#include "Runtime/Scene/WorldData.h"
#include "Runtime/Core/Log.h"

#include "tracy/Tracy.hpp"

#include "imgui.h"
#include "imgui_impl_dx12.h"
#include "imgui_impl_win32.h"

#include <exception>
#include <filesystem>
#include <format>
#include <vector>

// imgui's Win32 backend deliberately keeps this declaration out of its header (see the #if 0
// block in imgui_impl_win32.h); the application forwards its window messages through it.
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace
{
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
} // namespace

namespace MmdLab
{
RhiThread::RhiThread(
    Channel<FrameIndex, FrameResourcePool::kFrameCount>& input,
    FrameResourcePool& pool,
    const HWND window,
    const std::uint32_t width,
    const std::uint32_t height,
    Channel<Win32InputMessage, kWin32InputQueueCapacity>& inputQueue,
    Channel<UiRequest, kUiRequestQueueCapacity>& uiQueue,
    Channel<CameraInput, kCameraInputQueueCapacity>& cameraQueue)
    : input_(&input)
    , pool_(&pool)
    , window_(window)
    , width_(width)
    , height_(height)
    , inputQueue_(&inputQueue)
    , uiQueue_(&uiQueue)
    , cameraQueue_(&cameraQueue)
{
}

RhiThread::~RhiThread()
{
    if (imguiContext_ != nullptr)
    {
        ImGui_ImplDX12_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext(imguiContext_);
        imguiContext_ = nullptr;
    }
}

bool RhiThread::Init()
{
    ZoneScopedN("RhiThread::Init");
    try
    {
        // Load RenderDoc before creating the device so it hooks device creation.
        capture_ = std::make_unique<RenderDocCapture>();
        captureRequested_ = GetEnvironmentVariableW(L"MMDLAB_CAPTURE", nullptr, 0) > 0;
        if (captureRequested_ && capture_->IsAvailable())
        {
            capture_->SetCapturePath("captures/mesh");
        }

        renderer_ = std::make_unique<Dx12Renderer>(window_, width_, height_);

        // imgui context and backends are owned here, on the D3D12 owner thread.
        IMGUI_CHECKVERSION();
        imguiContext_ = ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
        ImGui::StyleColorsDark();

        // ImGui builds the font atlas from this bundled OFL font at startup. Loading it beside
        // the executable makes CJK rendering independent of the operating system's font set.
        const std::filesystem::path fontPath = ExecutableDirectory() / L"Fonts" / L"FanWunMing-SB.ttf";
        ImFont* font = io.Fonts->AddFontFromFileTTF(
            fontPath.string().c_str(), 16.0f, nullptr, io.Fonts->GetGlyphRangesChineseFull());
        if (font == nullptr)
        {
            LogWarning("RhiThread", std::format("Failed to load bundled UI font {}", fontPath.string()));
            io.Fonts->AddFontDefault();
        }

        ImGui_ImplWin32_Init(window_);

        Dx12Renderer* renderer = renderer_.get();
        ImGui_ImplDX12_InitInfo initInfo{};
        initInfo.Device = renderer->GetDevice();
        initInfo.CommandQueue = renderer->GetQueue();
        initInfo.NumFramesInFlight = 2;
        initInfo.RTVFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
        initInfo.DSVFormat = DXGI_FORMAT_D32_FLOAT;
        initInfo.SrvDescriptorHeap = renderer->GetImGuiSrvHeap();
        // The alloc/free callbacks are plain function pointers, so the renderer is carried in
        // UserData rather than captured (the backend passes the stored InitInfo back in).
        initInfo.UserData = renderer;
        initInfo.SrvDescriptorAllocFn = [](ImGui_ImplDX12_InitInfo* info, D3D12_CPU_DESCRIPTOR_HANDLE* cpu, D3D12_GPU_DESCRIPTOR_HANDLE* gpu)
        {
            static_cast<Dx12Renderer*>(info->UserData)->AllocImGuiSrvDescriptor(cpu, gpu);
        };
        initInfo.SrvDescriptorFreeFn = [](ImGui_ImplDX12_InitInfo* info, D3D12_CPU_DESCRIPTOR_HANDLE cpu, D3D12_GPU_DESCRIPTOR_HANDLE gpu)
        {
            static_cast<Dx12Renderer*>(info->UserData)->FreeImGuiSrvDescriptor(cpu, gpu);
        };
        ImGui_ImplDX12_Init(&initInfo);

        LogInfo("RhiThread", std::format("initialized ({}x{})", width_, height_));

        return true;
    }
    catch (const std::exception& exception)
    {
        LogError("RhiThread", std::format("init failed: {}", exception.what()));
        return false;
    }
}

uint32_t RhiThread::Run()
{
    bool renderFailed = false;

    while (const auto index = input_->Pop())
    {
        ZoneScopedN("RhiThread::Frame");
        FrameResource& frame = pool_->Get(*index);
        const RenderWorkBatch& batch = frame.renderToRhi;

        // Forward queued Win32 messages to imgui's Win32 backend (which lives on this thread).
        while (const auto message = inputQueue_->TryPop())
        {
            ImGui_ImplWin32_WndProcHandler(
                window_,
                message->message,
                static_cast<WPARAM>(message->wordParameter),
                static_cast<LPARAM>(message->longParameter));

            if (message->message == WM_SIZE)
            {
                const std::uint32_t width = LOWORD(message->longParameter);
                const std::uint32_t height = HIWORD(message->longParameter);
                minimized_ = width == 0 || height == 0;
                if (!minimized_ && !renderFailed)
                {
                    try
                    {
                        renderer_->Resize(width, height);
                    }
                    catch (const std::exception& exception)
                    {
                        LogError("RhiThread", std::format("resize failed: {}", exception.what()));
                        renderFailed = true;
                        PostMessageW(window_, WM_CLOSE, 0, 0);
                    }
                }
            }
        }

        ImGui_ImplDX12_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        ImGui::Begin("MMDLab");
        if (ImGui::BeginTabBar("##MainTabs"))
        {
            if (ImGui::BeginTabItem("Levels"))
            {
                if (batch.levels.empty())
        {
            ImGui::Text("No models found. Place .pmx files next to the viewer (or pass a directory).");
        }
        else
        {
            std::vector<const char*> levelItems;
            levelItems.reserve(batch.levels.size());
            for (const Level& level : batch.levels)
            {
                levelItems.push_back(level.name.c_str());
            }

            int selected = static_cast<int>(batch.selectedLevel);
            if (ImGui::Combo("Level", &selected, levelItems.data(), static_cast<int>(levelItems.size())))
            {
                if (selected >= 0 && selected < static_cast<int>(levelItems.size()))
                {
                    uiQueue_->TryPush(UiRequest{ UiCommand::SelectLevel, static_cast<std::uint32_t>(selected), true });
                }
            }

            // Per-instance visibility toggles: hide props that overlap the character while
            // inspecting a level. The checkbox reflects the frame's snapshot; a change is
            // forwarded to the GameThread, which owns the authoritative visibility flag.
            ImGui::Separator();
            ImGui::Text("Models (%zu):", batch.instances.size());
            for (std::size_t i = 0; i < batch.instances.size(); ++i)
            {
                const ModelInstance& instance = batch.instances[i];
                const char* label = instance.modelIndex < batch.models.size()
                    ? batch.models[instance.modelIndex].name.c_str()
                    : "?";

                ImGui::PushID(static_cast<int>(i));
                bool visible = instance.visible;
                if (ImGui::Checkbox(label, &visible))
                {
                    uiQueue_->TryPush(UiRequest{ UiCommand::SetInstanceVisible, static_cast<std::uint32_t>(i), visible });
                }
                ImGui::PopID();
            }
                }
                ImGui::EndTabItem();
            }

            if (ImGui::BeginTabItem("Motion"))
            {
                if (batch.motions.empty())
                {
                    ImGui::Text("No motions found. Place .vmd files in <project>/Motions.");
                }
                else
                {
                    std::vector<const char*> motionItems;
                    motionItems.reserve(batch.motions.size());
                    for (const MotionEntry& motion : batch.motions)
                    {
                        motionItems.push_back(motion.name.c_str());
                    }

                    int selected = batch.selectedMotion < batch.motions.size()
                        ? static_cast<int>(batch.selectedMotion)
                        : 0;
                    if (ImGui::Combo("Motion", &selected, motionItems.data(), static_cast<int>(motionItems.size())))
                    {
                        if (selected >= 0 && selected < static_cast<int>(motionItems.size()))
                        {
                            uiQueue_->TryPush(UiRequest{ .command = UiCommand::SelectMotion, .index = static_cast<std::uint32_t>(selected) });
                        }
                    }

                    ImGui::Separator();

                    const bool playing = batch.motionPlaying;
                    if (ImGui::Button(playing ? "Pause" : "Play"))
                    {
                        uiQueue_->TryPush(UiRequest{ .command = UiCommand::SetMotionPlaying, .playing = !playing });
                    }
                    ImGui::SameLine();
                    if (ImGui::Button("Restart"))
                    {
                        uiQueue_->TryPush(UiRequest{ .command = UiCommand::SeekMotion, .seekFrames = 0.0f });
                    }

                    // VMD runs at a fixed 30 fps, so a frame count also reads as time / 30 seconds.
                    ImGui::Text("Time %.1f / %.1f frames (%.2f / %.2f s)",
                        batch.motionTimeFrames, batch.motionDurationFrames,
                        batch.motionTimeFrames / 30.0f, batch.motionDurationFrames / 30.0f);

                    float time = batch.motionTimeFrames;
                    const float maxFrames = batch.motionDurationFrames > 0.0f ? batch.motionDurationFrames : 1.0f;
                    if (ImGui::SliderFloat("Timeline", &time, 0.0f, maxFrames, "%.1f"))
                    {
                        uiQueue_->TryPush(UiRequest{ .command = UiCommand::SeekMotion, .seekFrames = time });
                    }
                }
                ImGui::EndTabItem();
            }

            if (ImGui::BeginTabItem("Inspect"))
            {
                bool anyVisible = false;
        for (std::size_t i = 0; i < batch.instances.size(); ++i)
        {
            const ModelInstance& instance = batch.instances[i];
            if (!instance.visible || instance.modelIndex >= batch.models.size())
            {
                continue;
            }
            anyVisible = true;
            const Model& model = batch.models[instance.modelIndex];

            ImGui::PushID(static_cast<int>(i));
            if (ImGui::CollapsingHeader(model.name.c_str()))
            {
                const auto textureName = [&](const std::int32_t index) -> const char*
                {
                    return (index >= 0 && static_cast<std::size_t>(index) < model.mesh.textures.size())
                        ? model.mesh.textures[static_cast<std::size_t>(index)].c_str()
                        : "(none)";
                };

                if (ImGui::TreeNode(std::format("Submeshes ({})", model.mesh.drawPackets.size()).c_str()))
                {
                    for (std::size_t s = 0; s < model.mesh.drawPackets.size(); ++s)
                    {
                        const DrawPacket& packet = model.mesh.drawPackets[s];
                        ImGui::Text("#%zu: %u indices (%u tris) -> material %u",
                            s, packet.indexCount, packet.indexCount / 3, packet.materialIndex);
                    }
                    ImGui::TreePop();
                }

                if (ImGui::TreeNode(std::format("Materials ({})", model.mesh.materials.size()).c_str()))
                {
                    for (std::size_t m = 0; m < model.mesh.materials.size(); ++m)
                    {
                        const MMDToonMaterial& material = model.mesh.materials[m];
                        if (ImGui::TreeNode(std::format("Material {}", m).c_str()))
                        {
                            ImGui::Text("Diffuse  (%.3f, %.3f, %.3f, %.3f)",
                                material.baseColor[0], material.baseColor[1], material.baseColor[2], material.baseColor[3]);
                            ImGui::Text("Specular (%.3f, %.3f, %.3f) strength %.3f",
                                material.specularColor[0], material.specularColor[1], material.specularColor[2], material.specularStrength);
                            ImGui::Text("Ambient  (%.3f, %.3f, %.3f)",
                                material.ambientColor[0], material.ambientColor[1], material.ambientColor[2]);
                            ImGui::Text("Edge     (%.3f, %.3f, %.3f, %.3f) size %.3f",
                                material.edgeColor[0], material.edgeColor[1], material.edgeColor[2], material.edgeColor[3], material.edgeSize);
                            ImGui::Text("Base tex:   %s", textureName(material.baseColorTexture));
                            ImGui::Text("Toon tex:   %s", textureName(material.toonTexture));
                            ImGui::Text("Sphere tex: %s", textureName(material.sphereTexture));
                            ImGui::Text("Flags 0x%02X (%s), sphere mode %u",
                                material.flags,
                                (material.flags & 0x01u) != 0 ? "double-sided" : "single-sided",
                                material.sphereMode);
                            ImGui::TreePop();
                        }
                    }
                    ImGui::TreePop();
                }

                if (ImGui::TreeNode(std::format("Textures ({})", model.mesh.textures.size()).c_str()))
                {
                    for (std::size_t t = 0; t < model.mesh.textures.size(); ++t)
                    {
                        ImGui::Text("#%zu: %s", t, model.mesh.textures[t].c_str());
                    }
                    ImGui::TreePop();
                }
            }
            ImGui::PopID();
        }
                if (!anyVisible)
                {
                    ImGui::Text("No visible models.");
                }
                ImGui::EndTabItem();
            }

            if (ImGui::BeginTabItem("Camera"))
            {
                float fovDegrees = batch.camera.fovDegrees;
                if (ImGui::SliderFloat("Vertical FOV", &fovDegrees, 10.0f, 120.0f, "%.1f deg"))
                {
                    uiQueue_->TryPush(UiRequest{ .command = UiCommand::SetCameraFov, .fovDegrees = fovDegrees });
                }
                ImGui::EndTabItem();
            }

            ImGui::EndTabBar();
        }
        ImGui::End();

        // Camera control: orbit with a left-drag, pan with a middle-drag, and zoom with the
        // wheel, but only while the pointer is not over an imgui widget (WantCaptureMouse).
        // The deltas are forwarded to the GameThread, which owns the camera and applies them
        // before projecting the frame.
        const ImGuiIO& io = ImGui::GetIO();
        if (!io.WantCaptureMouse)
        {
            const bool orbiting = io.MouseDown[0];
            const bool panning = io.MouseDown[2]; // Middle button.
            const bool zooming = io.MouseWheel != 0.0f;
            if (orbiting || panning || zooming)
            {
                cameraQueue_->TryPush(CameraInput{
                    orbiting ? io.MouseDelta.x : 0.0f,
                    orbiting ? io.MouseDelta.y : 0.0f,
                    panning ? io.MouseDelta.x : 0.0f,
                    panning ? io.MouseDelta.y : 0.0f,
                    zooming ? io.MouseWheel : 0.0f });
            }
        }

        ImGui::Render();
        ImDrawData* drawData = ImGui::GetDrawData();

        if (!renderFailed && !minimized_)
        {
            // Build GPU resources only when the selected level's generation changed.
            if (!batch.instances.empty() && batch.levelGeneration != lastLevelGeneration_)
            {
                renderer_->EnsureModelsResident(batch.instances, batch.models);
                lastLevelGeneration_ = batch.levelGeneration;
            }

            // Capture only once the selected level has at least one resident instance; the
            // async load may still be in flight on the first frame, which would capture an
            // empty scene.
            const bool captureThisFrame =
                captureRequested_ && capture_ != nullptr && capture_->IsAvailable()
                && !batch.instances.empty();

            if (captureThisFrame)
            {
                capture_->StartCapture();
            }

            try
            {
                frame.gpuFenceValue = renderer_->Render(
                    batch.instances, batch.models, batch.bonePalette, batch.bonePaletteOffsets,
                    batch.morphDeltas, batch.morphDeltaOffsets, batch.camera, drawData);
            }
            catch (const std::exception& exception)
            {
                // A failed Render leaves the command list/allocator in an unknown state, so
                // retrying would fail again and spin. Mark the pipeline failed, ask the window to
                // close so the GameThread stops producing, and retire this frame immediately below.
                LogError("RhiThread", std::format("render failed: {}", exception.what()));
                renderFailed = true;
                PostMessageW(window_, WM_CLOSE, 0, 0);
                frame.gpuFenceValue = 0;
            }

            if (captureThisFrame)
            {
                const bool captured = capture_->EndCapture();
                LogInfo("RenderDoc", std::format("capture: {} {}", captured ? "ok" : "failed",
                                                 capture_->LastCapturePath()));
                captureRequested_ = false;
            }
        }
        else
        {
            frame.gpuFenceValue = 0;
        }

        pendingRetirement_.push_back(*index);

        // Retire frames whose GPU work has completed, oldest first.
        while (!pendingRetirement_.empty()
            && renderer_->IsFrameComplete(pool_->Get(pendingRetirement_.front()).gpuFenceValue))
        {
            pool_->Release(pendingRetirement_.front());
            pendingRetirement_.pop_front();
        }
    }

    // Drain any frames still awaiting GPU retirement before the thread exits.
    for (const FrameIndex index : pendingRetirement_)
    {
        pool_->Release(index);
    }
    pendingRetirement_.clear();

    return renderFailed ? 1 : 0;
}

void RhiThread::Stop()
{
    input_->Close();
}
} // namespace MmdLab
