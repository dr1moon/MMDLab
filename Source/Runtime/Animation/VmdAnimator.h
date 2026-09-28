#pragma once

#include "Runtime/Animation/SkeletonPose.h"
#include "Runtime/Asset/VmdFile.h"

#include <cstddef>
#include <string>
#include <unordered_map>

namespace MmdLab
{
// GameThread-owned playback state for one VMD bone motion. Advances a frame time and samples the
// motion into per-bone local transforms that EvaluateSkeletonPose then converts to a skinning
// palette. Bones whose names do not match a track keep their bind local transform.
class VmdAnimator final
{
public:
    // Replaces the current motion and resets playback to the first frame.
    void SetMotion(VmdMotion motion);

    [[nodiscard]] bool HasMotion() const { return !motion_.boneTracks.empty(); }
    [[nodiscard]] float TimeFrames() const { return timeFrames_; }
    [[nodiscard]] float DurationFrames() const { return durationFrames_; }

    // Advances playback by `deltaSeconds` at the VMD 30 fps rate, looping at the duration.
    void Advance(float deltaSeconds);

    // Samples the motion at the current time into a BonePose for `skeleton`. Bones with a
    // matching track use the interpolated keyframes; the rest use their bind local transform.
    void SamplePose(const Skeleton& skeleton, const BindPose& bindPose, BonePose& outPose) const;

private:
    [[nodiscard]] const VmdBoneTrack* FindTrack(const std::string& boneName) const;

    VmdMotion motion_;
    std::unordered_map<std::string, std::size_t> trackByBoneName_;
    float timeFrames_ = 0.0f;
    float durationFrames_ = 0.0f;
    // One-shot: log the VMD tracks that match no skeleton bone on the first sample.
    mutable bool unmatchedLogged_ = false;
};
} // namespace MmdLab
