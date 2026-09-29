#include "Runtime/Animation/VmdAnimator.h"
#include "Runtime/Core/Log.h"

#include "tracy/Tracy.hpp"

#include <DirectXMath.h>

#include <algorithm>
#include <cmath>
#include <format>

namespace MmdLab
{
namespace
{
constexpr float kVmdFramesPerSecond = 30.0f;

// Evaluates one MMD cubic-Bezier interpolation curve at linear time `t` (0..1). The curve maps the
// normalized time axis (x) to the normalized value axis (y): the control points (x1, y1) and
// (x2, y2), each in [0, 127], define a cubic Bezier from (0, 0) to (1, 1). Newton's method
// inverts x(s) = t to recover the curve parameter s, then returns y(s).
float EvaluateBezier(const std::uint8_t x1, const std::uint8_t y1, const std::uint8_t x2, const std::uint8_t y2, const float t)
{
    const float px1 = static_cast<float>(x1) / 127.0f;
    const float py1 = static_cast<float>(y1) / 127.0f;
    const float px2 = static_cast<float>(x2) / 127.0f;
    const float py2 = static_cast<float>(y2) / 127.0f;

    // x(s) = a s^3 + b s^2 + c s;  y(s) = d s^3 + e s^2 + f s.
    const float a = 3.0f * px1 - 3.0f * px2 + 1.0f;
    const float b = 3.0f * px2 - 6.0f * px1;
    const float c = 3.0f * px1;
    const float d = 3.0f * py1 - 3.0f * py2 + 1.0f;
    const float e = 3.0f * py2 - 6.0f * py1;
    const float f = 3.0f * py1;

    float s = t;
    for (int i = 0; i < 8; ++i)
    {
        const float x = ((a * s + b) * s + c) * s;
        const float dx = (3.0f * a * s + 2.0f * b) * s + c;
        if (std::fabs(dx) < 1e-6f)
        {
            break;
        }
        s = std::clamp(s - (x - t) / dx, 0.0f, 1.0f);
    }

    return ((d * s + e) * s + f) * s;
}

// Samples one track at `timeFrames` into a translation offset (w = 0) and a rotation quaternion
// (x, y, z, w). Position is per-component Bezier-interpolated and rotation is slerped between the
// two surrounding keyframes using each component's interpolation curve.
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

    // The interpolation curve lives on the later keyframe (b) and eases the a -> b segment. The
    // 64-byte block packs the four curves (X, Y, Z, R) in 16-byte slices with the control points
    // at offsets 0, 4, 8, 12 (x1, y1, x2, y2); each component uses its own curve, so the factors
    // differ and are applied per component.
    const float tx = EvaluateBezier(b.interpolation[0], b.interpolation[4], b.interpolation[8], b.interpolation[12], t);
    const float ty = EvaluateBezier(b.interpolation[16], b.interpolation[20], b.interpolation[24], b.interpolation[28], t);
    const float tz = EvaluateBezier(b.interpolation[32], b.interpolation[36], b.interpolation[40], b.interpolation[44], t);
    const float tr = EvaluateBezier(b.interpolation[48], b.interpolation[52], b.interpolation[56], b.interpolation[60], t);

    const XMVECTOR positionA = loadPosition(a);
    const XMVECTOR positionB = loadPosition(b);
    outPosition = XMVectorSet(
        XMVectorGetX(positionA) + (XMVectorGetX(positionB) - XMVectorGetX(positionA)) * tx,
        XMVectorGetY(positionA) + (XMVectorGetY(positionB) - XMVectorGetY(positionA)) * ty,
        XMVectorGetZ(positionA) + (XMVectorGetZ(positionB) - XMVectorGetZ(positionA)) * tz,
        0.0f);

    XMVECTOR rotationA = loadRotation(a);
    XMVECTOR rotationB = loadRotation(b);
    // Shortest-path slerp: negate B when it lies on the far side of the quaternion sphere.
    if (XMVectorGetX(XMVector4Dot(rotationA, rotationB)) < 0.0f)
    {
        rotationB = XMVectorNegate(rotationB);
    }
    outRotation = XMQuaternionSlerp(rotationA, rotationB, tr);
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

    morphTrackByName_.clear();
    morphTrackByName_.reserve(motion_.morphTracks.size());
    for (std::size_t i = 0; i < motion_.morphTracks.size(); ++i)
    {
        morphTrackByName_.emplace(motion_.morphTracks[i].morphName, i);
        for (const VmdMorphKey& key : motion_.morphTracks[i].keys)
        {
            durationFrames_ = std::max(durationFrames_, static_cast<float>(key.frame));
        }
    }
    timeFrames_ = 0.0f;
}

void VmdAnimator::Advance(const float deltaSeconds)
{
    if (!playing_ || !HasMotion())
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

void VmdAnimator::SeekFrames(const float frames)
{
    timeFrames_ = std::clamp(frames, 0.0f, durationFrames_);
}

void VmdAnimator::SamplePose(
    const Skeleton& skeleton,
    const BindPose& bindPose,
    BonePose& outPose) const
{
    ZoneScopedN("VmdAnimator::SamplePose");
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
        if (track == nullptr || track->keys.empty())
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

void VmdAnimator::SampleIkEnabled(const Skeleton& skeleton, std::vector<bool>& outEnabled) const
{
    outEnabled.assign(skeleton.ikChains.size(), true);

    // Hold the most recent show/IK keyframe at or before the current time (IK is discrete).
    const auto upper = std::upper_bound(motion_.showIkKeyframes.begin(), motion_.showIkKeyframes.end(),
        timeFrames_,
        [](const float t, const VmdShowIkKeyframe& keyframe) { return t < static_cast<float>(keyframe.frame); });
    if (upper == motion_.showIkKeyframes.begin())
    {
        return; // Before the first show/IK keyframe; every chain stays enabled.
    }

    const VmdShowIkKeyframe& active = *(upper - 1);
    for (const VmdIkBoneState& state : active.ikBones)
    {
        for (std::size_t i = 0; i < skeleton.ikChains.size(); ++i)
        {
            const std::uint16_t ikBone = skeleton.ikChains[i].ikBoneIndex;
            if (ikBone != kInvalidBoneIndex && static_cast<std::size_t>(ikBone) < skeleton.bones.size()
                && skeleton.bones[ikBone].name == state.ikBoneName)
            {
                outEnabled[i] = state.enabled;
                break;
            }
        }
    }
}

void VmdAnimator::SampleMorphWeights(const MorphSet& set, std::vector<float>& outWeights) const
{
    ZoneScopedN("VmdAnimator::SampleMorphWeights");
    outWeights.assign(set.morphs.size(), 0.0f);
    for (std::size_t i = 0; i < set.morphs.size(); ++i)
    {
        const auto it = morphTrackByName_.find(set.morphs[i].name);
        if (it != morphTrackByName_.end())
        {
            outWeights[i] = SampleMorphWeight(motion_.morphTracks[it->second]);
        }
    }
}

float VmdAnimator::SampleMorphWeight(const VmdMorphTrack& track) const
{
    if (track.keys.empty())
    {
        return 0.0f;
    }

    const auto upper = std::upper_bound(track.keys.begin(), track.keys.end(), timeFrames_,
        [](const float t, const VmdMorphKey& key) { return t < static_cast<float>(key.frame); });
    if (upper == track.keys.begin())
    {
        return track.keys.front().weight;
    }
    if (upper == track.keys.end())
    {
        return track.keys.back().weight;
    }

    const VmdMorphKey& a = *(upper - 1);
    const VmdMorphKey& b = *upper;
    const float span = static_cast<float>(b.frame) - static_cast<float>(a.frame);
    const float t = span > 0.0f ? (timeFrames_ - static_cast<float>(a.frame)) / span : 0.0f;
    return a.weight + (b.weight - a.weight) * t;
}
} // namespace MmdLab
