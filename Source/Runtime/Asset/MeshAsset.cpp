#include "Runtime/Asset/MeshAsset.h"

namespace MmdLab
{
MeshAsset BuildMeshAsset(const MmdlMeshData& mesh)
{
    // The .mmdl reader already validated ranges and material references; the runtime asset
    // is a straight copy of the CPU-side mesh data, including the bounds the source computed
    // or cooked in.
    MeshAsset asset;
    asset.vertices = mesh.vertices;
    asset.indices = mesh.indices;
    asset.materials = mesh.materials;
    asset.drawPackets = mesh.drawPackets;
    asset.textures = mesh.strings;
    for (int axis = 0; axis < 3; ++axis)
    {
        asset.boundsMin[axis] = mesh.boundsMin[axis];
        asset.boundsMax[axis] = mesh.boundsMax[axis];
    }
    return asset;
}
} // namespace MmdLab
