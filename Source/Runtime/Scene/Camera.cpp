#include "Runtime/Scene/Camera.h"

#include <algorithm>
#include <cmath>

namespace
{
constexpr float kPi = 3.14159265f;
constexpr float kDegreesToRadians = kPi / 180.0f;
constexpr float kOrbitSensitivity = 0.25f; // Degrees per pixel of orbit drag.
constexpr float kPanScale = 0.002f;        // Pivot translation per pixel, relative to distance.
constexpr float kPitchLimitDegrees = 89.0f;
constexpr float kMinimumFovDegrees = 10.0f;
constexpr float kMaximumFovDegrees = 120.0f;

// The camera's forward vector (local +Z in world space) for a YXZ Euler rotation, assuming
// zero roll (the orbit interaction keeps roll at zero).
void Forward(const MmdLab::Camera& camera, float out[3])
{
    const float pitch = camera.rotation[0] * kDegreesToRadians;
    const float yaw = camera.rotation[1] * kDegreesToRadians;
    const float cosPitch = std::cos(pitch);
    out[0] = std::sin(yaw) * cosPitch;
    out[1] = -std::cos(yaw) * std::sin(pitch);
    out[2] = std::cos(yaw) * cosPitch;
}
} // namespace

namespace MmdLab
{
void Camera::Orbit(const float deltaX, const float deltaY)
{
    rotation[1] += deltaX * kOrbitSensitivity; // yaw.
    rotation[0] -= deltaY * kOrbitSensitivity; // pitch (drag up looks down).
    rotation[2] = 0.0f;                        // roll stays zero.
    rotation[0] = std::max(-kPitchLimitDegrees, std::min(kPitchLimitDegrees, rotation[0]));
}

void Camera::Zoom(const float wheelDelta)
{
    orbitDistance *= std::exp(-wheelDelta * 0.1f);
    const float minimum = nearPlane * 2.0f;
    const float maximum = farPlane * 0.5f;
    orbitDistance = std::max(minimum, std::min(maximum, orbitDistance));
}

void Camera::Pan(const float deltaX, const float deltaY)
{
    // Translate the pivot in the camera's plane, scaled by distance so the motion matches the
    // view at any zoom level.
    float forward[3];
    Forward(*this, forward);

    // right = normalize(up x forward), with world up = (0, 1, 0).
    float right[3] = { forward[2], 0.0f, -forward[0] };
    const float rightLength = std::sqrt(right[0] * right[0] + right[1] * right[1] + right[2] * right[2]);
    if (rightLength > 1e-6f)
    {
        right[0] /= rightLength;
        right[1] /= rightLength;
        right[2] /= rightLength;
    }

    // camera up = cross(forward, right).
    const float camUp[3] = {
        forward[1] * right[2] - forward[2] * right[1],
        forward[2] * right[0] - forward[0] * right[2],
        forward[0] * right[1] - forward[1] * right[0],
    };

    const float scale = orbitDistance * kPanScale;
    pivot[0] += (-right[0] * deltaX + camUp[0] * deltaY) * scale;
    pivot[1] += (-right[1] * deltaX + camUp[1] * deltaY) * scale;
    pivot[2] += (-right[2] * deltaX + camUp[2] * deltaY) * scale;
}

void Camera::SetFovDegrees(const float degrees)
{
    fovDegrees = std::max(kMinimumFovDegrees, std::min(kMaximumFovDegrees, degrees));
}

void Camera::FrameTo(const float boundsMin[3], const float boundsMax[3])
{
    float extent = 0.0f;
    float center[3];
    for (int axis = 0; axis < 3; ++axis)
    {
        center[axis] = (boundsMin[axis] + boundsMax[axis]) * 0.5f;
        extent = std::max(extent, boundsMax[axis] - boundsMin[axis]);
    }
    if (extent <= 0.0f)
    {
        extent = 1.0f;
    }

    const float distance = extent * 2.0f;
    pivot[0] = center[0]; pivot[1] = center[1]; pivot[2] = center[2];
    rotation[0] = 0.0f; rotation[1] = 0.0f; rotation[2] = 0.0f;
    orbitDistance = distance;
    nearPlane = extent * 0.01f;
    farPlane = extent * 100.0f + distance;

    // Identity rotation looks along +Z; place the eye at -Z of the center so the PMX model
    // (which faces -Z) is seen from the front.
    position[0] = center[0];
    position[1] = center[1];
    position[2] = center[2] - distance;
}

void Camera::Tick(const float /*deltaTime*/)
{
    // Recompute the eye position from the orbit state. Unlike a spring-arm camera there is no
    // lag: input mutates the state directly (matching Unreal's editor viewport), so the eye
    // stops as soon as the mouse is released.
    float forward[3];
    Forward(*this, forward);
    position[0] = pivot[0] - forward[0] * orbitDistance;
    position[1] = pivot[1] - forward[1] * orbitDistance;
    position[2] = pivot[2] - forward[2] * orbitDistance;
}

DirectX::XMMATRIX Camera::ViewMatrix() const
{
    using namespace DirectX;
    const XMMATRIX eyeRotation = XMMatrixRotationRollPitchYaw(
        XMConvertToRadians(rotation[0]), XMConvertToRadians(rotation[1]), XMConvertToRadians(rotation[2]));
    const XMMATRIX eyeTranslation = XMMatrixTranslation(position[0], position[1], position[2]);
    // Row-vector: apply rotation then translation. Inverting gives the view matrix.
    return XMMatrixInverse(nullptr, XMMatrixMultiply(eyeRotation, eyeTranslation));
}

DirectX::XMMATRIX Camera::ProjectionMatrix(const float aspect) const
{
    return DirectX::XMMatrixPerspectiveFovLH(DirectX::XMConvertToRadians(fovDegrees), aspect, nearPlane, farPlane);
}
} // namespace MmdLab
