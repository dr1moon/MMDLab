#include "Runtime/Animation/MorphPose.h"
#include "Runtime/Animation/VmdAnimator.h"
#include "Runtime/Asset/VmdFile.h"
#include "Runtime/Core/TestFramework.h"

#include <cmath>
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

void AppendFixedString(std::vector<std::uint8_t>& out, const std::string& text, const std::size_t width)
{
    for (std::size_t i = 0; i < width; ++i)
    {
        out.push_back(i < text.size() ? static_cast<std::uint8_t>(text[i]) : 0);
    }
}

// A VMD with no bone tracks and two morph tracks: "smile" (frames 0 -> 30, weight 0 -> 1) and
// "blink" (frame 0, weight 1).
std::vector<std::uint8_t> BuildMorphVmd()
{
    std::vector<std::uint8_t> bytes;
    AppendFixedString(bytes, "Vocaloid Motion Data 0002", 30);
    AppendFixedString(bytes, "test-model", 20);
    AppendU32(bytes, 0); // Bone keyframe count.

    AppendU32(bytes, 3); // Morph keyframe count.
    AppendFixedString(bytes, "smile", 15);
    AppendU32(bytes, 30);
    AppendF32(bytes, 1.0f);
    AppendFixedString(bytes, "smile", 15);
    AppendU32(bytes, 0);
    AppendF32(bytes, 0.0f);
    AppendFixedString(bytes, "blink", 15);
    AppendU32(bytes, 0);
    AppendF32(bytes, 1.0f);

    return bytes;
}
} // namespace

MMDLAB_TEST(Animation.MorphPose, ResolvesGroupMorphs)
{
    MmdLab::MorphSet set;

    MmdLab::Morph groupAll;
    groupAll.name = "all";
    groupAll.kind = MmdLab::MorphKind::Group;
    groupAll.groupItems = { { 1, 1.0f }, { 2, 0.5f } };
    set.morphs.push_back(groupAll);

    MmdLab::Morph smile;
    smile.name = "smile";
    smile.kind = MmdLab::MorphKind::Vertex;
    set.morphs.push_back(smile);

    MmdLab::Morph eye;
    eye.name = "eye";
    eye.kind = MmdLab::MorphKind::Vertex;
    set.morphs.push_back(eye);

    MmdLab::Morph nested;
    nested.name = "nested";
    nested.kind = MmdLab::MorphKind::Group;
    nested.groupItems = { { 1, 2.0f } };
    set.morphs.push_back(nested);

    const std::vector<float> direct = { 1.0f, 0.0f, 0.0f, 0.5f };
    std::vector<float> resolved;
    std::vector<MmdLab::MorphResolveEntry> stack;
    MmdLab::ResolveMorphWeights(set, direct, resolved, stack);

    MMDLAB_CHECK(resolved.size() == 4);
    MMDLAB_CHECK(std::fabs(resolved[0]) < 1e-5f);            // Group holds no value itself.
    MMDLAB_CHECK(std::fabs(resolved[1] - 2.0f) < 1e-5f);     // 1.0 * 1.0 + 0.5 * 2.0.
    MMDLAB_CHECK(std::fabs(resolved[2] - 0.5f) < 1e-5f);     // 1.0 * 0.5.
    MMDLAB_CHECK(std::fabs(resolved[3]) < 1e-5f);            // Group holds no value itself.
}

MMDLAB_TEST(Animation.MorphPose, AppliesBoneMorphs)
{
    MmdLab::Morph boneMorph;
    boneMorph.name = "shift";
    boneMorph.kind = MmdLab::MorphKind::Bone;
    boneMorph.boneDeltas = { { 0, { 1.0f, 2.0f, 3.0f }, { 0.0f, 0.0f, 0.0f, 1.0f } } };
    MmdLab::MorphSet set;
    set.morphs.push_back(boneMorph);

    DirectX::XMFLOAT4X4 identity;
    DirectX::XMStoreFloat4x4(&identity, DirectX::XMMatrixIdentity());

    MmdLab::BonePose pose;
    pose.local.push_back(identity);

    // Full weight translates the bone by the position delta.
    std::vector<float> weights = { 1.0f };
    MmdLab::ApplyBoneMorphs(set, weights, pose);
    MMDLAB_CHECK(std::fabs(pose.local[0]._41 - 1.0f) < 1e-5f);
    MMDLAB_CHECK(std::fabs(pose.local[0]._42 - 2.0f) < 1e-5f);
    MMDLAB_CHECK(std::fabs(pose.local[0]._43 - 3.0f) < 1e-5f);

    // Half weight halves the offset.
    pose.local[0] = identity;
    weights[0] = 0.5f;
    MmdLab::ApplyBoneMorphs(set, weights, pose);
    MMDLAB_CHECK(std::fabs(pose.local[0]._41 - 0.5f) < 1e-5f);
    MMDLAB_CHECK(std::fabs(pose.local[0]._42 - 1.0f) < 1e-5f);
    MMDLAB_CHECK(std::fabs(pose.local[0]._43 - 1.5f) < 1e-5f);

    // A rotation delta of 90 degrees around Z turns +X into +Y.
    const float s = 0.70710678f;
    MmdLab::Morph rotMorph;
    rotMorph.name = "rotate";
    rotMorph.kind = MmdLab::MorphKind::Bone;
    rotMorph.boneDeltas = { { 0, { 0.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, s, s } } };
    MmdLab::MorphSet rotSet;
    rotSet.morphs.push_back(rotMorph);

    MmdLab::BonePose rotPose;
    rotPose.local.push_back(identity);
    std::vector<float> rotWeights = { 1.0f };
    MmdLab::ApplyBoneMorphs(rotSet, rotWeights, rotPose);

    const DirectX::XMVECTOR rotated = DirectX::XMVector3TransformNormal(
        DirectX::XMVectorSet(1.0f, 0.0f, 0.0f, 0.0f),
        DirectX::XMLoadFloat4x4(&rotPose.local[0]));
    MMDLAB_CHECK(std::fabs(DirectX::XMVectorGetX(rotated)) < 1e-4f);
    MMDLAB_CHECK(std::fabs(DirectX::XMVectorGetY(rotated) - 1.0f) < 1e-4f);
}

MMDLAB_TEST(Animation.MorphPose, AccumulatesVertexMorphDeltas)
{
    MmdLab::Morph morph;
    morph.name = "shape";
    morph.kind = MmdLab::MorphKind::Vertex;
    morph.vertexDeltas = {
        { 0, { 1.0f, 0.0f, 0.0f } },
        { 1, { 0.0f, 2.0f, 0.0f } },
    };
    MmdLab::MorphSet set;
    set.morphs.push_back(morph);

    std::vector<float> deltas(6, 0.0f); // Two vertices * three floats.
    const std::vector<float> weights = { 1.0f };
    MmdLab::AccumulateVertexMorphDeltas(set, weights, deltas);

    MMDLAB_CHECK(std::fabs(deltas[0] - 1.0f) < 1e-5f); // Vertex 0 x.
    MMDLAB_CHECK(std::fabs(deltas[1]) < 1e-5f);
    MMDLAB_CHECK(std::fabs(deltas[2]) < 1e-5f);
    MMDLAB_CHECK(std::fabs(deltas[3]) < 1e-5f);
    MMDLAB_CHECK(std::fabs(deltas[4] - 2.0f) < 1e-5f); // Vertex 1 y.
    MMDLAB_CHECK(std::fabs(deltas[5]) < 1e-5f);
}

MMDLAB_TEST(Animation.VmdAnimator, SamplesMorphWeights)
{
    const std::vector<std::uint8_t> bytes = BuildMorphVmd();
    const std::filesystem::path path = std::filesystem::temp_directory_path() / "mmdlab_morph_sample.vmd";
    {
        std::ofstream out(path, std::ios::binary);
        out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    }
    const MmdLab::VmdMotion motion = MmdLab::ParseVmdFile(path);
    std::filesystem::remove(path);

    MmdLab::MorphSet set;
    MmdLab::Morph smile;
    smile.name = "smile";
    smile.kind = MmdLab::MorphKind::Vertex;
    set.morphs.push_back(smile);
    MmdLab::Morph blink;
    blink.name = "blink";
    blink.kind = MmdLab::MorphKind::Vertex;
    set.morphs.push_back(blink);

    MmdLab::VmdAnimator animator;
    animator.SetMotion(motion);
    animator.SeekFrames(15.0f); // Halfway between smile's frame 0 and frame 30.

    std::vector<float> weights;
    std::vector<std::int32_t> trackIndices;
    animator.ResolveMorphTrackIndices(set, trackIndices);
    animator.SampleMorphWeights(set, trackIndices, weights);
    MMDLAB_CHECK(weights.size() == 2);
    MMDLAB_CHECK(std::fabs(weights[0] - 0.5f) < 1e-5f); // Linear midpoint of 0 -> 1.
    MMDLAB_CHECK(std::fabs(weights[1] - 1.0f) < 1e-5f); // Held single keyframe.
}
