#pragma once

#include "Runtime/Animation/SkeletonPose.h"
#include "Runtime/Asset/ImageLoader.h"
#include "Runtime/Asset/MeshAsset.h"
#include "Runtime/Asset/MmdlFormat.h"
#include "Runtime/Asset/Morph.h"
#include "Runtime/Asset/PhysicsAsset.h"
#include "Runtime/Asset/Skeleton.h"

#include <string>
#include <vector>

namespace MmdLab
{
// One loaded model: its CPU mesh, skeleton and skinning, decoded textures, and display name.
// Immutable after load; the RhiThread builds GPU resources on demand from this data when the
// model first appears in the selected level.
struct Model
{
    std::string name;
    MeshType meshType = MeshType::Static; // Static (rigid, no skinning) or Skeletal (skinned).
    MeshAsset mesh;
    Skeleton skeleton;                    // Empty for Static models.
    BindPose bindPose;                    // Empty for Static models.
    std::vector<SkinningVertex> skinning; // Parallel to mesh.vertices; empty for Static models.
    std::vector<Image> textures;
    MorphSet morphs; // Morph offsets (vertex offsets mesh-local); name-indexed for VMD lookup.
    PhysicsAsset physics; // Rigid bodies and joints; each model instance simulates its own copy.
};
} // namespace MmdLab
