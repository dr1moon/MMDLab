#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace MmdLab
{
// Invalid bone index: PMX encodes "no parent"/"no bone" as -1, which is 0xFFFF once narrowed to
// the two-byte bone index the runtime skeleton tree uses. A model never exceeds 65535 bones.
inline constexpr std::uint16_t kInvalidBoneIndex = 0xFFFF;

// One bone of the runtime skeleton: its bind-pose origin (head) in model space, the resolved
// tail point, and its parent in the hierarchy (kInvalidBoneIndex for a root). The tail is a
// target bone's head when the source used a tail index, otherwise the head plus the tail offset,
// so a bone's direction can be recovered without re-reading the source format.
struct Bone
{
    std::string name;
    float position[3] = { 0.0f, 0.0f, 0.0f };
    float tail[3] = { 0.0f, 0.0f, 0.0f };
    std::uint16_t parentIndex = kInvalidBoneIndex;
    // Explicit local axes (PMX LocalCoordinate flag), used instead of the head -> tail
    // derivation when the file stores them. Otherwise all zeros.
    bool hasLocalAxes = false;
    float localX[3] = { 0.0f, 0.0f, 0.0f };
    float localZ[3] = { 0.0f, 0.0f, 0.0f };

    // "付与" (grant) inheritance: this bone rotates by a scaled copy of `inheritParentIndex`'s
    // rotation (PMX InheritRotation flag). influence is 0..1 (fraction of the parent's rotation)
    // or negative (fraction of its inverse). Deform ("D") and twist bones are driven by it.
    bool hasInheritRotation = false;
    bool hasInheritTranslation = false; // PMX InheritTranslation flag (移動付与).
    std::uint16_t inheritParentIndex = kInvalidBoneIndex;
    float inheritInfluence = 0.0f;

    // Axis constraint (PMX FixedAxis flag): the bone's rotation is projected onto this single
    // axis, so it can only twist around it (arm twist bones use it).
    bool hasFixedAxis = false;
    float fixedAxis[3] = { 0.0f, 0.0f, 0.0f };
};

// One IK chain declared by the source asset: the IK control bone, the end bone the chain reaches
// toward, and the link bones to rotate. Links are ordered tip-to-root, so a leg chain is
// [knee, thigh] with the ankle as the target (the IK bone is the foot control).
struct IkChain
{
    std::uint16_t ikBoneIndex = kInvalidBoneIndex;   // The IK control bone (e.g. the foot IK).
    std::uint16_t targetBoneIndex = kInvalidBoneIndex; // The end bone (e.g. the ankle).
    std::vector<std::uint16_t> links;                // Chain bones, tip-to-root.
};

// The static bone hierarchy of a model, kept alongside the mesh so skeleton evaluation can walk
// it. `children` is parallel to `bones`: children[i] lists the indices of the bones whose parent
// is i, in file order. `ikChains` holds the IK constraints the PMX declared.
struct Skeleton
{
    std::vector<Bone> bones;
    std::vector<std::vector<std::uint16_t>> children;
    std::vector<IkChain> ikChains;
};

// Per-vertex linear-blend-skinning data, parallel to MeshAsset::vertices. Up to four bone
// indices and their weights; the indices are submesh-local (into the submesh's
// skin-reference-bone table), and unused slots carry index 0 with weight 0.
struct SkinningVertex
{
    std::uint8_t boneIndices[4] = { 0, 0, 0, 0 };
    float boneWeights[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
};

// The submesh-local bone index whose weight dominates this vertex (largest weight among the
// active slots), or -1 when the vertex is unskinned. Map the local index through the submesh's
// skin-reference-bone table to recover the global bone.
inline std::int32_t DominantBoneIndex(const SkinningVertex& vertex)
{
    std::int32_t dominant = -1;
    float bestWeight = 0.0f;
    for (int slot = 0; slot < 4; ++slot)
    {
        if (vertex.boneWeights[slot] > bestWeight)
        {
            dominant = static_cast<std::int32_t>(vertex.boneIndices[slot]);
            bestWeight = vertex.boneWeights[slot];
        }
    }
    return dominant;
}

} // namespace MmdLab
