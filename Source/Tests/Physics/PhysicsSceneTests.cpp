#include "Runtime/Animation/SkeletonPose.h"
#include "Runtime/Asset/PhysicsAsset.h"
#include "Runtime/Core/TestFramework.h"
#include "Runtime/Physics/PhysicsScene.h"

#include <DirectXMath.h>

#include <cmath>
#include <utility>
#include <vector>

namespace
{
using namespace DirectX;

// A vertical three-bone chain along +Y: root (0,10,0) -> hang (0,8,0) -> tip (0,6,0), pointing
// down, like a strand of hair hanging from the head.
MmdLab::Skeleton MakeHangingChain()
{
    MmdLab::Skeleton skeleton;
    const float heads[3] = { 10.0f, 8.0f, 6.0f };
    for (int i = 0; i < 3; ++i)
    {
        MmdLab::Bone bone;
        bone.name = i == 0 ? "root" : (i == 1 ? "hang" : "tip");
        bone.position[1] = heads[i];
        bone.tail[1] = heads[i] - 2.0f;
        bone.parentIndex = i == 0 ? MmdLab::kInvalidBoneIndex : static_cast<std::uint16_t>(i - 1);
        skeleton.bones.push_back(std::move(bone));
    }
    skeleton.children = { { 1 }, { 2 }, {} };
    return skeleton;
}

MmdLab::BodySetup MakeSphere(const std::uint16_t bone, const MmdLab::BodyMode mode, const float y)
{
    MmdLab::BodySetup body;
    body.boneIndex = bone;
    body.mode = mode;
    body.shape = MmdLab::BodyShape::Sphere;
    body.size[0] = 0.5f;
    body.position[1] = y;
    body.mass = 1.0f;
    body.group = 0;
    body.collisionMask = 0; // Collide with nothing unless a test opts in.
    return body;
}

float WorldY(const XMMATRIX& matrix)
{
    return XMVectorGetY(matrix.r[3]);
}

// Steps `seconds` at 60 Hz with the bind pose as the animated pose, returning the final world.
std::vector<XMMATRIX> Run(MmdLab::PhysicsScene& scene, const MmdLab::Skeleton& skeleton,
    const MmdLab::BindPose& bind, const float seconds)
{
    std::vector<XMMATRIX> world;
    const int frames = static_cast<int>(seconds * 60.0f);
    for (int frame = 0; frame < frames; ++frame)
    {
        MmdLab::EvaluateBoneWorld(skeleton, bind, nullptr, world);
        scene.Simulate(1.0f / 60.0f, world);
    }
    return world;
}
} // namespace

MMDLAB_TEST(Physics.Scene, FollowBoneBodyTracksItsBone)
{
    const MmdLab::Skeleton skeleton = MakeHangingChain();
    const MmdLab::BindPose bind = MmdLab::BuildBindPose(skeleton);
    MmdLab::PhysicsAsset asset;
    asset.bodies = { MakeSphere(0, MmdLab::BodyMode::FollowBone, 10.5f) };
    MmdLab::PhysicsScene scene(asset, skeleton, bind);

    // Move the root up by 3 through the animated pose: the kinematic body follows it rigidly,
    // keeping its bind offset (0.5 above the bone head), and gravity does not act on it.
    std::vector<XMMATRIX> world;
    MmdLab::EvaluateBoneWorld(skeleton, bind, nullptr, world);
    scene.Simulate(1.0f / 60.0f, world);
    for (int frame = 0; frame < 30; ++frame)
    {
        MmdLab::EvaluateBoneWorld(skeleton, bind, nullptr, world);
        world[0].r[3] = XMVectorSet(0.0f, 13.0f, 0.0f, 1.0f);
        scene.Simulate(1.0f / 60.0f, world);
    }
    MMDLAB_CHECK(std::fabs(WorldY(scene.BodyWorld(0)) - 13.5f) < 1e-3f);
    MMDLAB_CHECK(std::fabs(WorldY(world[0]) - 13.0f) < 1e-5f); // The bone is not overridden.
}

MMDLAB_TEST(Physics.Scene, SimulatedBodyFallsAndDrivesItsBoneAndChildren)
{
    const MmdLab::Skeleton skeleton = MakeHangingChain();
    const MmdLab::BindPose bind = MmdLab::BuildBindPose(skeleton);
    MmdLab::PhysicsAsset asset;
    MmdLab::BodySetup body = MakeSphere(1, MmdLab::BodyMode::Physics, 7.0f);
    asset.bodies = { body };
    MmdLab::PhysicsScene scene(asset, skeleton, bind);
    scene.SetGroundEnabled(false);

    // Free fall with MMD gravity (98 units/s^2) for 0.5 s: about 12.25 units down, less a little
    // for Bullet's default damping-free integration error.
    const std::vector<XMMATRIX> world = Run(scene, skeleton, bind, 0.5f);
    const float drop = 7.0f - WorldY(scene.BodyWorld(0));
    MMDLAB_CHECK(drop > 11.0f && drop < 13.0f);

    // The bone keeps its bind offset to the body, and the unsimulated child follows the bone.
    MMDLAB_CHECK(std::fabs((WorldY(scene.BodyWorld(0)) - WorldY(world[1])) - (7.0f - 8.0f)) < 1e-3f);
    MMDLAB_CHECK(std::fabs((WorldY(world[1]) - WorldY(world[2])) - 2.0f) < 1e-3f);
    MMDLAB_CHECK(std::fabs(WorldY(world[0]) - 10.0f) < 1e-5f); // The root is untouched.
}

MMDLAB_TEST(Physics.Scene, JointHoldsASimulatedBodyUnderAKinematicOne)
{
    const MmdLab::Skeleton skeleton = MakeHangingChain();
    const MmdLab::BindPose bind = MmdLab::BuildBindPose(skeleton);
    MmdLab::PhysicsAsset asset;
    asset.bodies = {
        MakeSphere(0, MmdLab::BodyMode::FollowBone, 10.0f),
        MakeSphere(1, MmdLab::BodyMode::Physics, 8.0f),
    };
    MmdLab::ConstraintSetup joint;
    joint.bodyA = 0;
    joint.bodyB = 1;
    joint.position[1] = 9.0f; // A locked joint midway between them.
    asset.constraints = { joint };
    MmdLab::PhysicsScene scene(asset, skeleton, bind);

    const std::vector<XMMATRIX> world = Run(scene, skeleton, bind, 1.0f);
    MMDLAB_CHECK(std::fabs(WorldY(scene.BodyWorld(1)) - 8.0f) < 0.05f);
    MMDLAB_CHECK(std::fabs(WorldY(world[1]) - 8.0f) < 0.05f);
}

MMDLAB_TEST(Physics.Scene, CollisionMaskDecidesWhetherBodiesCollide)
{
    const MmdLab::Skeleton skeleton = MakeHangingChain();
    const MmdLab::BindPose bind = MmdLab::BuildBindPose(skeleton);

    // A kinematic floor box (group 1) under the root, and a falling sphere (group 2) above it.
    const auto fallOnto = [&](const std::uint16_t sphereMask)
    {
        MmdLab::PhysicsAsset asset;
        MmdLab::BodySetup floor = MakeSphere(0, MmdLab::BodyMode::FollowBone, 0.0f);
        floor.shape = MmdLab::BodyShape::Box;
        floor.size[0] = 5.0f; floor.size[1] = 0.5f; floor.size[2] = 5.0f;
        floor.group = 1;
        floor.collisionMask = 0xFFFF;
        MmdLab::BodySetup ball = MakeSphere(1, MmdLab::BodyMode::Physics, 3.0f);
        ball.group = 2;
        ball.collisionMask = sphereMask;
        asset.bodies = { floor, ball };
        MmdLab::PhysicsScene scene(asset, skeleton, bind);
        scene.SetGroundEnabled(false);
        (void)Run(scene, skeleton, bind, 1.5f);
        return WorldY(scene.BodyWorld(1));
    };

    // Resting on the box top (0.5) plus the radius (0.5), or fallen far through it.
    MMDLAB_CHECK(std::fabs(fallOnto(1u << 1) - 1.0f) < 0.1f);
    MMDLAB_CHECK(fallOnto(0xFFFF & ~(1u << 1)) < -50.0f);
}

MMDLAB_TEST(Physics.Scene, ResetReturnsBodiesToThePoseAtRest)
{
    const MmdLab::Skeleton skeleton = MakeHangingChain();
    const MmdLab::BindPose bind = MmdLab::BuildBindPose(skeleton);
    MmdLab::PhysicsAsset asset;
    asset.bodies = { MakeSphere(1, MmdLab::BodyMode::Physics, 7.0f) };
    MmdLab::PhysicsScene scene(asset, skeleton, bind);
    (void)Run(scene, skeleton, bind, 0.5f);

    std::vector<XMMATRIX> world;
    MmdLab::EvaluateBoneWorld(skeleton, bind, nullptr, world);
    scene.Reset(world);
    MMDLAB_CHECK(std::fabs(WorldY(scene.BodyWorld(0)) - 7.0f) < 1e-4f);

    // With its velocity cleared it starts falling from rest again: one 60 Hz frame of free fall
    // is under 0.1 units.
    scene.Simulate(1.0f / 60.0f, world);
    MMDLAB_CHECK(7.0f - WorldY(scene.BodyWorld(0)) < 0.1f);
}

MMDLAB_TEST(Physics.Scene, BonePositionModeKeepsTheAnimatedPosition)
{
    const MmdLab::Skeleton skeleton = MakeHangingChain();
    const MmdLab::BindPose bind = MmdLab::BuildBindPose(skeleton);
    MmdLab::PhysicsAsset asset;
    asset.bodies = { MakeSphere(1, MmdLab::BodyMode::PhysicsWithBonePosition, 7.0f) };
    MmdLab::PhysicsScene scene(asset, skeleton, bind);
    scene.SetGroundEnabled(false);

    // The body falls away, but the bone only takes its rotation (none here), so it stays put.
    const std::vector<XMMATRIX> world = Run(scene, skeleton, bind, 0.5f);
    MMDLAB_CHECK(WorldY(scene.BodyWorld(0)) < 0.0f);
    MMDLAB_CHECK(std::fabs(WorldY(world[1]) - 8.0f) < 1e-4f);
}

MMDLAB_TEST(Physics.Scene, GroundCatchesBodiesWhateverTheirMask)
{
    const MmdLab::Skeleton skeleton = MakeHangingChain();
    const MmdLab::BindPose bind = MmdLab::BuildBindPose(skeleton);
    MmdLab::PhysicsAsset asset;
    asset.bodies = { MakeSphere(1, MmdLab::BodyMode::Physics, 3.0f) }; // Mask 0: collides with no group.

    MmdLab::PhysicsScene scene(asset, skeleton, bind);
    MMDLAB_CHECK(scene.GroundEnabled());
    (void)Run(scene, skeleton, bind, 1.5f);
    MMDLAB_CHECK(std::fabs(WorldY(scene.BodyWorld(0)) - 0.5f) < 0.1f); // Resting at its radius.

    MmdLab::PhysicsScene noGround(asset, skeleton, bind);
    noGround.SetGroundEnabled(false);
    (void)Run(noGround, skeleton, bind, 1.5f);
    MMDLAB_CHECK(WorldY(noGround.BodyWorld(0)) < -50.0f);
}

MMDLAB_TEST(Physics.Scene, KinematicBodiesUnderTheGroundDoNotCollideWithIt)
{
    // A follow-bone body at y = 0 overlaps the ground and another follow-bone body; neither pair
    // can respond, so they are filtered out and the body keeps tracking its bone exactly.
    const MmdLab::Skeleton skeleton = MakeHangingChain();
    const MmdLab::BindPose bind = MmdLab::BuildBindPose(skeleton);
    MmdLab::PhysicsAsset asset;
    MmdLab::BodySetup a = MakeSphere(0, MmdLab::BodyMode::FollowBone, 0.0f);
    a.collisionMask = 0xFFFF;
    MmdLab::BodySetup b = a;
    b.boneIndex = 1;
    asset.bodies = { a, b };
    MmdLab::PhysicsScene scene(asset, skeleton, bind);
    (void)Run(scene, skeleton, bind, 0.5f);
    MMDLAB_CHECK(std::fabs(WorldY(scene.BodyWorld(0))) < 1e-4f);
    MMDLAB_CHECK(std::fabs(WorldY(scene.BodyWorld(1))) < 1e-4f);
}
