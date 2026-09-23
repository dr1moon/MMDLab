#include "App/MmdViewer/WindowsApplication.h"

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

int WindowsApplication::Run()
{
    MSG message{};
    while (true)
    {
        const BOOL result = GetMessageW(&message, nullptr, 0, 0);
        if (result == -1)
        {
            throw std::runtime_error("Failed to retrieve a Windows message.");
        }

        if (result == 0)
        {
            return static_cast<int>(message.wParam);
        }

        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
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
