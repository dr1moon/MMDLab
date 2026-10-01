#pragma once

#include "Runtime/Asset/MmdlFormat.h"
#include "Runtime/Asset/Morph.h"
#include "Runtime/Asset/PhysicsAsset.h"
#include "Runtime/Asset/Skeleton.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace MmdLab
{
struct PmxVertex
{
    float position[3];
    float normal[3];
    float uv[2];
    float uv1[2] = { 0.0f, 0.0f }; // Additional UV, used by the sphere subtexture (mode 3).
    // Linear blend skinning: up to four bone indices and their weights. BDEF1/2/4 and QDEF map
    // directly; SDEF is read as BDEF2 (its spherical C/R0/R1 is dropped), the standard LBS
    // approximation. Unused slots are -1 / 0.
    std::int32_t boneIndices[4] = { -1, -1, -1, -1 };
    float boneWeights[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
};

struct PmxMaterial
{
    float diffuse[4];
    float specular[3];
    float specularStrength;
    float ambient[3];
    float edgeColor[4];
    float edgeSize;
    std::int32_t textureIndex = -1;  // -1 when there is no diffuse texture.
    std::int32_t sphereTextureIndex = -1; // -1 when there is no sphere map.
    std::int32_t toonTextureIndex = -1;   // -1 when there is no toon ramp.
    std::int32_t indexCount;  // Vertex indices in this material's range (3 per triangle).
    std::uint8_t drawFlags = 0; // PMX drawing flags; bit 0x01 = double-sided.
    std::uint8_t sphereMode = 0; // 0 = off, 1 = multiply, 2 = add, 3 = subtexture.
};

// PMX bone flag bits (the uint16 bone flags field).
namespace PmxBoneFlags
{
inline constexpr std::uint16_t TailIndex = 0x0001;
inline constexpr std::uint16_t Rotatable = 0x0002;
inline constexpr std::uint16_t Translatable = 0x0004;
inline constexpr std::uint16_t Visible = 0x0008;
inline constexpr std::uint16_t Enabled = 0x0010;
inline constexpr std::uint16_t Ik = 0x0020;
inline constexpr std::uint16_t InheritRotation = 0x0100;
inline constexpr std::uint16_t InheritTranslation = 0x0200;
inline constexpr std::uint16_t FixedAxis = 0x0400;
inline constexpr std::uint16_t LocalCoordinate = 0x0800;
inline constexpr std::uint16_t PhysicsAfterDeform = 0x1000;
inline constexpr std::uint16_t ExternalParentDeform = 0x2000;
} // namespace PmxBoneFlags

// One link of an IK bone's chain.
struct PmxIkLink
{
    std::uint16_t boneIndex = kInvalidBoneIndex;
    bool hasLimit = false;
    float limitMin[3] = { 0.0f, 0.0f, 0.0f };
    float limitMax[3] = { 0.0f, 0.0f, 0.0f };
};

// One bone of the PMX skeleton.
struct PmxBone
{
    std::string name;
    std::string nameEn;
    float position[3];             // Bone origin in model space (the bind pose).
    std::uint16_t parentIndex = kInvalidBoneIndex; // kInvalidBoneIndex for a root bone.
    std::int32_t deformLayer = 0;
    std::uint16_t flags = 0;

    // Bone tail: an explicit target bone (TailIndex flag) or a position offset.
    std::uint16_t tailIndex = kInvalidBoneIndex;
    float tailOffset[3] = { 0.0f, 0.0f, 0.0f };

    // Inheritance (InheritRotation or InheritTranslation flag).
    std::uint16_t inheritParentIndex = kInvalidBoneIndex;
    float inheritInfluence = 0.0f;

    float fixedAxis[3] = { 0.0f, 0.0f, 0.0f }; // FixedAxis flag.
    float localX[3] = { 0.0f, 0.0f, 0.0f };    // LocalCoordinate flag.
    float localZ[3] = { 0.0f, 0.0f, 0.0f };
    std::int32_t externalParentKey = 0;         // PhysicsAfterDeform flag.

    // IK (Ik flag).
    std::uint16_t ikTargetIndex = kInvalidBoneIndex;
    std::int32_t ikLoopCount = 0;
    float ikLimitAngle = 0.0f;
    std::vector<PmxIkLink> ikLinks;
};

// The parsed geometry, skeleton, morphs, and physics of a PMX model. Morphs are parsed (vertex,
// bone, and group stored; UV, material, flip, and impulse skipped); display frames are skipped;
// rigid bodies and joints are stored. Vertex-morph offsets reference global PMX vertex indices here; ConvertPmxToMmdl fans
// them out to mesh-local indices.
//
// Note: the "Static" in this type's name is historical ("parsed PMX geometry", as opposed to
// VMD animation) and is NOT the runtime `MeshType::Static` classification — this struct carries
// bones and skinning for both static and skeletal models.
struct PmxStaticMesh
{
    std::string modelName;
    std::vector<PmxVertex> vertices;
    std::vector<std::uint32_t> indices;
    std::vector<std::string> textures;
    std::vector<PmxMaterial> materials;
    std::vector<PmxBone> bones;
    std::vector<Morph> morphs;
    PhysicsAsset physics;
};

// Parses a PMX 2.0/2.1 file: header, model info, vertices (with skinning), indices, textures,
// materials, bones, morphs, display frames (skipped), rigid bodies, and joints. Soft bodies
// (PMX 2.1) are not read. Throws std::runtime_error on malformed or unsupported input.
[[nodiscard]] PmxStaticMesh ParsePmxStaticMesh(const std::filesystem::path& path);

// Converts a parsed PMX static mesh into the runtime .mmdl mesh data representation, applying
// the same convention conversion the offline cooker performs (positions/normals widened to
// four components, PMX materials mapped to runtime toon materials, one draw packet per PMX
// material). Texture paths pass through unchanged.
[[nodiscard]] MmdlMeshData ConvertPmxToMmdl(const PmxStaticMesh& pmx);
} // namespace MmdLab
