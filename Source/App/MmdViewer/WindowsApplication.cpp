#include "App/MmdViewer/WindowsApplication.h"

#include "tracy/Tracy.hpp"

#include <stdexcept>

namespace
{
constexpr wchar_t WindowClassName[] = L"MMDLabViewerWindow";
constexpr wchar_t WindowTitle[] = L"MMDLab Viewer";
}

namespace MmdLab
{
WindowsApplication::~WindowsApplication()
{
    if (windowHandle_ != nullptr)
    {
        DestroyWindow(windowHandle_);
    }

    if (windowClass_ != 0)
    {
        UnregisterClassW(WindowClassName, instanceHandle_);
    }
}

void WindowsApplication::Initialize(const HINSTANCE instanceHandle, const int showCommand)
{
    ZoneScopedN("WindowsApplication::Initialize");
    instanceHandle_ = instanceHandle;

    WNDCLASSEXW windowClassDescription{};
    windowClassDescription.cbSize = sizeof(windowClassDescription);
    windowClassDescription.hInstance = instanceHandle_;
    windowClassDescription.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClassDescription.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    windowClassDescription.lpszClassName = WindowClassName;
    windowClassDescription.lpfnWndProc = WindowProcedure;

    windowClass_ = RegisterClassExW(&windowClassDescription);
    if (windowClass_ == 0)
    {
        throw std::runtime_error("Failed to register the MMDLab viewer window class.");
    }

    windowHandle_ = CreateWindowExW(
        0,
        WindowClassName,
        WindowTitle,
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        1600,
        900,
        nullptr,
        nullptr,
        instanceHandle_,
        this);

    if (windowHandle_ == nullptr)
    {
        throw std::runtime_error("Failed to create the MMDLab viewer window.");
    }

    ShowWindow(windowHandle_, showCommand);
    UpdateWindow(windowHandle_);
}

bool WindowsApplication::ProcessMessages()
{
    MSG message{};
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE) != 0)
    {
        if (message.message == WM_QUIT)
        {
            return false;
        }

        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    return true;
}

LRESULT CALLBACK WindowsApplication::WindowProcedure(
    const HWND windowHandle,
    const UINT message,
    const WPARAM wordParameter,
    const LPARAM longParameter)
{
    if (message == WM_NCCREATE)
    {
        const auto* createStructure = reinterpret_cast<const CREATESTRUCTW*>(longParameter);
        SetWindowLongPtrW(windowHandle, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(createStructure->lpCreateParams));
    }

    // Forward every message to the RhiThread's imgui input queue (dropped if full). This runs in
    // the window procedure rather than the PeekMessage loop because sent messages (WM_SIZE, and
    // the WM_SIZE burst of a live resize drag) reach the window procedure directly and never
    // pass through the queue; forwarding only queued messages left the swap chain and imgui at
    // the startup size.
    auto* application = reinterpret_cast<WindowsApplication*>(GetWindowLongPtrW(windowHandle, GWLP_USERDATA));
    if (application != nullptr && application->inputSink_ != nullptr)
    {
        application->inputSink_->TryPush(Win32InputMessage{
            static_cast<std::uint32_t>(message),
            static_cast<std::uintptr_t>(wordParameter),
            static_cast<std::intptr_t>(longParameter) });
    }

    switch (message)
    {
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;

    default:
        return DefWindowProcW(windowHandle, message, wordParameter, longParameter);
    }
}
} // namespace MmdLab
