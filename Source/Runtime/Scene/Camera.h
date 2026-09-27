#pragma once

namespace MmdLab
{
// The world's orbit camera, owned by the World and projected into each frame for the renderer
// to turn into a view matrix. Mirrors a camera actor in the engine: game-side view state (a
// position and orientation), not renderer state.
//
// The orbit state (pivot, rotation, orbitDistance) is authoritative and is mutated directly by
// input, matching Unreal's editor viewport camera, which tracks the pointer 1:1 with no
// spring-arm lag. position is the view transform consumed by the renderer; VMD camera motion
// (a later milestone) can drive position and rotation directly.
struct Camera
{
    // Orbit state (mutated by input; Tick derives position from it).
    float pivot[3] = { 0.0f, 0.0f, 0.0f };     // Orbit center the camera looks at.
    float rotation[3] = { 0.0f, 0.0f, 0.0f };  // Euler degrees; YXZ (pitch, yaw, roll).
    float orbitDistance = 1.0f;                // Radius from the pivot.

    // View transform derived from the orbit state (position = pivot - forward * orbitDistance).
    float position[3] = { 0.0f, 0.0f, 0.0f };  // Eye position, projected to the renderer.

    float fovDegrees = 45.0f;
    float nearPlane = 0.01f;
    float farPlane = 1000.0f;

    void Orbit(float deltaX, float deltaY);
    void Zoom(float wheelDelta);
    void Pan(float deltaX, float deltaY);
    void FrameTo(const float boundsMin[3], const float boundsMax[3]);
    void Tick(float deltaTime);
};
} // namespace MmdLab
