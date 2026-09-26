#pragma once

#include "Runtime/Asset/Material.h"
#include "Runtime/Core/FrameResource.h"

#include <cstdint>
#include <string>
#include <vector>

namespace MmdLab
{
// The provisional .mmdl native asset format (version 4): a fixed header, a chunk table, and
// fixed-width data chunks. All integers are little-endian and fixed-width; the file contains
// no pointers, raw C++ containers, or compiler-dependent enums.

inline constexpr std::uint32_t MmdlMagic = 0x4C444D4D; // "MMDL".
inline constexpr std::uint32_t MmdlVersion = 4;

enum class MmdlChunkType : std::uint32_t
{
    StringTable = 1,
    MeshMetadata = 2,
    VertexBuffer = 3,
    IndexBuffer = 4,
    MaterialTable = 5,
    SubMeshTable = 6,
};

// Fixed file header at offset 0.
struct MmdlHeader
{
    std::uint32_t magic;
    std::uint32_t version;
    std::uint64_t chunkTableOffset;
    std::uint32_t chunkCount;
    std::uint32_t reserved;
    std::uint64_t totalFileSize;
};
static_assert(sizeof(MmdlHeader) == 32);

// One entry in the chunk table.
struct MmdlChunkDescriptor
{
    std::uint32_t type;
    std::uint32_t version;
    std::uint64_t offset;
    std::uint64_t size;
    std::uint32_t alignment;
    std::uint32_t reserved;
};
static_assert(sizeof(MmdlChunkDescriptor) == 32);

// MeshMetadata chunk payload.
struct MmdlMeshMetadata
{
    std::uint32_t vertexCount;
    std::uint32_t indexCount;
    std::uint32_t materialCount;
    std::uint32_t subMeshCount;
    std::uint32_t vertexStride;
    float boundsMin[3];
    float boundsMax[3];
};
static_assert(sizeof(MmdlMeshMetadata) == 44);

// One packed vertex: position + normal + UV, padded to 16-byte vectors (D3D12 prefers
// four-component vertex inputs). Fixed 40-byte stride, no vertex color.
struct MmdlVertex
{
    float position[4]; // xyz + w(=1).
    float normal[4];   // xyz + w(=0).
    float uv[2];
};
static_assert(sizeof(MmdlVertex) == 40);

// The CPU-side mesh data that the writer serializes and the reader returns. Materials are
// the runtime toon materials; draw packets are the sub-mesh draw ranges that reference them.
struct MmdlMeshData
{
    std::vector<MmdlVertex> vertices;
    std::vector<std::uint32_t> indices;
    std::vector<Material> materials;
    std::vector<DrawPacket> drawPackets;
    std::vector<std::string> strings; // Texture paths.
};
} // namespace MmdLab
