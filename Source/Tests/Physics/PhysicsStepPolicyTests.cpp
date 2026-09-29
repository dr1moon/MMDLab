#include "Runtime/Core/TestFramework.h"
#include "Runtime/Physics/PhysicsStepPolicy.h"

#include <cmath>

namespace
{
bool Near(const float a, const float b)
{
    return std::fabs(a - b) < 1e-6f;
}
} // namespace

MMDLAB_TEST(Physics.StepPolicy, PlayingSimulatesTheFrameTime)
{
    const MmdLab::PhysicsStep step = MmdLab::ResolvePhysicsStep(false, false, true, 10.0f, 10.5f, 1.0f / 60.0f);
    MMDLAB_CHECK(!step.reset);
    MMDLAB_CHECK(Near(step.deltaSeconds, 1.0f / 60.0f));
}

MMDLAB_TEST(Physics.StepPolicy, PausedFreezes)
{
    const MmdLab::PhysicsStep step = MmdLab::ResolvePhysicsStep(false, false, false, 10.0f, 10.0f, 1.0f / 60.0f);
    MMDLAB_CHECK(!step.reset);
    MMDLAB_CHECK(Near(step.deltaSeconds, 0.0f));
}

MMDLAB_TEST(Physics.StepPolicy, ANewMotionResets)
{
    MMDLAB_CHECK(MmdLab::ResolvePhysicsStep(true, false, true, 100.0f, 0.0f, 1.0f / 60.0f).reset);
    MMDLAB_CHECK(MmdLab::ResolvePhysicsStep(true, true, false, 0.0f, 0.0f, 1.0f / 60.0f).reset);
}

MMDLAB_TEST(Physics.StepPolicy, AShortScrubIsSimulatedOverItsDuration)
{
    // Three 30 fps frames in either direction, even while paused.
    const MmdLab::PhysicsStep forward = MmdLab::ResolvePhysicsStep(false, true, false, 10.0f, 13.0f, 1.0f / 60.0f);
    MMDLAB_CHECK(!forward.reset);
    MMDLAB_CHECK(Near(forward.deltaSeconds, 0.1f));
    const MmdLab::PhysicsStep backward = MmdLab::ResolvePhysicsStep(false, true, false, 13.0f, 10.0f, 1.0f / 60.0f);
    MMDLAB_CHECK(!backward.reset);
    MMDLAB_CHECK(Near(backward.deltaSeconds, 0.1f));
}

MMDLAB_TEST(Physics.StepPolicy, ALongJumpResets)
{
    MMDLAB_CHECK(MmdLab::ResolvePhysicsStep(false, true, true, 10.0f, 10.0f + MmdLab::kMaxSimulatedSeekFrames + 0.5f, 1.0f / 60.0f).reset);
    MMDLAB_CHECK(!MmdLab::ResolvePhysicsStep(false, true, true, 10.0f, 10.0f + MmdLab::kMaxSimulatedSeekFrames, 1.0f / 60.0f).reset);
}

MMDLAB_TEST(Physics.StepPolicy, TheLoopWrapIsNotAJump)
{
    // Advance() wrapping from the end back to frame 0 is not a seek, so it keeps simulating.
    const MmdLab::PhysicsStep step = MmdLab::ResolvePhysicsStep(false, false, true, 318.9f, 0.4f, 1.0f / 60.0f);
    MMDLAB_CHECK(!step.reset);
    MMDLAB_CHECK(Near(step.deltaSeconds, 1.0f / 60.0f));
}
