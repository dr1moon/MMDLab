#pragma once

#include "Runtime/Asset/Skeleton.h"

#include <DirectXMath.h>

#include <cstddef>
#include <span>
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

// Per-link scratch for the cyclic-coordinate-descent IK solver, one entry per chain link. Held in
// the caller's per-model scratch so the per-frame IK solve does not allocate.
struct IkLinkState
{
    std::uint16_t bone = kInvalidBoneIndex;
    DirectX::XMFLOAT4 animRotation{};
    DirectX::XMFLOAT4 rotation{};
    DirectX::XMFLOAT4 savedRotation{};
    DirectX::XMFLOAT3 position{};
    DirectX::XMFLOAT3 previousEuler{};
    float hingeAngle = 0.0f;
};

// Builds the bind pose from a skeleton. The bind rotation points each bone's local +Y along its
// head -> tail direction (a zero-length tail falls back to +Y); the +X/+Z roll is a stable
// default here and is refined to honor the PMX local-axis data once VMD playback needs the exact
// local frame.
[[nodiscard]] BindPose BuildBindPose(const Skeleton& skeleton);

// Fills `skeleton.deformOrder` and stably sorts `skeleton.ikChains` by their IK bone's
// (afterPhysics, deformLayer, index), the order MMD evaluates deform layers in. Call once after
// the bones and chains are built, before sampling IK state (which is parallel to ikChains).
void BuildDeformOrder(Skeleton& skeleton);

// Rebuilds `skeleton.childrenFlat` / `skeleton.childrenOffsets` from each bone's parent index, in
// file order. Call once after the bones are built, before any child traversal.
void BuildChildren(Skeleton& skeleton);

// Evaluates every bone's model-space world transform: forward kinematics over `motionPose` (null
// for the bind pose), then IK, axis constraints, and "付与" grants for the bones evaluated before
// physics, in deform order. This is the pose physics reads before it overrides the simulated
// bones. `outLocal` receives the per-bone local transforms that EvaluateBoneWorldAfterPhysics
// builds on. `ikEnabled` is as in EvaluateSkeletonPose.
void EvaluateBoneWorld(
    const Skeleton& skeleton,
    const BindPose& bindPose,
    const BonePose* motionPose,
    std::vector<DirectX::XMMATRIX>& outWorld,
    std::vector<DirectX::XMMATRIX>& outLocal,
    std::vector<IkLinkState>& ikStates,
    const std::vector<bool>* ikEnabled = nullptr);

// As above, for callers that never run the after-physics pass.
void EvaluateBoneWorld(
    const Skeleton& skeleton,
    const BindPose& bindPose,
    const BonePose* motionPose,
    std::vector<DirectX::XMMATRIX>& outWorld,
    const std::vector<bool>* ikEnabled = nullptr);

// The second evaluation pass, after physics has rewritten the simulated bones in `world`: every
// PMX PhysicsAfterDeform bone is re-evaluated from `local` on top of its final parent, then the
// after-physics IK chains, axis constraints, and grants run, so those bones follow the
// simulation. A no-op for a skeleton without such bones.
void EvaluateBoneWorldAfterPhysics(
    const Skeleton& skeleton,
    const BindPose& bindPose,
    const std::vector<DirectX::XMMATRIX>& local,
    std::vector<DirectX::XMMATRIX>& world,
    std::vector<DirectX::XMMATRIX>& phaseLocal,
    std::vector<IkLinkState>& ikStates,
    const std::vector<bool>* ikEnabled = nullptr);

// Builds the skinning palette (inverseBind_i * world_i) from final world transforms.
void BuildSkinningPalette(
    const BindPose& bindPose,
    const std::vector<DirectX::XMMATRIX>& world,
    std::vector<DirectX::XMFLOAT4X4>& outPalette);

// As above, into a caller-sized slice (outPalette.size() == world.size()), such as one model's
// range of a frame's concatenated palette.
void BuildSkinningPalette(
    const BindPose& bindPose,
    const std::vector<DirectX::XMMATRIX>& world,
    std::span<DirectX::XMFLOAT4X4> outPalette);

// EvaluateBoneWorld followed by BuildSkinningPalette, for callers without physics.
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
