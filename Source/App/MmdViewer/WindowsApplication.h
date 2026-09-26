#pragma once

#include "Runtime/Core/Channel.h"
#include "Runtime/Core/Ui.h"

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

    // Processes all pending window messages. Returns false when the window has closed.
    bool ProcessMessages();

    [[nodiscard]] HWND GetWindowHandle() const { return windowHandle_; }

    // The sink ProcessMessages() forwards raw Win32 input into; the RhiThread drains it and
    // feeds imgui's Win32 backend. Optional (input is dropped while unset).
    void SetInputSink(Channel<Win32InputMessage, kWin32InputQueueCapacity>* sink) { inputSink_ = sink; }

private:
    static LRESULT CALLBACK WindowProcedure(HWND windowHandle, UINT message, WPARAM wordParameter, LPARAM longParameter);

    HINSTANCE instanceHandle_ = nullptr;
    HWND windowHandle_ = nullptr;
    ATOM windowClass_ = 0;
    Channel<Win32InputMessage, kWin32InputQueueCapacity>* inputSink_ = nullptr;
};
} // namespace MmdLab
