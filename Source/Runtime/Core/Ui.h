#pragma once

#include <cstdint>

namespace MmdLab
{
// Bounded queue capacities for the two UI edges. Input is droppable (TryPush on a full queue
// discards a redundant mouse move), so it is larger; selection changes are rare and small.
inline constexpr std::size_t kWin32InputQueueCapacity = 256;
inline constexpr std::size_t kUiRequestQueueCapacity = 8;

// RhiThread -> GameThread: the user changed the selected model in the imgui combo. The
// GameThread applies the selection to its authoritative scene and bumps the model generation.
struct UiRequest
{
    std::uint32_t selectedModel = 0;
};

// GameThread -> RhiThread: a Win32 message forwarded from the window message loop so the
// RhiThread can feed imgui's Win32 backend (which lives there, on the D3D12 owner thread).
// wParam/lParam are stored as intptr_t-sized integers to keep this header free of <windows.h>;
// the RhiThread casts them back to WPARAM/LPARAM at the WndProcHandler call site.
struct Win32InputMessage
{
    std::uint32_t message = 0;
    std::uintptr_t wordParameter = 0; // WPARAM (unsigned).
    std::intptr_t longParameter = 0;  // LPARAM (signed).
};
} // namespace MmdLab
