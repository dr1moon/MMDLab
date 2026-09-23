#pragma once

#include <windows.h>

namespace MmdLab
{
class WindowsApplication final
{
public:
    WindowsApplication() = default;
    ~WindowsApplication();

    WindowsApplication(const WindowsApplication&) = delete;
    WindowsApplication& operator=(const WindowsApplication&) = delete;

    void Initialize(HINSTANCE instanceHandle, int showCommand);
    int Run();

private:
    static LRESULT CALLBACK WindowProcedure(HWND windowHandle, UINT message, WPARAM wordParameter, LPARAM longParameter);

    HINSTANCE instanceHandle_ = nullptr;
    HWND windowHandle_ = nullptr;
    ATOM windowClass_ = 0;
};
} // namespace MmdLab
