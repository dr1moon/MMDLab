#include "App/MmdViewer/ModelFrame.h"

#include "Runtime/Animation/MorphPose.h"
#include "Runtime/Animation/VmdAnimator.h"
#include "Runtime/Asset/Model.h"
#include "Runtime/Physics/PhysicsScene.h"

#include "tracy/Tracy.hpp"

#include <algorithm>
#include <chrono>

namespace MmdLab
{
ModelFrameStats EvaluateModelFrame(
    const Model& model,
    PhysicsScene* physics,
    const ModelFrameInput& input,
    ModelFrameScratch& scratch,
    const ModelFrameOutput& output)
{
    ZoneScopedN("EvaluateModelFrame");
    ModelFrameStats stats;

    // A Static mesh has no skeleton: skip pose evaluation, physics, and skinning. Vertex morphs
    // still apply (bone morphs are ignored — there is no skeleton to drive them).
    if (model.meshType == MeshType::Static)
    {
        if (input.animator != nullptr && input.animator->HasMotion())
        {
            const VmdAnimator& animator = *input.animator;
            if (scratch.cachedMotionGeneration != animator.MotionGeneration())
            {
                scratch.cachedMotionGeneration = animator.MotionGeneration();
                animator.ResolveMorphTrackIndices(model.morphs, scratch.morphTrack);
            }
            animator.SampleMorphWeights(model.morphs, scratch.morphTrack, scratch.morphWeights);
            ResolveMorphWeights(model.morphs, scratch.morphWeights, scratch.resolvedWeights, scratch.morphStack);
        }
        else
        {
            scratch.resolvedWeights.assign(model.morphs.morphs.size(), 0.0f);
        }

        std::fill(output.morphDeltas.begin(), output.morphDeltas.end(), 0.0f);
        AccumulateVertexMorphDeltas(model.morphs, scratch.resolvedWeights, output.morphDeltas);
        return stats;
    }

    const bool animated = input.animator != nullptr && input.animator->HasMotion();
    if (animated)
    {
        const VmdAnimator& animator = *input.animator;
        // The name -> track indices are fixed for a motion, so resolve them once per motion
        // generation and reuse them across frames (and across models sharing this scratch).
        if (scratch.cachedMotionGeneration != animator.MotionGeneration())
        {
            scratch.cachedMotionGeneration = animator.MotionGeneration();
            animator.ResolveBoneTrackIndices(model.skeleton, scratch.boneTrack);
            animator.ResolveMorphTrackIndices(model.morphs, scratch.morphTrack);
        }
        animator.SamplePose(model.skeleton, model.bindPose, scratch.boneTrack, scratch.motionPose);
        if (!scratch.ikChainByNameResolved)
        {
            scratch.ikChainByNameResolved = true;
            animator.ResolveIkChainByName(model.skeleton, scratch.ikChainByName);
        }
        animator.SampleIkEnabled(model.skeleton, scratch.ikChainByName, scratch.ikEnabled);
        // Sample and resolve morphs; bone morphs fold into the pose before skeleton evaluation
        // so their offsets reach the skinning palette.
        animator.SampleMorphWeights(model.morphs, scratch.morphTrack, scratch.morphWeights);
        ResolveMorphWeights(model.morphs, scratch.morphWeights, scratch.resolvedWeights, scratch.morphStack);
        ApplyBoneMorphs(model.morphs, scratch.resolvedWeights, scratch.motionPose);
        EvaluateBoneWorld(model.skeleton, model.bindPose, &scratch.motionPose, scratch.world, scratch.local,
            scratch.ikStates, &scratch.ikEnabled);
    }
    else
    {
        EvaluateBoneWorld(model.skeleton, model.bindPose, nullptr, scratch.world, scratch.local, scratch.ikStates);
        scratch.ikEnabled.clear();
        scratch.resolvedWeights.assign(model.morphs.morphs.size(), 0.0f);
    }

    // Physics runs between the animated pose and skinning: it moves the follow-bone colliders,
    // steps, and overrides the simulated bones (hair, cloth, accessories).
    if (physics != nullptr && input.physicsEnabled)
    {
        const auto physicsStart = std::chrono::steady_clock::now();
        if (input.physicsStep.reset)
        {
            physics->Reset(scratch.world);
        }
        physics->Simulate(input.physicsStep.deltaSeconds, scratch.world);
        stats.simulateMilliseconds = std::chrono::duration<float, std::milli>(
            std::chrono::steady_clock::now() - physicsStart).count();
    }
    if (physics != nullptr && !output.physicsBodies.empty())
    {
        physics->WriteDebugBodies(output.physicsBodies);
    }

    // PMX PhysicsAfterDeform bones (and their IK and grants) follow the simulation.
    EvaluateBoneWorldAfterPhysics(model.skeleton, model.bindPose, scratch.local, scratch.world,
        scratch.phaseLocal, scratch.ikStates, scratch.ikEnabled.empty() ? nullptr : &scratch.ikEnabled);
    BuildSkinningPalette(model.bindPose, scratch.world, output.palette);

    // Accumulate active vertex morphs into the dense per-vertex position delta (all zeros with no
    // motion), so the renderer uploads and applies it in the shader.
    std::fill(output.morphDeltas.begin(), output.morphDeltas.end(), 0.0f);
    AccumulateVertexMorphDeltas(model.morphs, scratch.resolvedWeights, output.morphDeltas);
    return stats;
}
} // namespace MmdLab
