#include "Runtime/Animation/VmdAnimator.h"
#include "Runtime/Core/Log.h"

#include <DirectXMath.h>

#include <algorithm>
#include <cmath>
#include <format>

namespace MmdLab
{
namespace
{
constexpr float kVmdFramesPerSecond = 30.0f;

// Samples one track at `timeFrames` into a translation offset (w = 0) and a rotation quaternion
// (x, y, z, w). Position is linearly interpolated and rotation is slerped between the two
// surrounding keyframes; the Bezier interpolation bytes are not yet consumed.
void SampleTrack(
    const VmdBoneTrack& track,
    const float timeFrames,
    DirectX::XMVECTOR& outPosition,
    DirectX::XMVECTOR& outRotation)
{
    using namespace DirectX;
    const std::vector<VmdBoneKey>& keys = track.keys;
    if (keys.empty())
    {
        return;
    }

    const auto loadPosition = [](const VmdBoneKey& key)
    {
        return XMVectorSet(key.position[0], key.position[1], key.position[2], 0.0f);
    };
    const auto loadRotation = [](const VmdBoneKey& key)
    {
        // The VMD rotation quaternion is already expressed in the model's native MMD frame
        // (left-handed, +Z into the screen), the same frame the PMX positions and bind rotations
        // are consumed in, so it is read unchanged.
        return XMVectorSet(key.rotation[0], key.rotation[1], key.rotation[2], key.rotation[3]);
    };

    // First key with frame > time; keys are sorted by frame.
    const auto upper = std::upper_bound(keys.begin(), keys.end(), timeFrames,
        [](const float t, const VmdBoneKey& key) { return t < static_cast<float>(key.frame); });

    if (upper == keys.begin())
    {
        outPosition = loadPosition(keys.front());
        outRotation = loadRotation(keys.front());
        return;
    }
    if (upper == keys.end())
    {
        outPosition = loadPosition(keys.back());
        outRotation = loadRotation(keys.back());
        return;
    }

    const VmdBoneKey& a = *(upper - 1);
    const VmdBoneKey& b = *upper;
    const float span = static_cast<float>(b.frame) - static_cast<float>(a.frame);
    const float t = span > 0.0f ? (timeFrames - static_cast<float>(a.frame)) / span : 0.0f;

    outPosition = XMVectorLerp(loadPosition(a), loadPosition(b), t);

    XMVECTOR rotationA = loadRotation(a);
    XMVECTOR rotationB = loadRotation(b);
    // Shortest-path slerp: negate B when it lies on the far side of the quaternion sphere.
    if (XMVectorGetX(XMVector4Dot(rotationA, rotationB)) < 0.0f)
    {
        rotationB = XMVectorNegate(rotationB);
    }
    outRotation = XMQuaternionSlerp(rotationA, rotationB, t);
}
} // namespace

void VmdAnimator::SetMotion(VmdMotion motion)
{
    motion_ = std::move(motion);
    trackByBoneName_.clear();
    trackByBoneName_.reserve(motion_.boneTracks.size());
    durationFrames_ = 0.0f;
    for (std::size_t i = 0; i < motion_.boneTracks.size(); ++i)
    {
        trackByBoneName_.emplace(motion_.boneTracks[i].boneName, i);
        for (const VmdBoneKey& key : motion_.boneTracks[i].keys)
        {
            durationFrames_ = std::max(durationFrames_, static_cast<float>(key.frame));
        }
    }
    timeFrames_ = 0.0f;
}

void VmdAnimator::Advance(const float deltaSeconds)
{
    if (!HasMotion())
    {
        return;
    }
    timeFrames_ += deltaSeconds * kVmdFramesPerSecond;
    if (durationFrames_ > 0.0f)
    {
        timeFrames_ = std::fmod(timeFrames_, durationFrames_);
        if (timeFrames_ < 0.0f)
        {
            timeFrames_ += durationFrames_;
        }
    }
}

void VmdAnimator::SamplePose(
    const Skeleton& skeleton,
    const BindPose& bindPose,
    BonePose& outPose) const
{
    using namespace DirectX;

    // One-shot diagnostic: log every VMD track whose bone name matches no bone in the character's
    // skeleton (the model with enough bones to be the rig, not a static prop), so a motion/model
    // name mismatch is visible instead of silently leaving those bones rigid.
    if (!unmatchedLogged_ && skeleton.bones.size() > 64)
    {
        unmatchedLogged_ = true;
        for (const VmdBoneTrack& track : motion_.boneTracks)
        {
            const bool matched = std::any_of(skeleton.bones.begin(), skeleton.bones.end(),
                [&](const Bone& bone) { return bone.name == track.boneName; });
            if (!matched)
            {
                LogWarning("Animation", std::format("VMD track '{}' matches no bone", track.boneName));
            }
        }
    }

    outPose.local.resize(skeleton.bones.size());

    for (std::size_t i = 0; i < skeleton.bones.size(); ++i)
    {
        const Bone& bone = skeleton.bones[i];
        const VmdBoneTrack* track = FindTrack(bone.name);
        if (track == nullptr)
        {
            outPose.local[i] = bindPose.localBind[i];
            continue;
        }

        XMVECTOR position;
        XMVECTOR rotation;
        SampleTrack(*track, timeFrames_, position, rotation);

        // The VMD rotation is a model-space rotation authored relative to the bone's bind
        // rotation, so compose it after the bind basis (R_bind * R_vmd), then translate to the
        // head. The VMD translation is a model-space offset added to the absolute head position
        // (it is NOT rotated by the bind basis: a foot-IK lift moves the foot up in model space).
        const XMMATRIX bindRotation = XMLoadFloat4x4(&bindPose.bindRotation[i]);
        const XMMATRIX rotationMatrix = XMMatrixRotationQuaternion(XMQuaternionNormalize(rotation));
        const XMMATRIX translation = XMMatrixTranslation(
            bone.position[0] + XMVectorGetX(position),
            bone.position[1] + XMVectorGetY(position),
            bone.position[2] + XMVectorGetZ(position));
        const XMMATRIX inverseParent = (bone.parentIndex != kInvalidBoneIndex
            && static_cast<std::size_t>(bone.parentIndex) < skeleton.bones.size())
            ? XMLoadFloat4x4(&bindPose.inverseBind[static_cast<std::size_t>(bone.parentIndex)])
            : XMMatrixIdentity();
        const XMMATRIX local = XMMatrixMultiply(
            XMMatrixMultiply(XMMatrixMultiply(bindRotation, rotationMatrix), translation),
            inverseParent);
        XMStoreFloat4x4(&outPose.local[i], local);
    }
}

const VmdBoneTrack* VmdAnimator::FindTrack(const std::string& boneName) const
{
    const auto it = trackByBoneName_.find(boneName);
    return it == trackByBoneName_.end() ? nullptr : &motion_.boneTracks[it->second];
}
} // namespace MmdLab
