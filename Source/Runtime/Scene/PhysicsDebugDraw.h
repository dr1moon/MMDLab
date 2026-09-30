#pragma once

#include "Runtime/Asset/PhysicsAsset.h"

#include <DirectXMath.h>

#include <vector>

namespace MmdLab
{
// One screen-space debug line, in pixels from the viewport's top-left corner.
struct DebugLine2D
{
    float x0 = 0.0f;
    float y0 = 0.0f;
    float x1 = 0.0f;
    float y1 = 0.0f;
    BodyMode mode = BodyMode::FollowBone; // Colors the line by how the body is driven.
};

// Appends a wireframe of `body` projected through `modelToClip` (body world is model space, so
// this is instance world * view * projection) to `out`: three rings for a sphere, the twelve
// edges of a box, and for a capsule its two end rings, four side lines, and the half circles of
// its caps. Segments with an end behind the near plane are dropped rather than clipped.
void AppendBodyWireframe(const PhysicsDebugBody& body, const DirectX::XMMATRIX& modelToClip,
    float viewportWidth, float viewportHeight, std::vector<DebugLine2D>& out);
} // namespace MmdLab
