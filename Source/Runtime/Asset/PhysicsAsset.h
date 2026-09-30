#pragma once

#include "Runtime/Asset/Skeleton.h"

#include <DirectXMath.h>

#include <cstdint>
#include <string>
#include <vector>

namespace MmdLab
{
// Invalid body index: PMX encodes "no rigid body" as -1.
inline constexpr std::uint32_t kInvalidBodyIndex = 0xFFFFFFFF;

// The collision shape of a body. Values match the PMX shape byte. Sizes: a sphere uses
// size[0] as its radius, a box uses size[0..2] as its half extents, and a capsule uses size[0]
// as its radius and size[1] as the height of its cylinder (along the body's local +Y).
enum class BodyShape : std::uint8_t
{
    Sphere = 0,
    Box = 1,
    Capsule = 2,
};

// How a body and its bone drive each other. Values match the PMX physics-mode byte.
enum class BodyMode : std::uint8_t
{
    FollowBone = 0,              // Kinematic: the animated bone moves the body.
    Physics = 1,                 // Simulated: the body moves the bone.
    PhysicsWithBonePosition = 2, // Simulated rotation; the bone keeps its animated position.
};

// One PMX rigid body. Position and rotation are the bind-pose transform in model space; the
// rotation is Euler angles in radians, applied Z first, then X, then Y.
struct BodySetup
{
    std::string name;
    std::uint16_t boneIndex = kInvalidBoneIndex; // kInvalidBoneIndex attaches to the model root.
    std::uint8_t group = 0;                      // Collision group, 0..15.
    std::uint16_t collisionMask = 0xFFFF;        // Bit g set = collides with group g.
    BodyShape shape = BodyShape::Sphere;
    BodyMode mode = BodyMode::FollowBone;
    float size[3] = { 0.0f, 0.0f, 0.0f };
    float position[3] = { 0.0f, 0.0f, 0.0f };
    float rotation[3] = { 0.0f, 0.0f, 0.0f };
    float mass = 0.0f;
    float linearDamping = 0.0f;
    float angularDamping = 0.0f;
    float restitution = 0.0f;
    float friction = 0.0f;
};

// One PMX joint: a six-degree-of-freedom spring constraint between two bodies. The joint frame
// is given in model space at bind pose (rotation as in BodySetup). Limits are relative to that
// frame; a stiffness of zero leaves that axis without a spring.
struct ConstraintSetup
{
    std::string name;
    std::uint32_t bodyA = kInvalidBodyIndex;
    std::uint32_t bodyB = kInvalidBodyIndex;
    float position[3] = { 0.0f, 0.0f, 0.0f };
    float rotation[3] = { 0.0f, 0.0f, 0.0f };
    float linearLowerLimit[3] = { 0.0f, 0.0f, 0.0f };
    float linearUpperLimit[3] = { 0.0f, 0.0f, 0.0f };
    float angularLowerLimit[3] = { 0.0f, 0.0f, 0.0f };
    float angularUpperLimit[3] = { 0.0f, 0.0f, 0.0f };
    float linearStiffness[3] = { 0.0f, 0.0f, 0.0f };
    float angularStiffness[3] = { 0.0f, 0.0f, 0.0f };
};

// The rigid bodies and joints a model declares for its physics simulation (hair, cloth,
// accessories, and the body colliders that push them).
struct PhysicsAsset
{
    std::vector<BodySetup> bodies;
    std::vector<ConstraintSetup> constraints;
};

// One simulated body's current shape and model-space transform, projected into a frame so the
// renderer can draw physics debug wireframes without touching the simulation.
struct PhysicsDebugBody
{
    DirectX::XMFLOAT4X4 world;
    BodyShape shape = BodyShape::Sphere;
    BodyMode mode = BodyMode::FollowBone;
    float size[3] = { 0.0f, 0.0f, 0.0f };
};

// Per-frame physics status for the UI.
struct PhysicsStats
{
    bool enabled = true;
    bool debugDraw = false;
    bool ground = true;
    std::uint32_t bodyCount = 0;
    std::uint32_t constraintCount = 0;
    float simulateMilliseconds = 0.0f; // GameThread time in PhysicsScene::Simulate this frame.
};
} // namespace MmdLab
