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

MMDLAB_TEST(Animation.SkeletonPose, TwoBoneIkPlacesAnkleAtTarget)
{
    using namespace DirectX;

    // A leg chain: thigh (0,0,0) -> knee (0,-1,0) -> ankle (0,-1,1), each link length 1, plus a
    // foot-IK control bone whose position is the ankle target.
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
    chain.links = { 1, 0 };    // knee, thigh (tip-to-root)
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

    std::vector<XMFLOAT4X4> palette;
    std::vector<XMMATRIX> world;
    MmdLab::EvaluateSkeletonPose(skeleton, bind, &motion, palette, world);

    // The ankle's world position must be the IK target.
    const XMVECTOR anklePos = world[2].r[3];
    MMDLAB_CHECK(std::fabs(XMVectorGetX(anklePos) - 0.0f) < 1e-4f);
    MMDLAB_CHECK(std::fabs(XMVectorGetY(anklePos) - (-0.5f)) < 1e-4f);
    MMDLAB_CHECK(std::fabs(XMVectorGetZ(anklePos) - 1.2f) < 1e-4f);

    // The thigh and shin lengths must be preserved by the solve.
    const XMVECTOR thighPos = world[0].r[3];
    const XMVECTOR kneePos = world[1].r[3];
    const float thighLen = XMVectorGetX(XMVector3Length(kneePos - thighPos));
    const float shinLen = XMVectorGetX(XMVector3Length(anklePos - kneePos));
    MMDLAB_CHECK(std::fabs(thighLen - 1.0f) < 1e-4f);
    MMDLAB_CHECK(std::fabs(shinLen - 1.0f) < 1e-4f);
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
    chain.links = { 0 };       // ankle (single link)
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
