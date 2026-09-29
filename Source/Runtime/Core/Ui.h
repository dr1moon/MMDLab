#pragma once

#include <cstdint>

namespace MmdLab
{
// Bounded queue capacities for the three UI edges. Win32 input is droppable (TryPush on a full
// queue discards a redundant mouse move), so it is larger; selection and camera inputs are rare
// or small per frame.
inline constexpr std::size_t kWin32InputQueueCapacity = 256;
inline constexpr std::size_t kUiRequestQueueCapacity = 8;
inline constexpr std::size_t kCameraInputQueueCapacity = 8;

enum class UiCommand : std::uint32_t
{
    SelectLevel,
    SetInstanceVisible,
    SelectMotion,
    SetMotionPlaying,
    SeekMotion,
    SetCameraFov,
};

// RhiThread -> GameThread: a user edit from the imgui panel. SelectLevel changes the level combo;
// SetInstanceVisible toggles one model of the selected level; SelectMotion / SetMotionPlaying /
// SeekMotion drive VMD playback. The GameThread applies the command to its authoritative world state.
struct UiRequest
{
    UiCommand command = UiCommand::SelectLevel;
    std::uint32_t index = 0;   // Level index (SelectLevel), instance index (SetInstanceVisible), or motion index (SelectMotion).
    bool visible = true;       // SetInstanceVisible: the instance's new visibility state.
    bool playing = true;       // SetMotionPlaying: whether the motion advances.
    float seekFrames = 0.0f;   // SeekMotion: target playback time in 30 fps frames.
    float fovDegrees = 45.0f;  // SetCameraFov: vertical field of view in degrees.
};

// RhiThread -> GameThread: orbit/pan/zoom deltas the user produced this frame (mouse
// drag/wheel while the pointer is not over an imgui widget). The GameThread applies them to the
// world's camera before projecting it into the frame.
struct CameraInput
{
    float orbitDeltaX = 0.0f;
    float orbitDeltaY = 0.0f;
    float panDeltaX = 0.0f;
    float panDeltaY = 0.0f;
    float zoomDelta = 0.0f;
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
