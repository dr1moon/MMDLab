#include "Runtime/Asset/ModelRegistry.h"
#include "Runtime/Asset/PmxFile.h"
#include "Runtime/Core/TestFramework.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>

namespace
{
// Walks up from the current directory looking for the repository's Project/Models folder, so the
// test works whether it is run from the repo root or a build directory.
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

// Returns the first .pmx file under the directory, or an empty path when none exists.
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

MMDLAB_TEST(Asset.Pmx, ParsesBonesAndSkinning)
{
    const std::filesystem::path models = FindModelsDirectory();
    const std::filesystem::path pmx = FindFirstPmx(models);
    if (pmx.empty())
    {
        return; // No test asset available; skip without failing.
    }

    const MmdLab::PmxStaticMesh mesh = MmdLab::ParsePmxStaticMesh(pmx);
    std::printf("vertices: %zu, bones: %zu\n", mesh.vertices.size(), mesh.bones.size());

    MMDLAB_CHECK(mesh.bones.size() > 0);

    // Bones: every parent index is kInvalidBoneIndex or a valid bone, and the parent chain
    // terminates (no cycle).
    std::size_t rootCount = 0;
    std::size_t maxDepth = 0;
    for (std::size_t i = 0; i < mesh.bones.size(); ++i)
    {
        const std::uint16_t parent = mesh.bones[i].parentIndex;
        MMDLAB_CHECK(parent == MmdLab::kInvalidBoneIndex || static_cast<std::size_t>(parent) < mesh.bones.size());
        if (parent == MmdLab::kInvalidBoneIndex)
        {
            ++rootCount;
            continue;
        }

        std::size_t depth = 0;
        std::uint16_t cursor = parent;
        while (cursor != MmdLab::kInvalidBoneIndex)
        {
            MMDLAB_CHECK(depth < mesh.bones.size()); // A cycle would exceed the bone count.
            cursor = mesh.bones[static_cast<std::size_t>(cursor)].parentIndex;
            ++depth;
        }
        maxDepth = std::max(maxDepth, depth);
    }
    std::printf("  root bones: %zu, max hierarchy depth: %zu\n", rootCount, maxDepth);

    // Skinning: every vertex references at least one valid bone, and the weights sum to ~1.
    for (const MmdLab::PmxVertex& vertex : mesh.vertices)
    {
        float weightSum = 0.0f;
        std::size_t boneCount = 0;
        for (int b = 0; b < 4; ++b)
        {
            if (vertex.boneIndices[b] >= 0)
            {
                ++boneCount;
                weightSum += vertex.boneWeights[b];
                MMDLAB_CHECK(static_cast<std::size_t>(vertex.boneIndices[b]) < mesh.bones.size());
            }
        }
        MMDLAB_CHECK(boneCount > 0);
        MMDLAB_CHECK(weightSum > 0.99f && weightSum < 1.01f);
    }
    std::printf("  skinning weights normalized\n");

    // Classification: a Static mesh cooks with no skinning/refBones; a Skeletal mesh carries a
    // real dependency (multi-bone skinning, a bone morph, or physics).
    const MmdLab::MmdlMeshData cooked = MmdLab::ConvertPmxToMmdl(mesh);
    if (cooked.meshType == MmdLab::MeshType::Static)
    {
        MMDLAB_CHECK(cooked.skinning.empty());
        MMDLAB_CHECK(cooked.refBones.empty());
    }
}

MMDLAB_TEST(Asset.Model, RetainsSkeletonAndSkinning)
{
    const std::filesystem::path models = FindModelsDirectory();
    const std::filesystem::path pmx = FindFirstPmx(models);
    if (pmx.empty())
    {
        return; // No test asset available; skip without failing.
    }

    const MmdLab::Model model = MmdLab::ModelRegistry::ParseModelFile(pmx);

    if (model.meshType == MmdLab::MeshType::Static)
    {
        // A single-bone model builds no runtime skeleton, skinning, or bind pose.
        MMDLAB_CHECK(model.skeleton.bones.empty());
        MMDLAB_CHECK(model.skinning.empty());
        MMDLAB_CHECK(model.bindPose.inverseBind.empty());
        return;
    }

    MMDLAB_CHECK(model.skeleton.bones.size() > 0);
    MMDLAB_CHECK(model.skinning.size() == model.mesh.vertices.size());

    // Parent indices are valid or kInvalidBoneIndex, and every parent's children list names its child.
    std::size_t childCount = 0;
    for (std::size_t i = 0; i < model.skeleton.bones.size(); ++i)
    {
        const std::uint16_t parent = model.skeleton.bones[i].parentIndex;
        MMDLAB_CHECK(parent == MmdLab::kInvalidBoneIndex || static_cast<std::size_t>(parent) < model.skeleton.bones.size());
        if (parent != MmdLab::kInvalidBoneIndex)
        {
            const auto siblings = model.skeleton.Children(static_cast<std::size_t>(parent));
            MMDLAB_CHECK(std::find(siblings.begin(), siblings.end(), static_cast<std::uint16_t>(i)) != siblings.end());
        }
    }
    childCount += model.skeleton.childrenFlat.size();
    std::size_t nonRootCount = 0;
    for (const MmdLab::Bone& bone : model.skeleton.bones)
    {
        if (bone.parentIndex != MmdLab::kInvalidBoneIndex)
        {
            ++nonRootCount;
        }
    }
    MMDLAB_CHECK(childCount == nonRootCount);

    // At least one bone resolves a tail distinct from its head, so the skeleton has real directions.
    bool hasTail = false;
    for (const MmdLab::Bone& bone : model.skeleton.bones)
    {
        if (std::fabs(bone.tail[0] - bone.position[0]) > 1e-4f
            || std::fabs(bone.tail[1] - bone.position[1]) > 1e-4f
            || std::fabs(bone.tail[2] - bone.position[2]) > 1e-4f)
        {
            hasTail = true;
            break;
        }
    }
    MMDLAB_CHECK(hasTail);

    // Every vertex resolves a dominant (submesh-local) bone, and the skin-reference-bone table
    // maps each submesh's local indices to valid global bones.
    for (const MmdLab::SkinningVertex& vertex : model.skinning)
    {
        MMDLAB_CHECK(MmdLab::DominantBoneIndex(vertex) >= 0);
    }
    for (const std::uint16_t bone : model.mesh.refBones)
    {
        MMDLAB_CHECK(static_cast<std::size_t>(bone) < model.skeleton.bones.size());
    }
    for (const MmdLab::DrawPacket& packet : model.mesh.drawPackets)
    {
        MMDLAB_CHECK(packet.refBoneCount <= 256);
        MMDLAB_CHECK(static_cast<std::uint64_t>(packet.refBoneOffset) + packet.refBoneCount <= model.mesh.refBones.size());
    }

    std::printf("  retained %zu bones and %zu skinning vertices\n",
                model.skeleton.bones.size(), model.skinning.size());
}
