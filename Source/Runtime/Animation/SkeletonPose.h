#pragma once

#include "Runtime/Asset/Skeleton.h"

#include <DirectXMath.h>

#include <cstddef>
#include <vector>

namespace MmdLab
{
// The immutable bind pose of a model, computed once from its Skeleton. `localBind[i]` is the
// bind local transform (translation to the bone head times the bind rotation derived from the
// head -> tail direction); `inverseBind[i]` is the inverse of the bone's bind world transform, so
// a model-space vertex multiplied by it lands in the bone's bind space for re-projection through
// the animated world transform.
struct BindPose
{
    // The parent-relative bind local transform (bind rotation composed with the head translation,
    // mapped back into the parent's frame).
    std::vector<DirectX::XMFLOAT4X4> localBind;
    // The pure bind rotation (model space), kept separate so animation can compose the VMD
    // rotation after it (R_bind * R_vmd) rather than before it.
    std::vector<DirectX::XMFLOAT4X4> bindRotation;
    std::vector<DirectX::XMFLOAT4X4> inverseBind;
};

// A per-bone local-transform override for one frame of animation, parallel to Skeleton::bones.
// A null pointer means "evaluate the bind pose" (localBind). When a motion poses a subset of
// bones, absent entries fall back to the bind local transform.
struct BonePose
{
    std::vector<DirectX::XMFLOAT4X4> local;
};

// Builds the bind pose from a skeleton. The bind rotation points each bone's local +Y along its
// head -> tail direction (a zero-length tail falls back to +Y); the +X/+Z roll is a stable
// default here and is refined to honor the PMX local-axis data once VMD playback needs the exact
// local frame.
[[nodiscard]] BindPose BuildBindPose(const Skeleton& skeleton);

// Evaluates the skinning palette (world_i * inverseBind_i) for every bone, ready for the vertex
// shader to consume as its bone-matrix array. `motionPose` may be null for the bind pose, in
// which case the palette is the identity and skinning reproduces the static mesh. `ikEnabled`,
// when non-null, is parallel to `skeleton.ikChains` and disables solving for the chains whose
// entry is false (null means every chain is solved). `scratchWorld` is reused across calls so
// the per-bone world matrices are not allocated every evaluation.
void EvaluateSkeletonPose(
    const Skeleton& skeleton,
    const BindPose& bindPose,
    const BonePose* motionPose,
    std::vector<DirectX::XMFLOAT4X4>& outPalette,
    std::vector<DirectX::XMMATRIX>& scratchWorld,
    const std::vector<bool>* ikEnabled = nullptr);
} // namespace MmdLab
