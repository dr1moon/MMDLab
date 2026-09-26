#include "Runtime/DX12/RhiThread.h"

#include "Runtime/DX12/Dx12Renderer.h"
#include "Runtime/DX12/RenderDocCapture.h"

#include "imgui.h"
#include "imgui_impl_dx12.h"
#include "imgui_impl_win32.h"

#include <exception>
#include <iostream>
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
    Channel<UiRequest, kUiRequestQueueCapacity>& uiQueue)
    : input_(&input)
    , pool_(&pool)
    , window_(window)
    , width_(width)
    , height_(height)
    , inputQueue_(&inputQueue)
    , uiQueue_(&uiQueue)
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

        return true;
    }
    catch (const std::exception& exception)
    {
        std::cerr << "RhiThread init failed: " << exception.what() << '\n';
        return false;
    }
}

uint32_t RhiThread::Run()
{
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

        ImGui::Begin("Models");
        if (batch.modelNames.empty())
        {
            ImGui::Text("No models found. Place .pmx files next to the viewer (or pass a directory).");
        }
        else
        {
            std::vector<const char*> items;
            items.reserve(batch.modelNames.size());
            for (const std::string& name : batch.modelNames)
            {
                items.push_back(name.c_str());
            }

            int selected = static_cast<int>(batch.selectedModel);
            if (ImGui::Combo("Model", &selected, items.data(), static_cast<int>(items.size())))
            {
                if (selected >= 0 && selected < static_cast<int>(items.size()))
                {
                    uiQueue_->TryPush(UiRequest{ static_cast<std::uint32_t>(selected) });
                }
            }
        }
        ImGui::End();

        // Camera control: orbit with a left-drag and zoom with the wheel, but only while the
        // pointer is not over an imgui widget (WantCaptureMouse).
        const ImGuiIO& io = ImGui::GetIO();
        if (!io.WantCaptureMouse)
        {
            if (io.MouseDown[0])
            {
                renderer_->Orbit(io.MouseDelta.x, io.MouseDelta.y);
            }
            if (io.MouseWheel != 0.0f)
            {
                renderer_->Zoom(io.MouseWheel);
            }
        }

        ImGui::Render();
        ImDrawData* drawData = ImGui::GetDrawData();

        // Rebuild GPU resources only when the selected model's generation changed.
        if (batch.mesh != nullptr && batch.modelGeneration != lastModelGeneration_)
        {
            renderer_->SetModel(*batch.mesh, batch.textures);
            lastModelGeneration_ = batch.modelGeneration;
        }

        const bool captureThisFrame =
            captureRequested_ && capture_ != nullptr && capture_->IsAvailable();

        if (captureThisFrame)
        {
            capture_->StartCapture();
        }

        std::uint64_t fenceValue = 0;
        try
        {
            fenceValue = renderer_->Render(batch.drawPackets, drawData);
        }
        catch (const std::exception& exception)
        {
            std::cerr << "RhiThread render failed: " << exception.what() << '\n';
        }
        frame.gpuFenceValue = fenceValue;

        if (captureThisFrame)
        {
            const bool captured = capture_->EndCapture();
            std::cerr << "capture: " << (captured ? "ok " : "failed ")
                      << capture_->LastCapturePath() << '\n';
            captureRequested_ = false;
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

    return 0;
}

void RhiThread::Stop()
{
    input_->Close();
}
} // namespace MmdLab
