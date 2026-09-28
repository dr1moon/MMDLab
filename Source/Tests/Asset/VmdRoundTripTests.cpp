#include "Runtime/Asset/VmdFile.h"
#include "Runtime/Core/TestFramework.h"

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace
{
void AppendU32(std::vector<std::uint8_t>& out, const std::uint32_t value)
{
    out.push_back(static_cast<std::uint8_t>(value & 0xFF));
    out.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFF));
    out.push_back(static_cast<std::uint8_t>((value >> 16) & 0xFF));
    out.push_back(static_cast<std::uint8_t>((value >> 24) & 0xFF));
}

void AppendF32(std::vector<std::uint8_t>& out, const float value)
{
    std::uint32_t bits;
    std::memcpy(&bits, &value, sizeof(bits));
    AppendU32(out, bits);
}

// Appends `text` left-aligned and zero-padded to `width` bytes (VMD fixed-width fields).
void AppendFixedString(std::vector<std::uint8_t>& out, const std::string& text, const std::size_t width)
{
    for (std::size_t i = 0; i < width; ++i)
    {
        out.push_back(i < text.size() ? static_cast<std::uint8_t>(text[i]) : 0);
    }
}

// Builds a minimal valid VMD: one model name and three bone keyframes across two tracks.
std::vector<std::uint8_t> BuildTestVmd()
{
    std::vector<std::uint8_t> bytes;
    AppendFixedString(bytes, "Vocaloid Motion Data 0002", 30);
    AppendFixedString(bytes, "test-model", 20);

    AppendU32(bytes, 3); // Three bone keyframes.

    AppendFixedString(bytes, "boneA", 15);
    AppendU32(bytes, 30); // Frame 30 (written out of order to exercise the sort).
    AppendF32(bytes, 1.0f); AppendF32(bytes, 2.0f); AppendF32(bytes, 3.0f);
    AppendF32(bytes, 0.0f); AppendF32(bytes, 0.0f); AppendF32(bytes, 0.0f); AppendF32(bytes, 1.0f);
    for (int i = 0; i < 64; ++i) bytes.push_back(0);

    AppendFixedString(bytes, "boneA", 15);
    AppendU32(bytes, 0);
    AppendF32(bytes, 0.0f); AppendF32(bytes, 0.0f); AppendF32(bytes, 0.0f);
    AppendF32(bytes, 0.0f); AppendF32(bytes, 0.0f); AppendF32(bytes, 0.0f); AppendF32(bytes, 1.0f);
    for (int i = 0; i < 64; ++i) bytes.push_back(0);

    AppendFixedString(bytes, "boneB", 15);
    AppendU32(bytes, 0);
    AppendF32(bytes, 0.0f); AppendF32(bytes, 0.0f); AppendF32(bytes, 0.0f);
    AppendF32(bytes, 0.0f); AppendF32(bytes, 0.0f); AppendF32(bytes, 0.0f); AppendF32(bytes, 1.0f);
    for (int i = 0; i < 64; ++i) bytes.push_back(0);

    AppendU32(bytes, 0); // Morph keyframe count.
    AppendU32(bytes, 0); // Camera keyframe count.
    AppendU32(bytes, 0); // Light keyframe count.
    AppendU32(bytes, 0); // Self-shadow keyframe count.

    // Show/IK section: two keyframes that toggle two IK bones by name.
    AppendU32(bytes, 2); // Show/IK keyframe count.

    AppendU32(bytes, 0); // Frame 0.
    bytes.push_back(1);  // Show flag.
    AppendU32(bytes, 2); // Two IK bones.
    AppendFixedString(bytes, "footIK", 20);
    bytes.push_back(1);  // footIK enabled.
    AppendFixedString(bytes, "handIK", 20);
    bytes.push_back(0);  // handIK disabled.

    AppendU32(bytes, 60); // Frame 60.
    bytes.push_back(1);   // Show flag.
    AppendU32(bytes, 2);  // Two IK bones.
    AppendFixedString(bytes, "footIK", 20);
    bytes.push_back(0);  // footIK disabled.
    AppendFixedString(bytes, "handIK", 20);
    bytes.push_back(1);  // handIK enabled.

    return bytes;
}
} // namespace

MMDLAB_TEST(Asset.Vmd, ParsesBoneMotion)
{
    const std::vector<std::uint8_t> bytes = BuildTestVmd();
    const std::filesystem::path path = std::filesystem::temp_directory_path() / "mmdlab_test.vmd";
    {
        std::ofstream out(path, std::ios::binary);
        out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    }

    const MmdLab::VmdMotion motion = MmdLab::ParseVmdFile(path);
    std::filesystem::remove(path);

    MMDLAB_CHECK(motion.modelName == "test-model");
    MMDLAB_CHECK(motion.boneTracks.size() == 2);

    const MmdLab::VmdBoneTrack* boneA = nullptr;
    const MmdLab::VmdBoneTrack* boneB = nullptr;
    for (const MmdLab::VmdBoneTrack& track : motion.boneTracks)
    {
        if (track.boneName == "boneA") { boneA = &track; }
        if (track.boneName == "boneB") { boneB = &track; }
    }
    MMDLAB_CHECK(boneA != nullptr);
    MMDLAB_CHECK(boneB != nullptr);
    MMDLAB_CHECK(boneA->keys.size() == 2);
    MMDLAB_CHECK(boneB->keys.size() == 1);

    // Keys are sorted by frame after parsing, even though the file listed frame 30 first.
    MMDLAB_CHECK(boneA->keys[0].frame == 0);
    MMDLAB_CHECK(boneA->keys[1].frame == 30);
    MMDLAB_CHECK(boneA->keys[1].position[0] == 1.0f);
    MMDLAB_CHECK(boneA->keys[1].position[1] == 2.0f);
    MMDLAB_CHECK(boneA->keys[1].position[2] == 3.0f);
    MMDLAB_CHECK(boneA->keys[0].rotation[3] == 1.0f);
    MMDLAB_CHECK(boneB->keys[0].frame == 0);

    // The show/IK section is parsed: two keyframes, each toggling two IK bones by name.
    MMDLAB_CHECK(motion.showIkKeyframes.size() == 2);
    MMDLAB_CHECK(motion.showIkKeyframes[0].frame == 0);
    MMDLAB_CHECK(motion.showIkKeyframes[0].show == true);
    MMDLAB_CHECK(motion.showIkKeyframes[0].ikBones.size() == 2);
    MMDLAB_CHECK(motion.showIkKeyframes[0].ikBones[0].ikBoneName == "footIK");
    MMDLAB_CHECK(motion.showIkKeyframes[0].ikBones[0].enabled == true);
    MMDLAB_CHECK(motion.showIkKeyframes[0].ikBones[1].ikBoneName == "handIK");
    MMDLAB_CHECK(motion.showIkKeyframes[0].ikBones[1].enabled == false);
    MMDLAB_CHECK(motion.showIkKeyframes[1].frame == 60);
    MMDLAB_CHECK(motion.showIkKeyframes[1].ikBones[0].enabled == false);
    MMDLAB_CHECK(motion.showIkKeyframes[1].ikBones[1].enabled == true);
}
