#pragma once

#include "Runtime/Asset/ImageLoader.h"
#include "Runtime/Asset/MeshAsset.h"
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
    MeshAsset mesh;
    Skeleton skeleton;
    std::vector<SkinningVertex> skinning; // Parallel to mesh.vertices.
    std::vector<Image> textures;
};
} // namespace MmdLab
