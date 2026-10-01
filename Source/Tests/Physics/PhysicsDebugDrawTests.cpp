#include "Runtime/Animation/SkeletonPose.h"
#include "Runtime/Asset/PhysicsAsset.h"
#include "Runtime/Core/TestFramework.h"
#include "Runtime/Physics/PhysicsScene.h"
#include "Runtime/Scene/PhysicsDebugDraw.h"

#include <DirectXMath.h>

#include <cmath>
#include <utility>
#include <vector>

namespace
{
using namespace DirectX;

MmdLab::Skeleton MakeSingleBone()
{
    MmdLab::Skeleton skeleton;
    MmdLab::Bone bone;
    bone.name = "root";
    bone.tail[1] = 1.0f;
    skeleton.bones.push_back(std::move(bone));
    MmdLab::BuildChildren(skeleton);
    return skeleton;
}
} // namespace

MMDLAB_TEST(Physics.DebugDraw, AppendsEveryBodyWithItsShapeModeAndTransform)
{
    const MmdLab::Skeleton skeleton = MakeSingleBone();
    const MmdLab::BindPose bind = MmdLab::BuildBindPose(skeleton);
    MmdLab::PhysicsAsset asset;
    MmdLab::BodySetup sphere;
    sphere.boneIndex = 0;
    sphere.shape = MmdLab::BodyShape::Sphere;
    sphere.mode = MmdLab::BodyMode::FollowBone;
    sphere.size[0] = 0.5f;
    sphere.position[1] = 2.0f;
    MmdLab::BodySetup box = sphere;
    box.shape = MmdLab::BodyShape::Box;
    box.mode = MmdLab::BodyMode::PhysicsWithBonePosition;
    box.mass = 1.0f;
    box.size[1] = 0.25f;
    box.size[2] = 0.75f;
    box.position[0] = 3.0f;
    asset.bodies = { sphere, box };

    const MmdLab::PhysicsScene scene(asset, skeleton, bind);
    std::vector<MmdLab::PhysicsDebugBody> bodies(scene.BodyCount());
    scene.WriteDebugBodies(bodies);

    MMDLAB_CHECK_EQUAL(static_cast<std::size_t>(2), bodies.size());
    MMDLAB_CHECK(bodies[0].shape == MmdLab::BodyShape::Sphere);
    MMDLAB_CHECK(bodies[0].mode == MmdLab::BodyMode::FollowBone);
    MMDLAB_CHECK(std::fabs(bodies[0].world.m[3][1] - 2.0f) < 1e-5f);
    MMDLAB_CHECK(bodies[1].shape == MmdLab::BodyShape::Box);
    MMDLAB_CHECK(bodies[1].mode == MmdLab::BodyMode::PhysicsWithBonePosition);
    MMDLAB_CHECK(std::fabs(bodies[1].world.m[3][0] - 3.0f) < 1e-5f);
    MMDLAB_CHECK_EQUAL(0.75f, bodies[1].size[2]);
}

MMDLAB_TEST(Physics.DebugDraw, ProjectsBodiesToTheScreen)
{
    // An orthographic-free check: a unit box at the origin seen by an identity "clip" transform
    // lands its corners at the viewport edges ((-1..1) NDC -> (0..width) pixels, y flipped).
    MmdLab::PhysicsDebugBody box;
    XMStoreFloat4x4(&box.world, XMMatrixIdentity());
    box.shape = MmdLab::BodyShape::Box;
    box.size[0] = 1.0f; box.size[1] = 1.0f; box.size[2] = 0.5f;
    std::vector<MmdLab::DebugLine2D> lines;
    MmdLab::AppendBodyWireframe(box, XMMatrixIdentity(), 200.0f, 100.0f, lines);
    MMDLAB_CHECK_EQUAL(static_cast<std::size_t>(12), lines.size());
    for (const MmdLab::DebugLine2D& line : lines)
    {
        for (const float x : { line.x0, line.x1 })
        {
            MMDLAB_CHECK(std::fabs(x) < 1e-3f || std::fabs(x - 200.0f) < 1e-3f);
        }
        for (const float y : { line.y0, line.y1 })
        {
            MMDLAB_CHECK(std::fabs(y) < 1e-3f || std::fabs(y - 100.0f) < 1e-3f);
        }
    }

    // A sphere draws three 16-segment rings, and a capsule its rings, sides, and cap arcs.
    MmdLab::PhysicsDebugBody sphere = box;
    sphere.shape = MmdLab::BodyShape::Sphere;
    lines.clear();
    MmdLab::AppendBodyWireframe(sphere, XMMatrixIdentity(), 200.0f, 100.0f, lines);
    MMDLAB_CHECK_EQUAL(static_cast<std::size_t>(48), lines.size());
    MmdLab::PhysicsDebugBody capsule = box;
    capsule.shape = MmdLab::BodyShape::Capsule;
    lines.clear();
    MmdLab::AppendBodyWireframe(capsule, XMMatrixIdentity(), 200.0f, 100.0f, lines);
    MMDLAB_CHECK_EQUAL(static_cast<std::size_t>(2 * 16 + 4 + 4 * 8), lines.size());
}

MMDLAB_TEST(Physics.DebugDraw, DropsSegmentsBehindTheCamera)
{
    // A perspective projection looking down +Z: a box entirely behind the eye (z < 0) draws nothing.
    MmdLab::PhysicsDebugBody box;
    XMStoreFloat4x4(&box.world, XMMatrixTranslation(0.0f, 0.0f, -10.0f));
    box.shape = MmdLab::BodyShape::Box;
    box.size[0] = 1.0f; box.size[1] = 1.0f; box.size[2] = 1.0f;
    const XMMATRIX projection = XMMatrixPerspectiveFovLH(XM_PIDIV4, 2.0f, 0.1f, 100.0f);
    std::vector<MmdLab::DebugLine2D> lines;
    MmdLab::AppendBodyWireframe(box, projection, 200.0f, 100.0f, lines);
    MMDLAB_CHECK(lines.empty());

    XMStoreFloat4x4(&box.world, XMMatrixTranslation(0.0f, 0.0f, 10.0f));
    MmdLab::AppendBodyWireframe(box, projection, 200.0f, 100.0f, lines);
    MMDLAB_CHECK_EQUAL(static_cast<std::size_t>(12), lines.size());
}
