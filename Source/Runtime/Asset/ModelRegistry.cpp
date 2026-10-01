#include "Runtime/Asset/ModelRegistry.h"

#include "Runtime/Asset/MmdlFile.h"
#include "Runtime/Core/Log.h"
#include "Runtime/Core/Utf8.h"

#include <windows.h>

#include <format>
#include <stdexcept>
#include <vector>

namespace MmdLab
{
namespace
{
// Builds a runtime Model from cooked .mmdl mesh data. The mesh is a straight copy; the skeleton
// is projected from the already-cooked bone records (tail resolved, flags expanded, IK chains
// inline), so a .mmdl load needs no PMX re-parse. Shared by the PMX and .mmdl load paths.
Model BuildModelFromMmdlData(std::string name, const MmdlMeshData& meshData)
{
    Model model;
    model.name = std::move(name);
    model.mesh = BuildMeshAsset(meshData);

    model.skeleton.bones.reserve(meshData.bones.size());
    for (const MmdlBone& source : meshData.bones)
    {
        Bone bone;
        bone.name = source.name;
        bone.parentIndex = source.parentIndex;
        for (int axis = 0; axis < 3; ++axis)
        {
            bone.position[axis] = source.position[axis];
            bone.tail[axis] = source.tail[axis];
            bone.localX[axis] = source.localX[axis];
            bone.localZ[axis] = source.localZ[axis];
            bone.fixedAxis[axis] = source.fixedAxis[axis];
        }
        bone.hasLocalAxes = source.hasLocalAxes != 0;
        bone.hasInheritRotation = source.hasInheritRotation != 0;
        bone.hasInheritTranslation = source.hasInheritTranslation != 0;
        bone.inheritParentIndex = source.inheritParentIndex;
        bone.inheritInfluence = source.inheritInfluence;
        bone.hasFixedAxis = source.hasFixedAxis != 0;
        bone.deformLayer = source.deformLayer;
        bone.afterPhysics = source.afterPhysics != 0;
        model.skeleton.bones.push_back(std::move(bone));
    }

    BuildChildren(model.skeleton);

    // IK chains: every bone that stores an IK target drives one chain.
    for (std::size_t i = 0; i < meshData.bones.size(); ++i)
    {
        const MmdlBone& source = meshData.bones[i];
        if (source.ikTargetIndex == kInvalidBoneIndex)
        {
            continue;
        }
        IkChain chain;
        chain.ikBoneIndex = static_cast<std::uint16_t>(i);
        chain.targetBoneIndex = source.ikTargetIndex;
        chain.loopCount = source.ikLoopCount;
        chain.limitAngle = source.ikLimitAngle;
        chain.links.reserve(source.ikLinks.size());
        for (const MmdlIkLink& sourceLink : source.ikLinks)
        {
            IkLink link;
            link.boneIndex = sourceLink.boneIndex;
            link.hasLimit = sourceLink.hasLimit != 0;
            for (int axis = 0; axis < 3; ++axis)
            {
                link.limitMin[axis] = sourceLink.limitMin[axis];
                link.limitMax[axis] = sourceLink.limitMax[axis];
            }
            chain.links.push_back(link);
        }
        model.skeleton.ikChains.push_back(std::move(chain));
    }
    BuildDeformOrder(model.skeleton);

    // Skinning arrives already remapped to submesh-local u8 indices by the cooker.
    model.skinning.reserve(meshData.skinning.size());
    for (const MmdlSkinningVertex& vertex : meshData.skinning)
    {
        SkinningVertex skinning;
        for (int slot = 0; slot < 4; ++slot)
        {
            skinning.boneIndices[slot] = vertex.boneIndices[slot];
            skinning.boneWeights[slot] = vertex.boneWeights[slot];
        }
        model.skinning.push_back(skinning);
    }

    // Morphs arrive already cooked: vertex offsets reference mesh-local vertices, and bone/group
    // offsets reference global indices.
    model.morphs.morphs = meshData.morphs;

    model.physics = meshData.physics;

    // Derive the inverse-bind matrices once so per-frame skeleton evaluation only needs to walk
    // the hierarchy and multiply; this is a pure function of the skeleton, so it is safe on the
    // I/O thread.
    model.bindPose = BuildBindPose(model.skeleton);

    // Textures are decoded separately on the I/O workers; their paths stay in model.mesh.textures.
    return model;
}

// Spawns the cook tool (MmdCooker.exe, next to this executable) to cook `pmx` into `mmdl`. The
// runtime has no PMX parser, so a missing or stale .mmdl is re-cooked out-of-process by the asset
// pipeline rather than in-process.
bool CookModel(const std::filesystem::path& pmx, const std::filesystem::path& mmdl)
{
    wchar_t buffer[MAX_PATH];
    if (GetModuleFileNameW(nullptr, buffer, MAX_PATH) == 0)
    {
        return false;
    }
    const std::filesystem::path cooker =
        std::filesystem::path(buffer).parent_path() / L"MmdCooker.exe";
    if (!std::filesystem::exists(cooker))
    {
        return false;
    }

    std::wstring commandLine =
        L"\"" + cooker.wstring() + L"\" \"" + pmx.wstring() + L"\" \"" + mmdl.wstring() + L"\"";
    std::vector<wchar_t> command(commandLine.begin(), commandLine.end());
    command.push_back(L'\0');

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(cooker.c_str(), command.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process))
    {
        return false;
    }
    WaitForSingleObject(process.hProcess, INFINITE);
    DWORD exitCode = 0;
    GetExitCodeProcess(process.hProcess, &exitCode);
    CloseHandle(process.hProcess);
    CloseHandle(process.hThread);
    return exitCode == 0;
}
} // namespace

void ModelRegistry::Reserve(const std::size_t count)
{
    models_.reserve(count);
}

Model ModelRegistry::ParseModelFile(const std::filesystem::path& path)
{
    // Prefer a current cooked .mmdl; spawn the cook tool when it is missing, stale, or corrupt.
    std::filesystem::path mmdlPath = path;
    mmdlPath.replace_extension(L".mmdl");
    if (std::filesystem::exists(mmdlPath))
    {
        try
        {
            return ParseModelFileFromMmdl(mmdlPath);
        }
        catch (const std::exception& exception)
        {
            LogWarning("Asset", std::format("'{}' is stale or corrupt ({}); re-cooking.",
                mmdlPath.string(), exception.what()));
        }
    }

    // Re-cook out-of-process via the asset pipeline, then read the fresh .mmdl.
    if (!CookModel(path, mmdlPath))
    {
        throw std::runtime_error("Failed to cook the model (MmdCooker.exe not found or failed).");
    }
    return ParseModelFileFromMmdl(mmdlPath);
}

Model ModelRegistry::ParseModelFileFromMmdl(const std::filesystem::path& path)
{
    const MmdlMeshData meshData = ReadMmdl(path);
    return BuildModelFromMmdlData(WideToUtf8(path.stem().wstring()), meshData);
}

std::size_t ModelRegistry::AddModel(Model&& model)
{
    const std::size_t index = models_.size();
    models_.push_back(std::move(model));
    return index;
}
} // namespace MmdLab
