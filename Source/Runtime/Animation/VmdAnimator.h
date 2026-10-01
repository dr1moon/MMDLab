#pragma once

#include "Runtime/Animation/SkeletonPose.h"
#include "Runtime/Asset/Morph.h"
#include "Runtime/Asset/VmdFile.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>
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

    // Samples the motion at the current time into a BonePose for `skeleton`. `boneTrack` is
    // parallel to `skeleton.bones` and holds each bone's motion-track index, or -1 for a bone the
    // motion does not animate; resolve it once per motion with ResolveBoneTrackIndices. Bones
    // with a matching track use the interpolated keyframes; the rest keep their bind local.
    void SamplePose(const Skeleton& skeleton, const BindPose& bindPose,
        std::span<const std::int32_t> boneTrack, BonePose& outPose) const;

    // Samples the show/IK track at the current time into `outEnabled`, parallel to
    // `skeleton.ikChains` (true = solve the chain). `ikChainByName` maps each chain's IK-bone name
    // to its chain index, resolved once per skeleton (see ResolveIkChainByName). With no show/IK
    // keyframe every chain defaults to enabled; IK on/off is discrete, so the most recent
    // keyframe is held.
    void SampleIkEnabled(const Skeleton& skeleton,
        const std::unordered_map<std::string, std::size_t>& ikChainByName,
        std::vector<bool>& outEnabled) const;

    // Resolves each IK chain's bone name to its chain index, so SampleIkEnabled can index chains
    // without scanning and comparing names every frame. `out` is cleared and rebuilt; call once
    // per skeleton and reuse it.
    void ResolveIkChainByName(const Skeleton& skeleton, std::unordered_map<std::string, std::size_t>& out) const;

    // Samples the morph tracks at the current time into `outWeights`, parallel to `set.morphs`
    // (0 for a morph with no track). `morphTrack` is parallel to `set.morphs` and holds each
    // morph's track index, or -1; resolve it once per motion with ResolveMorphTrackIndices.
    void SampleMorphWeights(const MorphSet& set, std::span<const std::int32_t> morphTrack,
        std::vector<float>& outWeights) const;

    // Resolves each bone / morph name to its motion-track index (or -1 for no matching track, or
    // a bone track with no keyframes), so per-frame sampling indexes arrays instead of hashing
    // names. `out` is sized to match; call once per motion generation and reuse it.
    void ResolveBoneTrackIndices(const Skeleton& skeleton, std::vector<std::int32_t>& out) const;
    void ResolveMorphTrackIndices(const MorphSet& set, std::vector<std::int32_t>& out) const;

private:
    [[nodiscard]] float SampleMorphWeight(const VmdMorphTrack& track) const;

    VmdMotion motion_;
    std::unordered_map<std::string, std::size_t> trackByBoneName_;
    std::unordered_map<std::string, std::size_t> morphTrackByName_;
    float timeFrames_ = 0.0f;
    float durationFrames_ = 0.0f;
    bool playing_ = true;
    std::uint32_t poseGeneration_ = 0;
    std::uint32_t motionGeneration_ = 0;
    // One-shot: log the VMD tracks that match no skeleton bone on the first sample. Atomic
    // because SamplePose is const and may run for several models at once.
    mutable std::atomic<bool> unmatchedLogged_{ false };
};
} // namespace MmdLab
