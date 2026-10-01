#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
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

    // Evaluation order (PMX deform layer and PhysicsAfterDeform flag): IK and grants run over
    // bones sorted by (afterPhysics, deformLayer, index); after-physics bones are evaluated in a
    // second pass once the rigid-body simulation has moved the bones it drives.
    std::int32_t deformLayer = 0;
    bool afterPhysics = false;
};

// One link of an IK chain. PMX may constrain a link's local VMD rotation to an Euler-angle range;
// standard knees lock two axes and permit only forward/backward bending on the remaining axis.
struct IkLink
{
    std::uint16_t boneIndex = kInvalidBoneIndex;
    bool hasLimit = false;
    float limitMin[3] = { 0.0f, 0.0f, 0.0f };
    float limitMax[3] = { 0.0f, 0.0f, 0.0f };
};

// One IK chain declared by the source asset: the IK control bone, the end bone the chain reaches
// toward, and the link bones to rotate. Links are ordered tip-to-root, so a leg chain is
// [knee, thigh] with the ankle as the target (the IK bone is the foot control). The chain is
// solved by cyclic coordinate descent (CCD): at most `loopCount` sweeps, each turning a link by
// at most `limitAngle` radians.
struct IkChain
{
    std::uint16_t ikBoneIndex = kInvalidBoneIndex;   // The IK control bone (e.g. the foot IK).
    std::uint16_t targetBoneIndex = kInvalidBoneIndex; // The end bone (e.g. the ankle).
    std::int32_t loopCount = 40;
    float limitAngle = 2.0f;
    std::vector<IkLink> links;                       // Chain bones, tip-to-root.
};

// The static bone hierarchy of a model, kept alongside the mesh so skeleton evaluation can walk
// it. Child lists are compressed-sparse-row: bone b's children are
// childrenFlat[childrenOffsets[b] .. childrenOffsets[b+1]) in file order, so traversal reads one
// contiguous array instead of chasing a vector-of-vectors. `ikChains` holds the IK constraints
// the PMX declared, sorted by their IK bone's evaluation order. `deformOrder` lists every bone
// index sorted by (afterPhysics, deformLayer, index); when it is empty (a hand-built skeleton),
// plain index order is used.
struct Skeleton
{
    std::vector<Bone> bones;
    std::vector<std::uint16_t> childrenFlat;
    std::vector<std::uint32_t> childrenOffsets; // Size == bones.size() + 1.
    std::vector<IkChain> ikChains;
    std::vector<std::uint16_t> deformOrder;

    // The children of `bone` in file order. Requires bone < bones.size().
    [[nodiscard]] std::span<const std::uint16_t> Children(const std::size_t bone) const
    {
        return std::span<const std::uint16_t>(childrenFlat).subspan(
            childrenOffsets[bone], childrenOffsets[bone + 1] - childrenOffsets[bone]);
    }
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
