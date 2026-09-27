#include "Runtime/Asset/ModelRegistry.h"

#include "Runtime/Asset/PmxFile.h"
#include "Runtime/Core/Utf8.h"

namespace MmdLab
{
void ModelRegistry::Reserve(const std::size_t count)
{
    models_.reserve(count);
}

Model ModelRegistry::ParseModelFile(const std::filesystem::path& path)
{
    const PmxStaticMesh pmx = ParsePmxStaticMesh(path);
    const MmdlMeshData meshData = ConvertPmxToMmdl(pmx);

    Model model;
    model.name = WideToUtf8(path.stem().wstring());
    model.mesh = BuildMeshAsset(meshData);

    // ConvertPmxToMmdl keeps only the static geometry; the skeleton and per-vertex skinning are
    // projected from the parse result here so the debug views (and later skeleton evaluation)
    // can walk them.
    model.skeleton.bones.reserve(pmx.bones.size());
    for (const PmxBone& source : pmx.bones)
    {
        Bone bone;
        bone.name = source.name;
        bone.parentIndex = source.parentIndex;
        for (int axis = 0; axis < 3; ++axis)
        {
            bone.position[axis] = source.position[axis];
        }

        // Resolve the tail: a target bone's head when the tail-index flag is set, otherwise the
        // head plus the tail offset.
        const bool hasTailBone = (source.flags & PmxBoneFlags::TailIndex) != 0
            && source.tailIndex >= 0
            && static_cast<std::size_t>(source.tailIndex) < pmx.bones.size();
        for (int axis = 0; axis < 3; ++axis)
        {
            bone.tail[axis] = hasTailBone
                ? pmx.bones[static_cast<std::size_t>(source.tailIndex)].position[axis]
                : source.position[axis] + source.tailOffset[axis];
        }

        model.skeleton.bones.push_back(std::move(bone));
    }

    model.skeleton.children.resize(model.skeleton.bones.size());
    for (std::size_t i = 0; i < model.skeleton.bones.size(); ++i)
    {
        const std::int32_t parent = model.skeleton.bones[i].parentIndex;
        if (parent >= 0 && static_cast<std::size_t>(parent) < model.skeleton.bones.size())
        {
            model.skeleton.children[static_cast<std::size_t>(parent)].push_back(static_cast<std::int32_t>(i));
        }
    }

    model.skinning.reserve(pmx.vertices.size());
    for (const PmxVertex& vertex : pmx.vertices)
    {
        SkinningVertex skinning;
        for (int slot = 0; slot < 4; ++slot)
        {
            skinning.boneIndices[slot] = vertex.boneIndices[slot];
            skinning.boneWeights[slot] = vertex.boneWeights[slot];
        }
        model.skinning.push_back(skinning);
    }

    // Textures are decoded separately on the I/O workers; their paths stay in model.mesh.textures.
    return model;
}

std::size_t ModelRegistry::AddModel(Model&& model)
{
    const std::size_t index = models_.size();
    models_.push_back(std::move(model));
    return index;
}
} // namespace MmdLab
