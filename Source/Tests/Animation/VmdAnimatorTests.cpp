#include "Runtime/Animation/VmdAnimator.h"
#include "Runtime/Core/TestFramework.h"

#include <DirectXMath.h>

#include <cmath>
#include <cstddef>
#include <utility>
#include <vector>

namespace
{
// A root bone at the origin pointing +Y, and a child at (0,1,0) also pointing +Y.
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

MMDLAB_TEST(Animation.VmdAnimator, SamplesTrackAndFallsBackToBind)
{
    // A single track for "root" holding a 90-degree rotation about Z at frame 0.
    MmdLab::VmdMotion motion;
    motion.modelName = "test";
    MmdLab::VmdBoneTrack track;
    track.boneName = "root";
    MmdLab::VmdBoneKey key;
    key.frame = 0;
    const float halfRootTwo = 0.70710678f;
    key.rotation[0] = 0.0f; key.rotation[1] = 0.0f;
    key.rotation[2] = halfRootTwo; key.rotation[3] = halfRootTwo; // 90 deg about +Z.
    track.keys.push_back(key);
    motion.boneTracks.push_back(std::move(track));

    MmdLab::VmdAnimator animator;
    animator.SetMotion(std::move(motion));

    const MmdLab::Skeleton skeleton = MakeChainSkeleton();
    const MmdLab::BindPose bind = MmdLab::BuildBindPose(skeleton);

    MmdLab::BonePose pose;
    animator.SamplePose(skeleton, bind, pose);
    MMDLAB_CHECK(pose.local.size() == skeleton.bones.size());

    // The root's local is R_vmd * bindLocal * T(0): the VMD rotation composed with the bind
    // rotation, applied in the bone's local frame.
    const DirectX::XMMATRIX expectedLocal = DirectX::XMMatrixMultiply(
        DirectX::XMMatrixRotationZ(DirectX::XMConvertToRadians(90.0f)),
        DirectX::XMLoadFloat4x4(&bind.localBind[0]));
    DirectX::XMFLOAT4X4 expected;
    DirectX::XMStoreFloat4x4(&expected, expectedLocal);
    MMDLAB_CHECK(MatricesEqual(pose.local[0], expected));

    // The child has no track, so it keeps its bind local transform.
    MMDLAB_CHECK(MatricesEqual(pose.local[1], bind.localBind[1]));
}

MMDLAB_TEST(Animation.VmdAnimator, AppliesVmdRotationAfterBindRotation)
{
    // A bone at the origin pointing +X. Its default roll yields a non-identity bind rotation
    // (local +Y is the head -> tail direction), so this test is sensitive to the VMD rotation
    // order: the rotation must be applied after the bind basis (R_bind * R_vmd), in model space,
    // not before it (R_vmd * R_bind).
    MmdLab::Skeleton skeleton;
    MmdLab::Bone bone;
    bone.name = "arm";
    bone.position[0] = 0.0f; bone.position[1] = 0.0f; bone.position[2] = 0.0f;
    bone.tail[0] = 1.0f; bone.tail[1] = 0.0f; bone.tail[2] = 0.0f;
    bone.parentIndex = MmdLab::kInvalidBoneIndex;
    skeleton.bones.push_back(std::move(bone));
    skeleton.children.resize(1);

    const MmdLab::BindPose bind = MmdLab::BuildBindPose(skeleton);

    MmdLab::VmdMotion motion;
    motion.modelName = "test";
    MmdLab::VmdBoneTrack track;
    track.boneName = "arm";
    MmdLab::VmdBoneKey key;
    key.frame = 0;
    const float halfRootTwo = 0.70710678f;
    key.rotation[0] = 0.0f; key.rotation[1] = 0.0f;
    key.rotation[2] = halfRootTwo; key.rotation[3] = halfRootTwo; // 90 deg about +Z.
    track.keys.push_back(key);
    motion.boneTracks.push_back(std::move(track));

    MmdLab::VmdAnimator animator;
    animator.SetMotion(std::move(motion));

    MmdLab::BonePose pose;
    animator.SamplePose(skeleton, bind, pose);

    std::vector<DirectX::XMFLOAT4X4> palette;
    std::vector<DirectX::XMMATRIX> worldScratch;
    MmdLab::EvaluateSkeletonPose(skeleton, bind, &pose, palette, worldScratch);

    // The skinning palette must be exactly RotZ(90 degrees), so the bone tip (1,0,0) lands at
    // (0,1,0). Applying the rotation before the bind basis would instead send it toward -Z.
    const DirectX::XMVECTOR tip = DirectX::XMVectorSet(1.0f, 0.0f, 0.0f, 1.0f);
    const DirectX::XMVECTOR skinned = DirectX::XMVector3Transform(
        tip, DirectX::XMLoadFloat4x4(&palette[0]));
    MMDLAB_CHECK(std::fabs(DirectX::XMVectorGetX(skinned)) < 1e-4f);
    MMDLAB_CHECK(std::fabs(DirectX::XMVectorGetY(skinned) - 1.0f) < 1e-4f);
    MMDLAB_CHECK(std::fabs(DirectX::XMVectorGetZ(skinned)) < 1e-4f);
}

MMDLAB_TEST(Animation.VmdAnimator, SamplesIkEnabledByBoneName)
{
    using namespace MmdLab;

    // Two IK chains: control bones "footIK" (matched by the motion) and "toeIK" (unmatched).
    Skeleton skeleton;
    skeleton.bones.resize(2);
    skeleton.bones[0].name = "footIK";
    skeleton.bones[1].name = "toeIK";
    skeleton.children.resize(2);

    IkChain footChain;
    footChain.ikBoneIndex = 0;
    footChain.targetBoneIndex = 0;
    footChain.links = { { 0 } };
    skeleton.ikChains.push_back(footChain);

    IkChain toeChain;
    toeChain.ikBoneIndex = 1;
    toeChain.targetBoneIndex = 1;
    toeChain.links = { { 1 } };
    skeleton.ikChains.push_back(toeChain);

    // A show/IK keyframe at frame 0 that disables only "footIK".
    VmdMotion motion;
    motion.modelName = "test";
    VmdShowIkKeyframe keyframe;
    keyframe.frame = 0;
    keyframe.show = true;
    VmdIkBoneState state;
    state.ikBoneName = "footIK";
    state.enabled = false;
    keyframe.ikBones.push_back(std::move(state));
    motion.showIkKeyframes.push_back(std::move(keyframe));

    VmdAnimator animator;
    animator.SetMotion(std::move(motion));

    std::vector<bool> enabled;
    animator.SampleIkEnabled(skeleton, enabled);

    MMDLAB_CHECK(enabled.size() == 2);
    MMDLAB_CHECK(!enabled[0]); // "footIK" disabled by the keyframe.
    MMDLAB_CHECK(enabled[1]);  // "toeIK" has no entry, so it defaults to enabled.
}

MMDLAB_TEST(Animation.VmdAnimator, InterpolatesPositionWithBezierCurve)
{
    using namespace MmdLab;

    const auto SetCurve = [](VmdBoneKey& key, const int curve,
        const std::uint8_t x1, const std::uint8_t y1, const std::uint8_t x2, const std::uint8_t y2)
    {
        key.interpolation[curve * 16 + 0] = x1;
        key.interpolation[curve * 16 + 4] = y1;
        key.interpolation[curve * 16 + 8] = x2;
        key.interpolation[curve * 16 + 12] = y2;
    };

    VmdBoneTrack track;
    track.boneName = "bone";

    VmdBoneKey a;
    a.frame = 0;
    for (int curve = 0; curve < 4; ++curve)
    {
        SetCurve(a, curve, 20, 20, 107, 107); // Linear on every curve.
    }
    track.keys.push_back(a);

    VmdBoneKey b;
    b.frame = 30;
    b.position[0] = 10.0f; b.position[1] = 20.0f; b.position[2] = 30.0f;
    SetCurve(b, 0, 0, 127, 127, 127); // X eases ahead: factor 0.875 at the midpoint.
    for (int curve = 1; curve < 4; ++curve)
    {
        SetCurve(b, curve, 20, 20, 107, 107); // Linear.
    }
    track.keys.push_back(b);

    VmdMotion motion;
    motion.modelName = "test";
    motion.boneTracks.push_back(std::move(track));

    VmdAnimator animator;
    animator.SetMotion(std::move(motion));
    animator.Advance(0.5f); // 15 frames = the midpoint between frame 0 and 30.

    Skeleton skeleton;
    Bone bone;
    bone.name = "bone";
    bone.parentIndex = kInvalidBoneIndex;
    skeleton.bones.push_back(std::move(bone));
    skeleton.children.resize(1);

    const BindPose bind = BuildBindPose(skeleton);
    BonePose pose;
    animator.SamplePose(skeleton, bind, pose);

    // X uses the non-linear curve (factor 0.875), Y/Z stay linear (factor 0.5).
    MMDLAB_CHECK(std::fabs(pose.local[0].m[3][0] - 8.75f) < 1e-3f);
    MMDLAB_CHECK(std::fabs(pose.local[0].m[3][1] - 10.0f) < 1e-3f);
    MMDLAB_CHECK(std::fabs(pose.local[0].m[3][2] - 15.0f) < 1e-3f);
}

MMDLAB_TEST(Animation.VmdAnimator, GenerationsSeparateMotionChangesFromSeeks)
{
    // Physics resets on a new motion but may simulate a short seek, and must not see the loop
    // wrap as either.
    MmdLab::VmdMotion motion;
    MmdLab::VmdBoneTrack track;
    track.boneName = "bone";
    track.keys.push_back(MmdLab::VmdBoneKey{});
    MmdLab::VmdBoneKey last{};
    last.frame = 30;
    track.keys.push_back(last);
    motion.boneTracks.push_back(track);

    MmdLab::VmdAnimator animator;
    animator.SetMotion(motion);
    const std::uint32_t motionGeneration = animator.MotionGeneration();
    const std::uint32_t poseGeneration = animator.PoseGeneration();

    animator.SeekFrames(10.0f);
    MMDLAB_CHECK_EQUAL(motionGeneration, animator.MotionGeneration());
    MMDLAB_CHECK(animator.PoseGeneration() != poseGeneration);

    const std::uint32_t afterSeek = animator.PoseGeneration();
    animator.Advance(1.0f); // 30 frames: wraps past the end.
    MMDLAB_CHECK_EQUAL(afterSeek, animator.PoseGeneration());
    MMDLAB_CHECK_EQUAL(motionGeneration, animator.MotionGeneration());

    animator.SetMotion(motion);
    MMDLAB_CHECK(animator.MotionGeneration() != motionGeneration);
}
