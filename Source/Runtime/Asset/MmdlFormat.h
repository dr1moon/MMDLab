#pragma once

#include "Runtime/Asset/Material.h"
#include "Runtime/Asset/Skeleton.h"
#include "Runtime/Core/FrameResource.h"

#include <cstdint>
#include <string>
#include <vector>

namespace MmdLab
{
// The provisional .mmdl native asset format (version 6): a fixed header, a chunk table, and
// fixed-width data chunks. All integers are little-endian and fixed-width; the file contains
// no pointers, raw C++ containers, or compiler-dependent enums.

inline constexpr std::uint32_t MmdlMagic = 0x4C444D4D; // "MMDL".
inline constexpr std::uint32_t MmdlVersion = 7;

enum class MmdlChunkType : std::uint32_t
{
    StringTable = 1,
    MeshMetadata = 2,
    VertexBuffer = 3,
    IndexBuffer = 4,
    MaterialTable = 5,
    SubMeshTable = 6,
    Skeleton = 7,
    SkinningVertexBuffer = 8,
    SubMeshBoneTable = 9,
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
    float uv[2];       // base UV.
    float uv1[2];      // additional UV, used by the sphere subtexture (mode 3).
};
static_assert(sizeof(MmdlVertex) == 48);

// One bone of the skeleton, with the bind-pose head position and the already-resolved tail point
// (the writer resolves a tail-index reference to a target bone's head), plus the explicit local
// axes, "付与" (grant) inheritance, axis constraint, and IK chain the PMX stored. This is the
// complete cooked representation, so a .mmdl load needs no PMX re-parse. The name is stored
// inline (not in the string table) so the string table stays texture paths only.
struct MmdlBone
{
    std::string name;
    float position[3];
    float tail[3];
    std::uint16_t parentIndex;
    std::uint32_t hasLocalAxes; // 0 or 1.
    float localX[3];
    float localZ[3];

    // "付与" (grant) inheritance.
    std::uint32_t hasInheritRotation;    // 0 or 1.
    std::uint32_t hasInheritTranslation; // 0 or 1.
    std::uint16_t inheritParentIndex;
    float inheritInfluence;

    // Axis constraint (PMX FixedAxis).
    std::uint32_t hasFixedAxis; // 0 or 1.
    float fixedAxis[3];

    // IK: kInvalidBoneIndex when this bone is not an IK bone; otherwise the chain it drives.
    std::uint16_t ikTargetIndex = kInvalidBoneIndex;
    std::int32_t ikLoopCount = 0;
    float ikLimitAngle = 0.0f;
    std::vector<std::uint16_t> ikLinks;
};

// Per-vertex linear-blend-skinning weights, parallel to the vertex buffer. Up to four bone
// indices and weights; the indices are submesh-local (into the submesh's skin-reference-bone
// table) and unused slots carry index 0 with weight 0.
struct MmdlSkinningVertex
{
    std::uint8_t boneIndices[4];
    float boneWeights[4];
};
static_assert(sizeof(MmdlSkinningVertex) == 20);

// The CPU-side mesh data that the writer serializes and the reader returns. Materials are
// the runtime toon materials; draw packets are the sub-mesh draw ranges that reference them.
struct MmdlMeshData
{
    std::vector<MmdlVertex> vertices;
    std::vector<std::uint32_t> indices;
    std::vector<MMDToonMaterial> materials;
    std::vector<DrawPacket> drawPackets;
    std::vector<std::string> strings; // Texture paths.

    // Skeleton and per-vertex skinning, cooked so the .mmdl is a complete asset.
    std::vector<MmdlBone> bones;
    std::vector<MmdlSkinningVertex> skinning; // Parallel to `vertices`.

    // Concatenated per-submesh skin-reference-bone lists: global bone indices (u16), sliced by
    // DrawPacket::refBoneOffset/refBoneCount. Each submesh's list has <= 256 entries so the
    // per-vertex u8 indices above stay in range.
    std::vector<std::uint16_t> refBones;

    // Model-space bounds of the vertex positions. The source populates them (the PMX converter
    // computes them; the .mmdl reader reads the cooked values) so downstream consumers never
    // re-scan the vertices. Sentinel-filled until populated.
    float boundsMin[3] = { 3.4e38f, 3.4e38f, 3.4e38f };
    float boundsMax[3] = { -3.4e38f, -3.4e38f, -3.4e38f };
};
} // namespace MmdLab
