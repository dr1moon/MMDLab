#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace MmdLab
{
// One bone of the runtime skeleton: its bind-pose origin (head) in model space, the resolved
// tail point used to draw the bone, and its parent in the hierarchy (-1 for a root). The tail
// is a target bone's head when the source used a tail index, otherwise the head plus the tail
// offset, so a bone can be drawn as a head-to-tail segment without re-reading the source format.
struct Bone
{
    std::string name;
    float position[3] = { 0.0f, 0.0f, 0.0f };
    float tail[3] = { 0.0f, 0.0f, 0.0f };
    std::int32_t parentIndex = -1;
};

// The static bone hierarchy of a model, kept alongside the mesh so debug views (and later
// skeleton evaluation) can walk it. `children` is parallel to `bones`: children[i] lists the
// indices of the bones whose parent is i, in file order.
struct Skeleton
{
    std::vector<Bone> bones;
    std::vector<std::vector<std::int32_t>> children;
};

// Per-vertex linear-blend-skinning data, parallel to MeshAsset::vertices. Up to four bone
// indices and their weights; unused slots are -1 / 0.
struct SkinningVertex
{
    std::int32_t boneIndices[4] = { -1, -1, -1, -1 };
    float boneWeights[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
};

// The bone whose weight dominates this vertex (largest weight among the active slots), or -1
// when the vertex is unskinned.
inline std::int32_t DominantBoneIndex(const SkinningVertex& vertex)
{
    std::int32_t dominant = -1;
    float bestWeight = 0.0f;
    for (int slot = 0; slot < 4; ++slot)
    {
        if (vertex.boneIndices[slot] >= 0 && vertex.boneWeights[slot] > bestWeight)
        {
            dominant = vertex.boneIndices[slot];
            bestWeight = vertex.boneWeights[slot];
        }
    }
    return dominant;
}

// A deterministic per-bone debug color: hues stepped by the golden angle so neighboring bones
// stay visually distinct, at fixed saturation and value for a consistent palette. Shared by the
// skeleton overlay and the skinning-color view so each bone's line matches its mesh region.
inline void BoneDebugColor(const std::size_t boneIndex, float rgb[3])
{
    constexpr float kGoldenAngle = 0.61803398875f;
    constexpr float kSaturation = 0.72f;
    constexpr float kValue = 0.92f;

    const float hue = std::fmod(static_cast<float>(boneIndex) * kGoldenAngle, 1.0f);
    const float h = hue < 0.0f ? hue + 1.0f : hue;

    const float chroma = kValue * kSaturation;
    const float x = chroma * (1.0f - std::fabs(std::fmod(h * 6.0f, 2.0f) - 1.0f));
    const float m = kValue - chroma;

    float r = 0.0f;
    float g = 0.0f;
    float b = 0.0f;
    const int sector = static_cast<int>(h * 6.0f);
    switch (sector)
    {
    case 0: r = chroma; g = x; b = 0.0f; break;
    case 1: r = x; g = chroma; b = 0.0f; break;
    case 2: r = 0.0f; g = chroma; b = x; break;
    case 3: r = 0.0f; g = x; b = chroma; break;
    case 4: r = x; g = 0.0f; b = chroma; break;
    default: r = chroma; g = 0.0f; b = x; break;
    }
    rgb[0] = r + m;
    rgb[1] = g + m;
    rgb[2] = b + m;
}
} // namespace MmdLab
