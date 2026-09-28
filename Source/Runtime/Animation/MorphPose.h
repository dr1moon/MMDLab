#pragma once

#include "Runtime/Animation/SkeletonPose.h"
#include "Runtime/Asset/Morph.h"

#include <span>
#include <vector>

namespace MmdLab
{
// Resolves group morphs into a flat leaf-weight vector parallel to `set.morphs`. `directWeights`
// is the sampled (pre-resolution) weight per morph from VMD or an editor; a group morph holds no
// value itself, distributing `weight * ratio` to each member recursively. A non-group morph's
// resolved weight is its direct weight plus whatever group distributions land on it.
void ResolveMorphWeights(
    const MorphSet& set,
    const std::vector<float>& directWeights,
    std::vector<float>& outResolved);

// Applies weighted bone morphs to `pose` in the bone's local frame: the position delta is a
// local-space translation and the rotation delta composes onto the local rotation (slerped from
// identity by the weight). Call after VMD sampling and before EvaluateSkeletonPose so the morph
// offsets flow into the skinning palette. This is the MVP convention; the exact MMD morph-space
// semantics are refined later against a real asset.
void ApplyBoneMorphs(
    const MorphSet& set,
    const std::vector<float>& resolvedWeights,
    BonePose& pose);

// Accumulates weighted vertex morphs into a dense per-vertex position delta. `outDeltas` holds
// three floats per vertex (parallel to the model's mesh vertices), zero-filled on entry; each
// non-zero-weight morph adds `weight * positionDelta` at its referenced vertices. The caller sizes
// `outDeltas` to vertexCount * 3.
void AccumulateVertexMorphDeltas(
    const MorphSet& set,
    const std::vector<float>& resolvedWeights,
    std::span<float> outDeltas);
} // namespace MmdLab
