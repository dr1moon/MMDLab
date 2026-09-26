#pragma once

#include <cstdint>
#include <span>
#include <string>

namespace MmdLab
{
struct MeshAsset;
struct Image;

// Identifies which physical frame resource (0..2) a stage is currently handling.
// This is spatial identity: "where" in the rotating set of in-flight frames.
using FrameIndex = uint32_t;

// Identifies one logical frame (a single iteration of the render loop). This is
// temporal identity: the "when" of a frame, a monotonically increasing version. A single
// FrameResource is reused for many FrameIds over time.
using FrameId = uint64_t;

// The render-facing data for one frame: the sealed, immutable payload the GameThread
// projects and the RenderThread consumes. It carries the selected model's CPU asset data and
// the UI projection (model names + selection) that the RhiThread renders through imgui.
struct RenderFrame
{
    const MeshAsset* mesh = nullptr;          // Selected model's CPU mesh (owned by the Scene).
    std::span<const Image> textures;          // Selected model's CPU textures (owned by the Scene).
    std::uint32_t modelGeneration = 0;        // Bumps on every model switch.
    std::span<const std::string> modelNames;  // UTF-8 display names (owned by the Scene).
    std::uint32_t selectedModel = 0;          // Index the UI combo shows.
};

// One sub-mesh draw command: a range of the index buffer plus the material that shades it.
struct DrawPacket
{
    std::uint32_t firstIndex;    // Offset into the index buffer, in indices.
    std::uint32_t indexCount;    // Number of indices in this range (multiple of 3).
    std::uint32_t materialIndex; // Index into the mesh asset's material array.
};

// The compiled render work the RenderThread produces and the RhiThread consumes: the
// immutable draw list plus the model and UI projection it needs to (re)build GPU resources
// and draw the imgui overlay.
struct RenderWorkBatch
{
    std::span<const DrawPacket> drawPackets;
    const MeshAsset* mesh = nullptr;
    std::span<const Image> textures;
    std::uint32_t modelGeneration = 0;
    std::span<const std::string> modelNames;
    std::uint32_t selectedModel = 0;
};

// One reusable bundle of everything an in-flight frame needs across the
// GameThread -> RenderThread -> RhiThread pipeline. Exactly three of these rotate: a
// stage owns a FrameResource only while it holds the matching FrameIndex, so at any
// moment each resource has a single owner.
struct FrameResource
{
    FrameId frameId = 0;
    // Each type name carries the content; each field name carries the direction
    // (producer -> consumer) of one edge of the data graph.
    RenderFrame gameToRender;      // GameThread produces, RenderThread consumes.
    RenderWorkBatch renderToRhi;   // RenderThread produces, RhiThread consumes.
    // Fence value RhiThread records when it submits this frame's GPU work. It tells
    // RhiThread when the frame can be retired and the resource returned to the pool.
    uint64_t gpuFenceValue = 0;
};
} // namespace MmdLab
