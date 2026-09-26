#pragma once

#include "Runtime/Asset/MmdlFormat.h"
#include "Runtime/Core/FrameResource.h"

#include <vector>

namespace MmdLab
{
// A cooked mesh ready for upload and rendering: CPU-side vertices, indices, materials, and
// the immutable draw list. The GPU buffers are created by the RhiThread from this data.
struct MeshAsset
{
    std::vector<MmdlVertex> vertices;
    std::vector<std::uint32_t> indices;
    std::vector<Material> materials;
    std::vector<DrawPacket> drawPackets;
    std::vector<std::string> textures; // Texture paths (the .mmdl string table).
};

// Builds a mesh asset from .mmdl mesh data (materials and sub-meshes carry straight through).
[[nodiscard]] MeshAsset BuildMeshAsset(const MmdlMeshData& mesh);
} // namespace MmdLab
