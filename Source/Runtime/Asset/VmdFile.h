#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace MmdLab
{
// One bone keyframe: a frame index (30 fps), a translation offset, a rotation quaternion
// (x, y, z, w), and the 64-byte interpolation block. The interpolation bytes are the Bezier
// control points; they are stored verbatim but not yet consumed (interpolation is linear/slerp
// until Bezier evaluation is implemented).
struct VmdBoneKey
{
    std::uint32_t frame = 0;
    float position[3] = { 0.0f, 0.0f, 0.0f };
    float rotation[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
    std::uint8_t interpolation[64] = { 0 };
};

// One bone's keyframes, sorted by frame. Keyed by the bone's UTF-8 name.
struct VmdBoneTrack
{
    std::string boneName;
    std::vector<VmdBoneKey> keys;
};

// One IK bone's on/off state recorded by a show/IK keyframe, keyed by the IK bone's UTF-8 name.
struct VmdIkBoneState
{
    std::string ikBoneName;
    bool enabled = true;
};

// One show/IK (表示・IK) keyframe: the frame, the model display flag, and the on/off state of
// every IK bone at that frame. IK is a discrete on/off, so sampling holds the most recent
// keyframe rather than interpolating.
struct VmdShowIkKeyframe
{
    std::uint32_t frame = 0;
    bool show = true;                     // Model display flag (表示); parsed but not consumed.
    std::vector<VmdIkBoneState> ikBones;  // IK on/off states, in file order.
};

// One morph keyframe: a frame index (30 fps) and the morph weight at that frame.
struct VmdMorphKey
{
    std::uint32_t frame = 0;
    float weight = 0.0f;
};

// One morph's keyframes, sorted by frame. Keyed by the morph's UTF-8 name.
struct VmdMorphTrack
{
    std::string morphName;
    std::vector<VmdMorphKey> keys;
};

// A parsed VMD motion: the model name, the bone tracks, the morph tracks, and the show/IK
// keyframes. VMD is a MikuMikuDance file-format identifier; the camera, light, and self-shadow
// sections are skipped because no runtime consumer exists yet.
struct VmdMotion
{
    std::string modelName;
    std::vector<VmdBoneTrack> boneTracks;
    std::vector<VmdMorphTrack> morphTracks;        // Sorted by frame within each track.
    std::vector<VmdShowIkKeyframe> showIkKeyframes; // Sorted by frame.
};

// Parses the header, bone-motion, and morph-motion sections of a VMD 2.0 file. Bone and model
// names are Shift-JIS encoded and are converted to UTF-8. Throws std::runtime_error on malformed
// input.
[[nodiscard]] VmdMotion ParseVmdFile(const std::filesystem::path& path);
} // namespace MmdLab
