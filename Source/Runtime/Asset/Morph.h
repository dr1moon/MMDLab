#pragma once

#include "Runtime/Asset/Skeleton.h"

#include <cstdint>
#include <string>
#include <vector>

namespace MmdLab
{
// PMX morph kinds (the one-byte "morph type" field). Vertex, bone, and group morphs are parsed
// and stored; the UV, material, flip, and impulse kinds are read-but-skipped until a runtime
// consumer needs them.
enum class MorphKind : std::uint8_t
{
    Group = 0,
    Vertex = 1,
    Bone = 2,
    Uv = 3,
    AdditionalUv1 = 4,
    AdditionalUv2 = 5,
    AdditionalUv3 = 6,
    AdditionalUv4 = 7,
    Material = 8,
    Flip = 9,
    Impulse = 10,
};

// One vertex-morph offset: a vertex index plus the model-space position delta applied at full
// weight. In the PMX parse result the index is a global PMX vertex index; the converter fans each
// global offset out to every per-submesh copy, so the cooked (.mmdl) representation is mesh-local.
struct VertexMorphDelta
{
    std::uint32_t vertexIndex = 0;
    float positionDelta[3] = { 0.0f, 0.0f, 0.0f };
};

// One bone-morph offset: a global bone index plus the translation and rotation delta applied at
// full weight (the rotation is a quaternion in x, y, z, w order).
struct BoneMorphDelta
{
    std::uint16_t boneIndex = kInvalidBoneIndex;
    float positionDelta[3] = { 0.0f, 0.0f, 0.0f };
    float rotationDelta[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
};

// One group-morph member: the referenced morph index and its ratio.
struct GroupMorphItem
{
    std::uint32_t morphIndex = 0;
    float ratio = 0.0f;
};

// One morph: the name, panel category, kind, and only the offsets its kind uses. Vertex offsets
// are mesh-local in the cooked representation (see VertexMorphDelta); bone and group offsets
// reference global bone/morph indices, which the model shares across submeshes.
struct Morph
{
    std::string name;
    std::string nameEn;
    std::uint8_t panel = 0; // PMX panel: 0=system, 1=eyebrow, 2=eye, 3=mouth, 4=other.
    MorphKind kind = MorphKind::Vertex;
    std::vector<VertexMorphDelta> vertexDeltas;
    std::vector<BoneMorphDelta> boneDeltas;
    std::vector<GroupMorphItem> groupItems;
};

// The morph set of a model. VMD morph-track lookup is resolved once per motion by VmdAnimator
// (which owns the motion's name -> track map), so no name index is kept here.
struct MorphSet
{
    std::vector<Morph> morphs;
};
} // namespace MmdLab
