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

// A parsed VMD bone motion. VMD is a MikuMikuDance file-format identifier; the morph, camera,
// light, self-shadow, and IK sections are skipped because no runtime consumer exists yet.
struct VmdMotion
{
    std::string modelName;
    std::vector<VmdBoneTrack> boneTracks;
};

// Parses the header and bone-motion section of a VMD 2.0 file. Bone and model names are
// Shift-JIS encoded and are converted to UTF-8. Throws std::runtime_error on malformed input.
[[nodiscard]] VmdMotion ParseVmdFile(const std::filesystem::path& path);
} // namespace MmdLab
