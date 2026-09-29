#pragma once

#include <cmath>
#include <cstdint>

namespace MmdLab
{
// What physics does this frame: reset to the pose (a jump the simulation must not see as
// velocity) or simulate for `deltaSeconds` (0 freezes the simulated bones in place while the
// follow-bone bodies still track the pose).
struct PhysicsStep
{
    bool reset = false;
    float deltaSeconds = 0.0f;
};

// A timeline scrub this many 30 fps frames or shorter is simulated over its own duration, so
// dragging the slider keeps hair and cloth moving continuously; a longer jump resets.
inline constexpr float kMaxSimulatedSeekFrames = 6.0f;

// Decides the physics step from the playback state. `motionChanged`: a new motion was installed.
// `seeked`: the user moved the timeline. `playing`: playback advances. `previousFrames` and
// `currentFrames`: the playback time last frame and now, in 30 fps frames.
[[nodiscard]] inline PhysicsStep ResolvePhysicsStep(const bool motionChanged, const bool seeked,
    const bool playing, const float previousFrames, const float currentFrames, const float frameDeltaSeconds)
{
    if (motionChanged)
    {
        return { true, 0.0f };
    }
    if (seeked)
    {
        const float jump = std::fabs(currentFrames - previousFrames);
        if (jump > kMaxSimulatedSeekFrames)
        {
            return { true, 0.0f };
        }
        return { false, jump / 30.0f };
    }
    return { false, playing ? frameDeltaSeconds : 0.0f };
}
} // namespace MmdLab
