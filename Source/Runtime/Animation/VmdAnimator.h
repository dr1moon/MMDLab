#pragma once

#include "Runtime/Animation/SkeletonPose.h"
#include "Runtime/Asset/Morph.h"
#include "Runtime/Asset/VmdFile.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

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
    [[nodiscard]] bool IsPlaying() const { return playing_; }
    void SetPlaying(bool playing) { playing_ = playing; }

    // Bumped whenever the sampled pose jumps rather than advancing continuously (a new motion or
    // a seek; the loop wrap is not a jump), so physics knows to reset instead of simulating it.
    [[nodiscard]] std::uint32_t PoseGeneration() const { return poseGeneration_; }

    // Bumped only when a new motion is installed, so a pose jump can be told apart from a seek.
    [[nodiscard]] std::uint32_t MotionGeneration() const { return motionGeneration_; }

    // Sets playback time directly, clamped to [0, duration], so the UI timeline can seek and
    // restart (seek to 0) without affecting the play/pause state.
    void SeekFrames(float frames);

    // Advances playback by `deltaSeconds` at the VMD 30 fps rate, looping at the duration.
    // Does nothing while paused (see SetPlaying).
    void Advance(float deltaSeconds);

    // Samples the motion at the current time into a BonePose for `skeleton`. Bones with a
    // matching track use the interpolated keyframes; the rest use their bind local transform.
    void SamplePose(const Skeleton& skeleton, const BindPose& bindPose, BonePose& outPose) const;

    // Samples the show/IK track at the current time into `outEnabled`, parallel to
    // `skeleton.ikChains` (true = solve the chain). With no show/IK keyframe every chain defaults
    // to enabled; IK on/off is discrete, so the most recent keyframe is held.
    void SampleIkEnabled(const Skeleton& skeleton, std::vector<bool>& outEnabled) const;

    // Samples the morph tracks at the current time into `outWeights`, parallel to `set.morphs`
    // (0 for a morph with no track). Morph weights are linearly interpolated between keyframes.
    void SampleMorphWeights(const MorphSet& set, std::vector<float>& outWeights) const;

private:
    [[nodiscard]] const VmdBoneTrack* FindTrack(const std::string& boneName) const;
    [[nodiscard]] float SampleMorphWeight(const VmdMorphTrack& track) const;

    VmdMotion motion_;
    std::unordered_map<std::string, std::size_t> trackByBoneName_;
    std::unordered_map<std::string, std::size_t> morphTrackByName_;
    float timeFrames_ = 0.0f;
    float durationFrames_ = 0.0f;
    bool playing_ = true;
    std::uint32_t poseGeneration_ = 0;
    std::uint32_t motionGeneration_ = 0;
    // One-shot: log the VMD tracks that match no skeleton bone on the first sample.
    mutable bool unmatchedLogged_ = false;
};
} // namespace MmdLab
