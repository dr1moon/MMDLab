#pragma once

#include "Runtime/Animation/SkeletonPose.h"
#include "Runtime/Asset/PhysicsAsset.h"
#include "Runtime/Asset/Skeleton.h"

#include <DirectXMath.h>

#include <memory>
#include <vector>

namespace MmdLab
{
// The rigid-body simulation of one model instance, backed by a private Bullet world. Owned by
// the GameThread; persistent across frames (bodies carry velocity from frame to frame).
//
// Each frame runs between skeleton evaluation and skinning, in the standard MMD order:
//   1. FollowBone bodies are moved to their animated bone (kinematic, so they push the others).
//   2. The world steps at a fixed 120 Hz.
//   3. Simulated bodies write their transform back to their bone (PhysicsWithBonePosition keeps
//      the bone's animated position), and every descendant of a simulated bone follows it with
//      its animated parent-relative transform.
class PhysicsScene final
{
public:
    // Builds the bodies and joints at bind pose. The skeleton and bind pose must outlive the
    // scene (they belong to the immutable Model).
    PhysicsScene(const PhysicsAsset& asset, const Skeleton& skeleton, const BindPose& bindPose);
    ~PhysicsScene();

    PhysicsScene(const PhysicsScene&) = delete;
    PhysicsScene& operator=(const PhysicsScene&) = delete;

    [[nodiscard]] std::size_t BodyCount() const;

    // The static floor plane at y = 0 that every simulated body collides with (on by default).
    void SetGroundEnabled(bool enabled);
    [[nodiscard]] bool GroundEnabled() const;

    // Teleports every body to its bone in `world` with zero velocity. Call when the pose jumps
    // (a new motion, a seek) so the simulation does not see a huge velocity and explode. The
    // first Simulate also resets.
    void Reset(const std::vector<DirectX::XMMATRIX>& world);

    // Advances the simulation by `deltaSeconds` and rewrites the simulated bones in `world`,
    // which holds the animated world transforms on entry (EvaluateBoneWorld).
    void Simulate(float deltaSeconds, std::vector<DirectX::XMMATRIX>& world);

    // The current model-space transform of body `index` (for tests and debug drawing).
    [[nodiscard]] DirectX::XMMATRIX BodyWorld(std::size_t index) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace MmdLab
