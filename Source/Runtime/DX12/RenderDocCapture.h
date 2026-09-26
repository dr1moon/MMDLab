#pragma once

#include "Runtime/DX12/renderdoc_app.h"

#include <windows.h>

#include <string>

namespace MmdLab
{
// Optional integration with RenderDoc's in-application capture API. Loads renderdoc.dll at
// runtime and triggers frame captures. If the DLL cannot be loaded, IsAvailable() returns
// false and every call becomes a no-op, so it is safe to leave in place in any build.
class RenderDocCapture final
{
public:
    // dllPath: path to renderdoc.dll, or nullptr to load "renderdoc.dll" from the search path.
    explicit RenderDocCapture(const wchar_t* dllPath = nullptr);
    ~RenderDocCapture();

    RenderDocCapture(const RenderDocCapture&) = delete;
    RenderDocCapture& operator=(const RenderDocCapture&) = delete;

    [[nodiscard]] bool IsAvailable() const { return api_ != nullptr; }

    // Sets the capture path template, e.g. "captures/clear" -> "captures/clear_frameN.rdc".
    void SetCapturePath(const char* path);

    // Starts capturing on the default (wildcard) device and window.
    void StartCapture();

    // Ends the capture; returns true if a capture was written.
    [[nodiscard]] bool EndCapture();

    // The absolute path of the most recent capture, or empty if none.
    [[nodiscard]] std::string LastCapturePath() const;

private:
    HMODULE module_ = nullptr;
    RENDERDOC_API_1_6_0* api_ = nullptr;
};
} // namespace MmdLab
