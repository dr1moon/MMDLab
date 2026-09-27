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

    // Bones: every parent index is -1 or a valid bone, and the parent chain terminates (no cycle).
    std::size_t rootCount = 0;
    std::size_t maxDepth = 0;
    for (std::size_t i = 0; i < mesh.bones.size(); ++i)
    {
        const std::int32_t parent = mesh.bones[i].parentIndex;
        MMDLAB_CHECK(parent == -1 || (parent >= 0 && static_cast<std::size_t>(parent) < mesh.bones.size()));
        if (parent == -1)
        {
            ++rootCount;
            continue;
        }

        std::size_t depth = 0;
        std::int32_t cursor = parent;
        while (cursor != -1)
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

    MMDLAB_CHECK(model.skeleton.bones.size() > 0);
    MMDLAB_CHECK(model.skinning.size() == model.mesh.vertices.size());

    // Parent indices are valid or -1, and every parent's children list names its child.
    std::size_t childCount = 0;
    for (std::size_t i = 0; i < model.skeleton.bones.size(); ++i)
    {
        const std::int32_t parent = model.skeleton.bones[i].parentIndex;
        MMDLAB_CHECK(parent == -1 || (parent >= 0 && static_cast<std::size_t>(parent) < model.skeleton.bones.size()));
        if (parent != -1)
        {
            const std::vector<std::int32_t>& siblings = model.skeleton.children[static_cast<std::size_t>(parent)];
            MMDLAB_CHECK(std::find(siblings.begin(), siblings.end(), static_cast<std::int32_t>(i)) != siblings.end());
        }
    }
    for (const std::vector<std::int32_t>& siblings : model.skeleton.children)
    {
        childCount += siblings.size();
    }
    std::size_t nonRootCount = 0;
    for (const MmdLab::Bone& bone : model.skeleton.bones)
    {
        if (bone.parentIndex != -1)
        {
            ++nonRootCount;
        }
    }
    MMDLAB_CHECK(childCount == nonRootCount);

    // At least one bone resolves a tail distinct from its head, so the overlay has real segments.
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

    // Every vertex resolves a valid dominant bone index.
    for (const MmdLab::SkinningVertex& vertex : model.skinning)
    {
        const std::int32_t dominant = MmdLab::DominantBoneIndex(vertex);
        MMDLAB_CHECK(dominant >= 0 && static_cast<std::size_t>(dominant) < model.skeleton.bones.size());
    }

    std::printf("  retained %zu bones and %zu skinning vertices\n",
                model.skeleton.bones.size(), model.skinning.size());
}
