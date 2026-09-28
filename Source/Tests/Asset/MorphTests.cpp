#include "Runtime/Asset/MmdlFile.h"
#include "Runtime/Asset/MmdlFormat.h"
#include "Runtime/Asset/MmdlWriter.h"
#include "Runtime/Asset/PmxFile.h"
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
// --- .mmdl morph round-trip ---

MmdLab::MmdlMeshData MakeMeshWithMorphs()
{
    MmdLab::MmdlMeshData mesh;

    mesh.vertices = {
        { { 0.0f, 0.0f, 0.0f, 1.0f }, { 0.0f, 0.0f, 1.0f, 0.0f }, { 0.0f, 0.0f }, { 0.0f, 0.0f } },
        { { 1.0f, 0.0f, 0.0f, 1.0f }, { 0.0f, 0.0f, 1.0f, 0.0f }, { 1.0f, 0.0f }, { 0.0f, 0.0f } },
        { { 0.0f, 1.0f, 0.0f, 1.0f }, { 0.0f, 0.0f, 1.0f, 0.0f }, { 0.0f, 1.0f }, { 0.0f, 0.0f } },
    };
    mesh.indices = { 0, 1, 2 };

    MmdLab::MMDToonMaterial material;
    material.baseColor[0] = 1.0f;
    material.baseColor[1] = 1.0f;
    material.baseColor[2] = 1.0f;
    material.baseColor[3] = 1.0f;
    mesh.materials = { material };
    mesh.drawPackets = { { 0, 3, 0, 0, 1 } };
    mesh.refBones = { 0 };
    mesh.strings = { "white.png" };

    MmdLab::MmdlBone bone{};
    bone.name = "root";
    bone.position[1] = 0.0f;
    bone.tail[1] = 1.0f;
    bone.parentIndex = MmdLab::kInvalidBoneIndex;
    mesh.bones = { bone };

    for (int i = 0; i < 3; ++i)
    {
        MmdLab::MmdlSkinningVertex skin{};
        skin.boneIndices[0] = 0;
        skin.boneWeights[0] = 1.0f;
        mesh.skinning.push_back(skin);
    }

    // Vertex morph (mesh-local indices).
    MmdLab::Morph smile;
    smile.name = "smile";
    smile.nameEn = "smile_en";
    smile.panel = 3; // mouth.
    smile.kind = MmdLab::MorphKind::Vertex;
    smile.vertexDeltas = {
        { 0, { 0.0f, 0.1f, 0.0f } },
        { 1, { 0.2f, 0.0f, 0.0f } },
    };

    // Bone morph.
    MmdLab::Morph boneMorph;
    boneMorph.name = "bone";
    boneMorph.panel = 4; // other.
    boneMorph.kind = MmdLab::MorphKind::Bone;
    boneMorph.boneDeltas = { { 0, { 0.0f, 1.0f, 0.0f }, { 0.0f, 0.0f, 0.0f, 1.0f } } };

    // Group morph referencing the vertex and bone morphs.
    MmdLab::Morph happy;
    happy.name = "happy";
    happy.panel = 3; // mouth.
    happy.kind = MmdLab::MorphKind::Group;
    happy.groupItems = { { 0, 1.0f }, { 1, 0.5f } };

    mesh.morphs = { smile, boneMorph, happy };
    return mesh;
}

// --- VMD morph parse ---

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

std::vector<std::uint8_t> BuildTestVmdWithMorphs()
{
    std::vector<std::uint8_t> bytes;
    AppendFixedString(bytes, "Vocaloid Motion Data 0002", 30);
    AppendFixedString(bytes, "test-model", 20);

    AppendU32(bytes, 0); // Bone keyframe count.

    AppendU32(bytes, 3); // Morph keyframe count.
    AppendFixedString(bytes, "smile", 15);
    AppendU32(bytes, 30);
    AppendF32(bytes, 0.5f);
    AppendFixedString(bytes, "smile", 15);
    AppendU32(bytes, 0);
    AppendF32(bytes, 0.0f);
    AppendFixedString(bytes, "blink", 15);
    AppendU32(bytes, 0);
    AppendF32(bytes, 1.0f);

    return bytes; // No camera/light/shadow/showIK sections.
}

// --- PMX morph parse (real asset) ---

std::filesystem::path FindModelsDirectory()
{
    std::filesystem::path current = std::filesystem::current_path();
    for (int level = 0; level < 8 && !current.empty(); ++level)
    {
        const std::filesystem::path candidate = current / L"Project" / L"Models";
        if (std::filesystem::is_directory(candidate))
        {
            return candidate;
        }
        const std::filesystem::path parent = current.parent_path();
        if (parent == current)
        {
            break;
        }
        current = parent;
    }
    return {};
}

std::filesystem::path FindFirstPmx(const std::filesystem::path& directory)
{
    if (directory.empty())
    {
        return {};
    }
    for (const auto& entry : std::filesystem::recursive_directory_iterator(directory))
    {
        if (entry.is_regular_file() && entry.path().extension() == L".pmx")
        {
            return entry.path();
        }
    }
    return {};
}
} // namespace

MMDLAB_TEST(Asset.Mmdl, MorphRoundTripPreservesMorphs)
{
    const MmdLab::MmdlMeshData expected = MakeMeshWithMorphs();
    const std::filesystem::path path = std::filesystem::temp_directory_path() / "mmdl_morph_roundtrip.mmdl";

    MmdLab::WriteMmdl(path, expected);
    const MmdLab::MmdlMeshData actual = MmdLab::ReadMmdl(path);
    std::filesystem::remove(path);

    MMDLAB_CHECK(expected.morphs.size() == 3);
    MMDLAB_CHECK(actual.morphs.size() == expected.morphs.size());

    for (std::size_t i = 0; i < expected.morphs.size(); ++i)
    {
        const MmdLab::Morph& e = expected.morphs[i];
        const MmdLab::Morph& a = actual.morphs[i];
        MMDLAB_CHECK_EQUAL(e.name, a.name);
        MMDLAB_CHECK_EQUAL(e.nameEn, a.nameEn);
        MMDLAB_CHECK_EQUAL(e.panel, a.panel);
        MMDLAB_CHECK_EQUAL(static_cast<std::uint8_t>(e.kind), static_cast<std::uint8_t>(a.kind));

        MMDLAB_CHECK(e.vertexDeltas.size() == a.vertexDeltas.size());
        for (std::size_t d = 0; d < e.vertexDeltas.size(); ++d)
        {
            MMDLAB_CHECK_EQUAL(e.vertexDeltas[d].vertexIndex, a.vertexDeltas[d].vertexIndex);
            for (int axis = 0; axis < 3; ++axis)
            {
                MMDLAB_CHECK_EQUAL(e.vertexDeltas[d].positionDelta[axis], a.vertexDeltas[d].positionDelta[axis]);
            }
        }

        MMDLAB_CHECK(e.boneDeltas.size() == a.boneDeltas.size());
        for (std::size_t d = 0; d < e.boneDeltas.size(); ++d)
        {
            MMDLAB_CHECK_EQUAL(e.boneDeltas[d].boneIndex, a.boneDeltas[d].boneIndex);
            for (int axis = 0; axis < 3; ++axis)
            {
                MMDLAB_CHECK_EQUAL(e.boneDeltas[d].positionDelta[axis], a.boneDeltas[d].positionDelta[axis]);
            }
            for (int axis = 0; axis < 4; ++axis)
            {
                MMDLAB_CHECK_EQUAL(e.boneDeltas[d].rotationDelta[axis], a.boneDeltas[d].rotationDelta[axis]);
            }
        }

        MMDLAB_CHECK(e.groupItems.size() == a.groupItems.size());
        for (std::size_t d = 0; d < e.groupItems.size(); ++d)
        {
            MMDLAB_CHECK_EQUAL(e.groupItems[d].morphIndex, a.groupItems[d].morphIndex);
            MMDLAB_CHECK_EQUAL(e.groupItems[d].ratio, a.groupItems[d].ratio);
        }
    }
}

MMDLAB_TEST(Asset.Vmd, ParsesMorphMotion)
{
    const std::vector<std::uint8_t> bytes = BuildTestVmdWithMorphs();
    const std::filesystem::path path = std::filesystem::temp_directory_path() / "mmdlab_test_morph.vmd";
    {
        std::ofstream out(path, std::ios::binary);
        out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    }

    const MmdLab::VmdMotion motion = MmdLab::ParseVmdFile(path);
    std::filesystem::remove(path);

    MMDLAB_CHECK(motion.boneTracks.empty());
    MMDLAB_CHECK(motion.morphTracks.size() == 2);

    const MmdLab::VmdMorphTrack* smile = nullptr;
    const MmdLab::VmdMorphTrack* blink = nullptr;
    for (const MmdLab::VmdMorphTrack& track : motion.morphTracks)
    {
        if (track.morphName == "smile") { smile = &track; }
        if (track.morphName == "blink") { blink = &track; }
    }
    MMDLAB_CHECK(smile != nullptr);
    MMDLAB_CHECK(blink != nullptr);

    // Keys are sorted by frame after parsing (frame 30 was written before frame 0).
    MMDLAB_CHECK(smile->keys.size() == 2);
    MMDLAB_CHECK(smile->keys[0].frame == 0);
    MMDLAB_CHECK(smile->keys[1].frame == 30);
    MMDLAB_CHECK(smile->keys[1].weight == 0.5f);
    MMDLAB_CHECK(blink->keys.size() == 1);
    MMDLAB_CHECK(blink->keys[0].weight == 1.0f);
}

MMDLAB_TEST(Asset.Pmx, ParsesMorphs)
{
    const std::filesystem::path models = FindModelsDirectory();
    const std::filesystem::path pmx = FindFirstPmx(models);
    if (pmx.empty())
    {
        return; // No test asset available; skip without failing.
    }

    const MmdLab::PmxStaticMesh mesh = MmdLab::ParsePmxStaticMesh(pmx);

    // Validate morph references in the raw parse result (global vertex/bone indices).
    for (const MmdLab::Morph& morph : mesh.morphs)
    {
        for (const MmdLab::VertexMorphDelta& delta : morph.vertexDeltas)
        {
            MMDLAB_CHECK(delta.vertexIndex < mesh.vertices.size());
        }
        for (const MmdLab::BoneMorphDelta& delta : morph.boneDeltas)
        {
            MMDLAB_CHECK(delta.boneIndex < mesh.bones.size());
        }
        for (const MmdLab::GroupMorphItem& item : morph.groupItems)
        {
            MMDLAB_CHECK(item.morphIndex < mesh.morphs.size());
        }
    }

    // The cooker fans each vertex morph's global offset out to mesh-local vertices, so every
    // cooked delta must reference a vertex of the expanded mesh.
    const MmdLab::MmdlMeshData cooked = MmdLab::ConvertPmxToMmdl(mesh);
    for (const MmdLab::Morph& morph : cooked.morphs)
    {
        for (const MmdLab::VertexMorphDelta& delta : morph.vertexDeltas)
        {
            MMDLAB_CHECK(delta.vertexIndex < cooked.vertices.size());
        }
        for (const MmdLab::BoneMorphDelta& delta : morph.boneDeltas)
        {
            MMDLAB_CHECK(delta.boneIndex < cooked.bones.size());
        }
        for (const MmdLab::GroupMorphItem& item : morph.groupItems)
        {
            MMDLAB_CHECK(item.morphIndex < cooked.morphs.size());
        }
    }
}
