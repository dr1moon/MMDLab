#include "Runtime/DX12/RhiThread.h"

#include "Runtime/Asset/Model.h"
#include "Runtime/DX12/Dx12Renderer.h"
#include "Runtime/DX12/RenderDocCapture.h"
#include "Runtime/Scene/WorldData.h"
#include "Runtime/Core/Log.h"

#include "imgui.h"
#include "imgui_impl_dx12.h"
#include "imgui_impl_win32.h"

#include <exception>
#include <format>
#include <functional>
#include <string>
#include <vector>

// imgui's Win32 backend deliberately keeps this declaration out of its header (see the #if 0
// block in imgui_impl_win32.h); the application forwards its window messages through it.
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

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

        // The embedded font has no CJK glyphs, so load a system font covering Simplified
        // Chinese for non-ASCII model names; fall back to the embedded font if it is absent.
        ImFont* font = io.Fonts->AddFontFromFileTTF(
            "C:/Windows/Fonts/msyh.ttc", 18.0f, nullptr, io.Fonts->GetGlyphRangesChineseSimplifiedCommon());
        if (font == nullptr)
        {
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
        }

        ImGui_ImplDX12_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        ImGui::Begin("Levels");
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

            // View-only debug overlays, owned by the RhiThread and passed straight to Render.
            ImGui::Separator();
            ImGui::Text("Visualization");
            ImGui::Checkbox("Skeleton", &showSkeleton_);
            ImGui::Checkbox("Skinning colors", &showSkinningColors_);

            // Per-model bone hierarchy, collapsed by default: each visible model lists its
            // skeleton as a tree so the hierarchy can be inspected alongside the 3D overlay.
            if (ImGui::CollapsingHeader("Skeleton tree"))
            {
                for (std::size_t i = 0; i < batch.instances.size(); ++i)
                {
                    const ModelInstance& instance = batch.instances[i];
                    if (!instance.visible || instance.modelIndex >= batch.models.size())
                    {
                        continue;
                    }
                    const Skeleton& skeleton = batch.models[instance.modelIndex].skeleton;

                    ImGui::PushID(static_cast<int>(i));
                    const std::string bonesLabel = std::format("Bones ({})", skeleton.bones.size());
                    if (ImGui::TreeNode(bonesLabel.c_str()))
                    {
                        std::function<void(std::int32_t)> emit;
                        emit = [&](std::int32_t boneIndex)
                        {
                            if (boneIndex < 0 || static_cast<std::size_t>(boneIndex) >= skeleton.bones.size())
                            {
                                return;
                            }
                            const Bone& bone = skeleton.bones[static_cast<std::size_t>(boneIndex)];
                            const bool leaf = skeleton.children[static_cast<std::size_t>(boneIndex)].empty();

                            ImGui::PushID(static_cast<int>(boneIndex));
                            const bool open = leaf
                                ? ImGui::TreeNodeEx(bone.name.c_str(), ImGuiTreeNodeFlags_Leaf)
                                : ImGui::TreeNodeEx(bone.name.c_str());
                            if (open && !leaf)
                            {
                                for (std::int32_t child : skeleton.children[static_cast<std::size_t>(boneIndex)])
                                {
                                    emit(child);
                                }
                                ImGui::TreePop();
                            }
                            ImGui::PopID();
                        };

                        for (std::size_t root = 0; root < skeleton.bones.size(); ++root)
                        {
                            if (skeleton.bones[root].parentIndex == -1)
                            {
                                emit(static_cast<std::int32_t>(root));
                            }
                        }
                        ImGui::TreePop();
                    }
                    ImGui::PopID();
                }
            }
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

        if (!renderFailed)
        {
            // Build GPU resources only when the selected level's generation changed.
            if (!batch.instances.empty() && batch.levelGeneration != lastLevelGeneration_)
            {
                renderer_->EnsureModelsResident(batch.instances, batch.models);
                lastLevelGeneration_ = batch.levelGeneration;
            }

            const bool captureThisFrame =
                captureRequested_ && capture_ != nullptr && capture_->IsAvailable();

            if (captureThisFrame)
            {
                capture_->StartCapture();
            }

            try
            {
                frame.gpuFenceValue = renderer_->Render(batch.instances, batch.models, batch.camera, drawData,
                                                        DebugViewOptions{ showSkeleton_, showSkinningColors_ });
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
            // Degraded drain: rendering is broken. Retire frames immediately and keep consuming
            // so the upstream stages can drain and shut down cleanly instead of deadlocking.
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
