#pragma once

#include "Runtime/Animation/MorphPose.h"
#include "Runtime/Animation/SkeletonPose.h"
#include "Runtime/Asset/PhysicsAsset.h"
#include "Runtime/Physics/PhysicsStepPolicy.h"

#include <DirectXMath.h>

#include <cstdint>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace MmdLab
{
struct Model;
class PhysicsScene;
class VmdAnimator;

// Per-model scratch the GameThread reuses across frames, so evaluating a model allocates only
// when its size grows. One per registry model: each model's evaluation touches only its own
// scratch, which is what lets models be evaluated independently.
struct ModelFrameScratch
{
    BonePose motionPose;
    std::vector<bool> ikEnabled;
    std::vector<DirectX::XMMATRIX> world;
    std::vector<DirectX::XMMATRIX> local;
    std::vector<DirectX::XMMATRIX> phaseLocal;
    std::vector<IkLinkState> ikStates;
    std::vector<float> morphWeights;
    std::vector<float> resolvedWeights;
    std::vector<MorphResolveEntry> morphStack;
    // Precomputed VMD track indices, resolved once per motion generation so per-frame sampling
    // indexes arrays instead of hashing bone/morph names. `boneTrack` is parallel to
    // Skeleton::bones; `morphTrack` is parallel to MorphSet::morphs; -1 means "no track".
    std::vector<std::int32_t> boneTrack;
    std::vector<std::int32_t> morphTrack;
    std::uint32_t cachedMotionGeneration = 0xFFFFFFFFu;
    // Maps each IK chain's bone name to its chain index, resolved once per skeleton so
    // SampleIkEnabled indexes chains instead of scanning and comparing names every frame.
    std::unordered_map<std::string, std::size_t> ikChainByName;
    bool ikChainByNameResolved = false;
};

// This frame's inputs shared by every model: read-only while the models evaluate.
struct ModelFrameInput
{
    const VmdAnimator* animator = nullptr; // Null or motionless: every model holds its bind pose.
    PhysicsStep physicsStep;
    bool physicsEnabled = true;
    bool physicsDebugDraw = false;
};

// Where one model writes this frame: its pre-sized slices of the frame's concatenated
// snapshots. `physicsBodies` is empty unless debug drawing is on.
struct ModelFrameOutput
{
    std::span<DirectX::XMFLOAT4X4> palette;
    std::span<float> morphDeltas;
    std::span<PhysicsDebugBody> physicsBodies;
};

// What one model reports back, summed into the frame's PhysicsStats after every model finishes.
struct ModelFrameStats
{
    float simulateMilliseconds = 0.0f;
};

// Evaluates one model for this frame, in the standard MMD order: sample the motion and morphs,
// solve the skeleton, step physics, re-solve the after-physics bones, then build the skinning
// palette and the vertex-morph deltas. Touches only `model`'s own `physics` scene and `scratch`
// and writes only `output`, so distinct models may be evaluated concurrently.
[[nodiscard]] ModelFrameStats EvaluateModelFrame(
    const Model& model,
    PhysicsScene* physics,
    const ModelFrameInput& input,
    ModelFrameScratch& scratch,
    const ModelFrameOutput& output);
} // namespace MmdLab
