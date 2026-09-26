#include "Runtime/Asset/MeshAsset.h"

namespace MmdLab
{
MeshAsset BuildMeshAsset(const MmdlMeshData& mesh)
{
    // The .mmdl reader already validated ranges and material references; the runtime asset
    // is a straight copy of the CPU-side mesh data.
    MeshAsset asset;
    asset.vertices = mesh.vertices;
    asset.indices = mesh.indices;
    asset.materials = mesh.materials;
    asset.drawPackets = mesh.drawPackets;
    asset.textures = mesh.strings;
    return asset;
}
} // namespace MmdLab
