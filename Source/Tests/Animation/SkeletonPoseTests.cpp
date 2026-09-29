#include "Runtime/Animation/SkeletonPose.h"
#include "Runtime/Core/TestFramework.h"

#include <DirectXMath.h>

#include <cmath>
#include <cstddef>
#include <utility>
#include <vector>

namespace
{
// A two-bone chain: a root at the origin pointing +Y, and a child at (0,1,0) also pointing +Y.
MmdLab::Skeleton MakeChainSkeleton()
{
    MmdLab::Skeleton skeleton;

    MmdLab::Bone root;
    root.name = "root";
    root.position[0] = 0.0f; root.position[1] = 0.0f; root.position[2] = 0.0f;
    root.tail[0] = 0.0f; root.tail[1] = 1.0f; root.tail[2] = 0.0f;
    root.parentIndex = MmdLab::kInvalidBoneIndex;
    skeleton.bones.push_back(std::move(root));

    MmdLab::Bone child;
    child.name = "child";
    child.position[0] = 0.0f; child.position[1] = 1.0f; child.position[2] = 0.0f;
    child.tail[0] = 0.0f; child.tail[1] = 2.0f; child.tail[2] = 0.0f;
    child.parentIndex = 0;
    skeleton.bones.push_back(std::move(child));

    skeleton.children.resize(2);
    skeleton.children[0].push_back(1);
    return skeleton;
}

bool IsIdentity(const DirectX::XMFLOAT4X4& matrix, const float epsilon = 1e-5f)
{
    for (int row = 0; row < 4; ++row)
    {
        for (int col = 0; col < 4; ++col)
        {
            const float expected = (row == col) ? 1.0f : 0.0f;
            if (std::fabs(matrix.m[row][col] - expected) > epsilon)
            {
                return false;
            }
        }
    }
    return true;
}

bool MatricesEqual(const DirectX::XMFLOAT4X4& a, const DirectX::XMFLOAT4X4& b, const float epsilon = 1e-4f)
{
    for (int row = 0; row < 4; ++row)
    {
        for (int col = 0; col < 4; ++col)
        {
            if (std::fabs(a.m[row][col] - b.m[row][col]) > epsilon)
            {
                return false;
            }
        }
    }
    return true;
}
} // namespace

MMDLAB_TEST(Animation.SkeletonPose, BindPosePaletteIsIdentity)
{
    const MmdLab::Skeleton skeleton = MakeChainSkeleton();
    const MmdLab::BindPose bind = MmdLab::BuildBindPose(skeleton);

    std::vector<DirectX::XMFLOAT4X4> palette;
    std::vector<DirectX::XMMATRIX> worldScratch;
    MmdLab::EvaluateSkeletonPose(skeleton, bind, nullptr, palette, worldScratch);

    MMDLAB_CHECK(palette.size() == skeleton.bones.size());
    for (const DirectX::XMFLOAT4X4& matrix : palette)
    {
        MMDLAB_CHECK(IsIdentity(matrix));
    }
}

MMDLAB_TEST(Animation.SkeletonPose, RotatedRootCancelsBindToRotation)
{
    const MmdLab::Skeleton skeleton = MakeChainSkeleton();
    const MmdLab::BindPose bind = MmdLab::BuildBindPose(skeleton);

    // Rotate the root bone a further 90 degrees about Z. The resulting root palette must equal
    // that extra rotation: inverseBind * (bindLocal * extraRotation) collapses to extraRotation.
    const DirectX::XMMATRIX extraRotation = DirectX::XMMatrixRotationZ(DirectX::XMConvertToRadians(90.0f));

    MmdLab::BonePose motion;
    motion.local.resize(skeleton.bones.size());
    for (std::size_t i = 0; i < skeleton.bones.size(); ++i)
    {
        motion.local[i] = bind.localBind[i];
    }
    const DirectX::XMMATRIX bindLocal = DirectX::XMLoadFloat4x4(&bind.localBind[0]);
    const DirectX::XMMATRIX movedLocal = DirectX::XMMatrixMultiply(bindLocal, extraRotation);
    DirectX::XMStoreFloat4x4(&motion.local[0], movedLocal);

    std::vector<DirectX::XMFLOAT4X4> palette;
    std::vector<DirectX::XMMATRIX> worldScratch;
    MmdLab::EvaluateSkeletonPose(skeleton, bind, &motion, palette, worldScratch);

    DirectX::XMFLOAT4X4 expected;
    DirectX::XMStoreFloat4x4(&expected, extraRotation);
    MMDLAB_CHECK(MatricesEqual(palette[0], expected));
}

MMDLAB_TEST(Animation.SkeletonPose, RotatedBonePivotsAboutItsHead)
{
    // A single bone whose head is NOT at the model origin. Rotating it must pivot its vertices
    // about the head, not the model origin (a translate/rotate order bug flings them outward).
    MmdLab::Skeleton skeleton;
    MmdLab::Bone bone;
    bone.name = "arm";
    bone.position[0] = 0.0f; bone.position[1] = 1.0f; bone.position[2] = 0.0f;
    bone.tail[0] = 0.0f; bone.tail[1] = 2.0f; bone.tail[2] = 0.0f;
    bone.parentIndex = MmdLab::kInvalidBoneIndex;
    skeleton.bones.push_back(std::move(bone));
    skeleton.children.resize(1);

    const MmdLab::BindPose bind = MmdLab::BuildBindPose(skeleton);

    // Animate the bone with a 90-degree rotation about Z, in the same R * T shape the VMD
    // animator produces (rotation pivots about the head, then translates to it).
    MmdLab::BonePose motion;
    motion.local.resize(1);
    const DirectX::XMMATRIX movedLocal = DirectX::XMMatrixMultiply(
        DirectX::XMMatrixRotationZ(DirectX::XMConvertToRadians(90.0f)),
        DirectX::XMMatrixTranslation(0.0f, 1.0f, 0.0f));
    DirectX::XMStoreFloat4x4(&motion.local[0], movedLocal);

    std::vector<DirectX::XMFLOAT4X4> palette;
    std::vector<DirectX::XMMATRIX> worldScratch;
    MmdLab::EvaluateSkeletonPose(skeleton, bind, &motion, palette, worldScratch);

    // The tail vertex (0,2,0) sits one unit from the head along +Y; after the rotation it must
    // still sit one unit from the head, not fling toward the model origin.
    const DirectX::XMVECTOR tail = DirectX::XMVectorSet(0.0f, 2.0f, 0.0f, 1.0f);
    const DirectX::XMVECTOR head = DirectX::XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);
    const DirectX::XMVECTOR skinned = DirectX::XMVector3Transform(
        tail, DirectX::XMLoadFloat4x4(&palette[0]));
    const float distance = DirectX::XMVectorGetX(
        DirectX::XMVector3Length(DirectX::XMVectorSubtract(skinned, head)));
    MMDLAB_CHECK(std::fabs(distance - 1.0f) < 1e-4f);
}

MMDLAB_TEST(Animation.SkeletonPose, LocalCoordinateAxesDefineBindRotation)
{
    // A bone with explicit local axes must use them for the bind rotation, not the head -> tail
    // direction. Here localX/localZ are the identity axes, so the bind rotation is identity even
    // though the tail points along +Y (whose default roll would be a non-identity rotation).
    MmdLab::Skeleton skeleton;
    MmdLab::Bone bone;
    bone.name = "eye";
    bone.position[0] = 0.0f; bone.position[1] = 0.0f; bone.position[2] = 0.0f;
    bone.tail[0] = 0.0f; bone.tail[1] = 1.0f; bone.tail[2] = 0.0f;
    bone.parentIndex = MmdLab::kInvalidBoneIndex;
    bone.hasLocalAxes = true;
    bone.localX[0] = 1.0f; bone.localX[1] = 0.0f; bone.localX[2] = 0.0f;
    bone.localZ[0] = 0.0f; bone.localZ[1] = 0.0f; bone.localZ[2] = 1.0f;
    skeleton.bones.push_back(std::move(bone));
    skeleton.children.resize(1);

    const MmdLab::BindPose bind = MmdLab::BuildBindPose(skeleton);

    // At the origin the bind local is the pure bind rotation (X = +X, Z = +Z, Y = Z x X = +Y).
    MMDLAB_CHECK(IsIdentity(bind.localBind[0]));
}

MMDLAB_TEST(Animation.SkeletonPose, LookAtIkAimsToeAtTarget)
{
    using namespace DirectX;

    // A foot chain: ankle at the origin pointing +Z, toe at (0,0,1) as its child, plus a toe-IK
    // control bone placed straight up so the LookAt must swing the foot from +Z to +Y.
    MmdLab::Skeleton skeleton;
    const auto AddBone = [&](const char* name, float px, float py, float pz, float tx, float ty, float tz,
        std::uint16_t parent)
    {
        MmdLab::Bone bone;
        bone.name = name;
        bone.position[0] = px; bone.position[1] = py; bone.position[2] = pz;
        bone.tail[0] = tx; bone.tail[1] = ty; bone.tail[2] = tz;
        bone.parentIndex = parent;
        skeleton.bones.push_back(std::move(bone));
    };
    AddBone("ankle", 0, 0, 0, 0, 0, 1, MmdLab::kInvalidBoneIndex);
    AddBone("toe", 0, 0, 1, 0, 0, 2, 0);
    AddBone("toe_ik", 0, 1, 0, 0, 1, 1, MmdLab::kInvalidBoneIndex);

    skeleton.children.resize(3);
    skeleton.children[0].push_back(1);

    MmdLab::IkChain chain;
    chain.ikBoneIndex = 2;
    chain.targetBoneIndex = 1; // toe
    chain.links = { { 0 } };   // ankle (single link)
    skeleton.ikChains.push_back(chain);

    const MmdLab::BindPose bind = MmdLab::BuildBindPose(skeleton);

    MmdLab::BonePose motion;
    motion.local.resize(3);
    motion.local[0] = bind.localBind[0];
    motion.local[1] = bind.localBind[1];
    XMStoreFloat4x4(&motion.local[2], XMMatrixTranslation(0.0f, 1.0f, 0.0f));

    std::vector<XMFLOAT4X4> palette;
    std::vector<XMMATRIX> world;
    MmdLab::EvaluateSkeletonPose(skeleton, bind, &motion, palette, world);

    // The foot (ankle -> toe) must now point along ankle -> target.
    const XMVECTOR footDir = XMVector3Normalize(world[1].r[3] - world[0].r[3]);
    const XMVECTOR targetDir = XMVector3Normalize(world[2].r[3] - world[0].r[3]);
    MMDLAB_CHECK(std::fabs(XMVectorGetX(footDir) - XMVectorGetX(targetDir)) < 1e-4f);
    MMDLAB_CHECK(std::fabs(XMVectorGetY(footDir) - XMVectorGetY(targetDir)) < 1e-4f);
    MMDLAB_CHECK(std::fabs(XMVectorGetZ(footDir) - XMVectorGetZ(targetDir)) < 1e-4f);
}

MMDLAB_TEST(Animation.SkeletonPose, DisabledIkKeepsAnkleAtBind)
{
    using namespace DirectX;

    // A thigh -> knee -> ankle chain whose IK is disabled via
    // `ikEnabled`, so the ankle must keep its bind position instead of reaching the IK target.
    MmdLab::Skeleton skeleton;
    const auto AddBone = [&](const char* name, float px, float py, float pz, float tx, float ty, float tz,
        std::uint16_t parent)
    {
        MmdLab::Bone bone;
        bone.name = name;
        bone.position[0] = px; bone.position[1] = py; bone.position[2] = pz;
        bone.tail[0] = tx; bone.tail[1] = ty; bone.tail[2] = tz;
        bone.parentIndex = parent;
        skeleton.bones.push_back(std::move(bone));
    };
    AddBone("thigh", 0, 0, 0, 0, -1, 0, MmdLab::kInvalidBoneIndex);
    AddBone("knee", 0, -1, 0, 0, -1, 1, 0);
    AddBone("ankle", 0, -1, 1, 0, -1, 2, 1);
    AddBone("foot_ik", 0, 0, 0, 0, 0, 1, MmdLab::kInvalidBoneIndex);

    skeleton.children.resize(4);
    skeleton.children[0].push_back(1);
    skeleton.children[1].push_back(2);

    MmdLab::IkChain chain;
    chain.ikBoneIndex = 3;
    chain.targetBoneIndex = 2; // ankle
    chain.links = { { 1 }, { 0 } };    // knee, thigh (tip-to-root)
    skeleton.ikChains.push_back(chain);

    const MmdLab::BindPose bind = MmdLab::BuildBindPose(skeleton);

    const XMVECTOR target = XMVectorSet(0.0f, -0.5f, 1.2f, 0.0f);

    MmdLab::BonePose motion;
    motion.local.resize(4);
    for (std::size_t i = 0; i < 3; ++i)
    {
        motion.local[i] = bind.localBind[i];
    }
    XMStoreFloat4x4(&motion.local[3], XMMatrixTranslationFromVector(target));

    const std::vector<bool> ikEnabled = { false };
    std::vector<XMFLOAT4X4> palette;
    std::vector<XMMATRIX> world;
    MmdLab::EvaluateSkeletonPose(skeleton, bind, &motion, palette, world, &ikEnabled);

    // The ankle keeps its bind position (0,-1,1), not the IK target (0,-0.5,1.2).
    const XMVECTOR anklePos = world[2].r[3];
    MMDLAB_CHECK(std::fabs(XMVectorGetX(anklePos) - 0.0f) < 1e-4f);
    MMDLAB_CHECK(std::fabs(XMVectorGetY(anklePos) - (-1.0f)) < 1e-4f);
    MMDLAB_CHECK(std::fabs(XMVectorGetZ(anklePos) - 1.0f) < 1e-4f);
}

namespace
{
// A standard MMD leg, facing -Z: thigh (0,10,0) -> knee (0,5,0) -> ankle (0,0,0), and a foot-IK
// control bone. The chain mirrors a real PMX leg: 40 iterations of at most 2 radians, and the knee
// limited to a negative X rotation (-180 .. -0.5 degrees) so it only bends forward.
struct MmdLeg
{
    MmdLab::Skeleton skeleton;
    MmdLab::BindPose bind;
};

MmdLeg MakeMmdLeg(const bool limitKnee = true)
{
    MmdLeg leg;
    MmdLab::Skeleton& skeleton = leg.skeleton;
    const auto AddBone = [&](const char* name, float px, float py, float pz, float tx, float ty, float tz,
        std::uint16_t parent)
    {
        MmdLab::Bone bone;
        bone.name = name;
        bone.position[0] = px; bone.position[1] = py; bone.position[2] = pz;
        bone.tail[0] = tx; bone.tail[1] = ty; bone.tail[2] = tz;
        bone.parentIndex = parent;
        skeleton.bones.push_back(std::move(bone));
    };
    AddBone("lower_body", 0, 11, 0, 0, 10, 0, MmdLab::kInvalidBoneIndex); // Tail down, like 下半身.
    AddBone("thigh", 0, 10, 0, 0, 5, 0, 0);
    AddBone("knee", 0, 5, 0, 0, 0, 0, 1);
    AddBone("ankle", 0, 0, 0, 0, 0, -1, 2);
    AddBone("foot_ik", 0, 0, 0, 0, 0, 1, MmdLab::kInvalidBoneIndex);

    skeleton.children.resize(5);
    skeleton.children[0].push_back(1);
    skeleton.children[1].push_back(2);
    skeleton.children[2].push_back(3);

    MmdLab::IkChain chain;
    chain.ikBoneIndex = 4;
    chain.targetBoneIndex = 3;
    chain.loopCount = 40;
    chain.limitAngle = 2.0f;
    MmdLab::IkLink knee;
    knee.boneIndex = 2;
    knee.hasLimit = limitKnee;
    knee.limitMin[0] = -DirectX::XM_PI;
    knee.limitMax[0] = DirectX::XMConvertToRadians(-0.5f);
    MmdLab::IkLink thigh;
    thigh.boneIndex = 1;
    chain.links = { knee, thigh };
    skeleton.ikChains.push_back(chain);

    leg.bind = MmdLab::BuildBindPose(skeleton);
    return leg;
}

// A motion pose with every bone at bind except the foot IK, moved to `target`, and optionally a
// VMD rotation on the lower body.
MmdLab::BonePose MakeLegMotion(const MmdLeg& leg, const DirectX::XMVECTOR target,
    const DirectX::XMMATRIX& lowerBodyVmd = DirectX::XMMatrixIdentity())
{
    using namespace DirectX;
    MmdLab::BonePose motion;
    motion.local = leg.bind.localBind;
    const XMMATRIX bindRotation = XMLoadFloat4x4(&leg.bind.bindRotation[0]);
    XMStoreFloat4x4(&motion.local[0], XMMatrixMultiply(
        XMMatrixMultiply(bindRotation, lowerBodyVmd), XMMatrixTranslation(0.0f, 11.0f, 0.0f)));
    XMStoreFloat4x4(&motion.local[4], XMMatrixTranslationFromVector(target));
    return motion;
}

std::vector<DirectX::XMMATRIX> SolveLeg(const MmdLeg& leg, const MmdLab::BonePose& motion)
{
    std::vector<DirectX::XMFLOAT4X4> palette;
    std::vector<DirectX::XMMATRIX> world;
    MmdLab::EvaluateSkeletonPose(leg.skeleton, leg.bind, &motion, palette, world);
    return world;
}

// The knee's MMD-frame rotation relative to the thigh (bind rotations removed), i.e. the local
// rotation the IK solve gave the knee.
DirectX::XMMATRIX KneeLocalRotation(const MmdLeg& leg, const std::vector<DirectX::XMMATRIX>& world)
{
    using namespace DirectX;
    const XMMATRIX thighMmd = XMMatrixMultiply(
        XMMatrixTranspose(XMLoadFloat4x4(&leg.bind.bindRotation[1])), world[1]);
    const XMMATRIX kneeMmd = XMMatrixMultiply(
        XMMatrixTranspose(XMLoadFloat4x4(&leg.bind.bindRotation[2])), world[2]);
    XMMATRIX relative = XMMatrixMultiply(kneeMmd, XMMatrixInverse(nullptr, thighMmd));
    relative.r[3] = XMVectorSet(0.0f, 0.0f, 0.0f, 1.0f);
    return relative;
}

float Distance(const DirectX::XMVECTOR a, const DirectX::XMVECTOR b)
{
    return DirectX::XMVectorGetX(DirectX::XMVector3Length(DirectX::XMVectorSubtract(a, b)));
}
} // namespace

MMDLAB_TEST(Animation.SkeletonPose, IkPlacesAnkleAtReachableTarget)
{
    using namespace DirectX;
    const MmdLeg leg = MakeMmdLeg();
    const XMVECTOR target = XMVectorSet(0.0f, 3.0f, -2.0f, 0.0f);
    const std::vector<XMMATRIX> world = SolveLeg(leg, MakeLegMotion(leg, target));

    MMDLAB_CHECK(Distance(world[3].r[3], target) < 1e-2f);
    // Bone lengths are preserved: IK only rotates.
    MMDLAB_CHECK(std::fabs(Distance(world[2].r[3], world[1].r[3]) - 5.0f) < 1e-4f);
    MMDLAB_CHECK(std::fabs(Distance(world[3].r[3], world[2].r[3]) - 5.0f) < 1e-4f);
    MMDLAB_CHECK(Distance(world[1].r[3], XMVectorSet(0.0f, 10.0f, 0.0f, 0.0f)) < 1e-4f);
}

MMDLAB_TEST(Animation.SkeletonPose, IkKneeBendsForward)
{
    using namespace DirectX;
    // Lift the foot straight up under the hip: the leg must fold with the knee in front (-Z),
    // whatever side the target is on.
    const MmdLeg leg = MakeMmdLeg();
    for (const float z : { 0.0f, 1.0f, -1.0f })
    {
        const std::vector<XMMATRIX> world = SolveLeg(leg, MakeLegMotion(leg, XMVectorSet(0.0f, 4.0f, z, 0.0f)));
        MMDLAB_CHECK(XMVectorGetZ(world[2].r[3]) < -1.0f);
        MMDLAB_CHECK(std::fabs(XMVectorGetX(world[2].r[3])) < 1e-3f);
    }
}

MMDLAB_TEST(Animation.SkeletonPose, IkKneeRotatesOnlyAboutItsLimitAxis)
{
    using namespace DirectX;
    // A target off to the side: the thigh swings outward, but the knee's local rotation must stay
    // a pure negative-X hinge (no twist or side bend) within its limit.
    const MmdLeg leg = MakeMmdLeg();
    const std::vector<XMMATRIX> world = SolveLeg(leg, MakeLegMotion(leg, XMVectorSet(2.0f, 3.0f, -1.0f, 0.0f)));

    XMFLOAT4X4 knee;
    XMStoreFloat4x4(&knee, KneeLocalRotation(leg, world));
    // A rotation about X leaves the X axis fixed.
    MMDLAB_CHECK(std::fabs(knee.m[0][0] - 1.0f) < 1e-4f);
    MMDLAB_CHECK(std::fabs(knee.m[0][1]) < 1e-4f);
    MMDLAB_CHECK(std::fabs(knee.m[0][2]) < 1e-4f);
    // Row-vector RotationX(a): m[1][2] = sin(a); the limit is negative.
    const float angle = std::atan2(knee.m[1][2], knee.m[1][1]);
    MMDLAB_CHECK(angle < 0.0f);
    MMDLAB_CHECK(angle >= -XM_PI - 1e-4f);
}

MMDLAB_TEST(Animation.SkeletonPose, IkKneeLimitKeepsStraightLegAtMaxReach)
{
    using namespace DirectX;
    // Beyond reach the leg straightens toward the target, but the knee limit (max -0.5 degrees)
    // keeps it from locking fully straight or bending backward.
    const MmdLeg leg = MakeMmdLeg();
    const std::vector<XMMATRIX> world = SolveLeg(leg, MakeLegMotion(leg, XMVectorSet(0.0f, -5.0f, -1.0f, 0.0f)));

    XMFLOAT4X4 knee;
    XMStoreFloat4x4(&knee, KneeLocalRotation(leg, world));
    const float angle = std::atan2(knee.m[1][2], knee.m[1][1]);
    MMDLAB_CHECK(angle <= XMConvertToRadians(-0.5f) + 1e-4f);
    MMDLAB_CHECK(angle > XMConvertToRadians(-5.0f));
}

MMDLAB_TEST(Animation.SkeletonPose, IkKneeFollowsBodyRotation)
{
    using namespace DirectX;
    // Turn the lower body 90 degrees about +Y: the character now faces -X, and the IK solve must
    // bend the knee toward the new front instead of keeping the bind pose's -Z.
    const MmdLeg leg = MakeMmdLeg();
    const XMMATRIX turn = XMMatrixRotationY(XM_PIDIV2);
    const XMVECTOR front = XMVector3TransformNormal(XMVectorSet(0.0f, 0.0f, -1.0f, 0.0f), turn);
    const std::vector<XMMATRIX> world = SolveLeg(leg, MakeLegMotion(leg, XMVectorSet(0.0f, 4.0f, 0.0f, 0.0f), turn));

    const XMVECTOR kneeOffset = world[2].r[3] - world[1].r[3];
    MMDLAB_CHECK(XMVectorGetX(XMVector3Dot(kneeOffset, front)) > 1.0f);
    MMDLAB_CHECK(Distance(world[3].r[3], XMVectorSet(0.0f, 4.0f, 0.0f, 0.0f)) < 1e-2f);
}

MMDLAB_TEST(Animation.SkeletonPose, IkWithoutLimitStillReachesTarget)
{
    using namespace DirectX;
    // Chains without link limits (hair, skirts, props) take the general CCD path.
    const MmdLeg leg = MakeMmdLeg(false);
    const XMVECTOR target = XMVectorSet(1.0f, 2.0f, -3.0f, 0.0f);
    const std::vector<XMMATRIX> world = SolveLeg(leg, MakeLegMotion(leg, target));
    MMDLAB_CHECK(Distance(world[3].r[3], target) < 1e-2f);
}

namespace
{
// Three root bones at the origin pointing +Y: a source animated by `rotation`, and two grant
// bones that each copy the rotation of the bone named by `inheritFrom` (influence 1).
struct GrantRig
{
    MmdLab::Skeleton skeleton;
    MmdLab::BindPose bind;
};

GrantRig MakeGrantRig(const std::uint16_t firstInheritsFrom, const std::uint16_t secondInheritsFrom,
    const std::int32_t firstLayer, const bool secondAfterPhysics)
{
    GrantRig rig;
    for (int i = 0; i < 3; ++i)
    {
        MmdLab::Bone bone;
        bone.name = i == 0 ? "grantA" : (i == 1 ? "grantB" : "source");
        bone.tail[1] = 1.0f;
        rig.skeleton.bones.push_back(std::move(bone));
    }
    rig.skeleton.bones[0].hasInheritRotation = true;
    rig.skeleton.bones[0].inheritParentIndex = firstInheritsFrom;
    rig.skeleton.bones[0].inheritInfluence = 1.0f;
    rig.skeleton.bones[0].deformLayer = firstLayer;
    rig.skeleton.bones[1].hasInheritRotation = true;
    rig.skeleton.bones[1].inheritParentIndex = secondInheritsFrom;
    rig.skeleton.bones[1].inheritInfluence = 1.0f;
    rig.skeleton.bones[1].afterPhysics = secondAfterPhysics;
    rig.skeleton.children.resize(3);
    MmdLab::BuildDeformOrder(rig.skeleton);
    rig.bind = MmdLab::BuildBindPose(rig.skeleton);
    return rig;
}

MmdLab::BonePose PoseSource(const GrantRig& rig, const DirectX::XMMATRIX& rotation)
{
    MmdLab::BonePose motion;
    motion.local.assign(rig.bind.localBind.begin(), rig.bind.localBind.end());
    DirectX::XMStoreFloat4x4(&motion.local[2], DirectX::XMMatrixMultiply(
        DirectX::XMLoadFloat4x4(&rig.bind.bindRotation[2]), rotation));
    return motion;
}

// The model-space +Y direction of a bone's world transform (its bind +Y is the model +Y here).
DirectX::XMVECTOR BoneUp(const DirectX::XMMATRIX& world)
{
    return DirectX::XMVector3Normalize(DirectX::XMVector3TransformNormal(
        DirectX::XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f),
        world));
}
} // namespace

MMDLAB_TEST(Animation.SkeletonPose, HigherDeformLayerGrantsAfterItsSource)
{
    using namespace DirectX;
    // grantA (index 0) copies grantB (index 1), which copies the source. In index order grantA
    // would read grantB before grantB received its grant; on deform layer 1 it runs after it.
    const XMMATRIX rotation = XMMatrixRotationZ(XM_PIDIV2);
    for (const std::int32_t layer : { 0, 1 })
    {
        const GrantRig rig = MakeGrantRig(1, 2, layer, false);
        const MmdLab::BonePose motion = PoseSource(rig, rotation);
        std::vector<XMMATRIX> world;
        MmdLab::EvaluateBoneWorld(rig.skeleton, rig.bind, &motion, world);

        const XMVECTOR expected = XMVector3TransformNormal(XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f), rotation);
        const float grantAError = XMVectorGetX(XMVector3Length(BoneUp(world[0]) - expected));
        MMDLAB_CHECK(XMVectorGetX(XMVector3Length(BoneUp(world[1]) - expected)) < 1e-4f);
        MMDLAB_CHECK(layer == 1 ? grantAError < 1e-4f : grantAError > 0.5f);
    }
}

MMDLAB_TEST(Animation.SkeletonPose, AfterPhysicsGrantFollowsTheSimulatedSource)
{
    using namespace DirectX;
    // grantB is PhysicsAfterDeform and copies the source. Physics then turns the source a further
    // 90 degrees: only the after-physics pass lets grantB pick that up.
    const GrantRig rig = MakeGrantRig(2, 2, 0, true);
    const MmdLab::BonePose motion = PoseSource(rig, XMMatrixIdentity());
    std::vector<XMMATRIX> world;
    std::vector<XMMATRIX> local;
    MmdLab::EvaluateBoneWorld(rig.skeleton, rig.bind, &motion, world, local);

    const XMMATRIX simulated = XMMatrixRotationX(XM_PIDIV2);
    world[2] = XMMatrixMultiply(world[2], simulated); // What PhysicsScene would write back.
    const XMVECTOR expected = XMVector3TransformNormal(XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f), simulated);
    MMDLAB_CHECK(XMVectorGetX(XMVector3Length(BoneUp(world[1]) - expected)) > 0.5f);

    MmdLab::EvaluateBoneWorldAfterPhysics(rig.skeleton, rig.bind, local, world);
    MMDLAB_CHECK(XMVectorGetX(XMVector3Length(BoneUp(world[1]) - expected)) < 1e-4f);
    // grantA inherits too but is evaluated before physics, so it keeps the animated source.
    MMDLAB_CHECK(XMVectorGetX(XMVector3Length(BoneUp(world[0]) - XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f))) < 1e-4f);
}
