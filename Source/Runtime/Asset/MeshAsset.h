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

    // Model-space bounds of the vertex positions, computed once at build time so level framing
    // and camera fits read them instead of re-scanning every vertex. Sentinel-filled for an
    // empty mesh.
    float boundsMin[3] = { 3.4e38f, 3.4e38f, 3.4e38f };
    float boundsMax[3] = { -3.4e38f, -3.4e38f, -3.4e38f };
};

// Builds a mesh asset from .mmdl mesh data (materials and sub-meshes carry straight through).
[[nodiscard]] MeshAsset BuildMeshAsset(const MmdlMeshData& mesh);
} // namespace MmdLab
