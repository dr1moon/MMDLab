#pragma once

#include "Runtime/Asset/PhysicsAsset.h"
#include "Runtime/Scene/Camera.h"
#include "Runtime/Scene/WorldData.h"

#include <DirectXMath.h>

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace MmdLab
{
struct Model; // Span-only in the frame projection; defined in Runtime/Asset/Model.h.

// Identifies which physical frame resource (0..2) a stage is currently handling.
// This is spatial identity: "where" in the rotating set of in-flight frames.
using FrameIndex = uint32_t;

// Identifies one logical frame (a single iteration of the render loop). This is
// temporal identity: the "when" of a frame, a monotonically increasing version. A single
// FrameResource is reused for many FrameIds over time.
using FrameId = uint64_t;

// The render-facing data for one frame: the sealed, immutable payload the GameThread
// projects and the RenderThread consumes. It carries the selected level's instances plus
// spans into the world's model pool and level list, and the UI projection (level names +
// selection) that the RhiThread renders through imgui.
struct RenderFrame
{
    std::span<const ModelInstance> instances; // Selected level's instances (snapshot, owned by the FrameResource).
    std::span<const Model> models;            // The model pool (owned by the world).
    std::span<const Level> levels;            // All levels (owned by the world).
    std::uint32_t selectedLevel = 0;          // Index the UI combo shows.
    std::uint32_t levelGeneration = 0;        // Bumps on every level switch.
    std::span<const MotionEntry> motions;     // Motion files available for playback (owned by the world).
    std::uint32_t selectedMotion = kInvalidMotionIndex; // Index the Motion combo shows.
    bool motionPlaying = true;                // Whether playback advances this frame.
    float motionTimeFrames = 0.0f;            // Current playback time, in 30 fps frames.
    float motionDurationFrames = 0.0f;        // Motion length, in 30 fps frames.
    Camera camera;                            // World camera snapshot (owned by the world).
    // Skinning palettes, concatenated in model-index order and sliced by `bonePaletteOffsets`
    // (size modelCount + 1). Owned by the FrameResource; the renderer reads per-model ranges.
    std::span<const DirectX::XMFLOAT4X4> bonePalette;
    std::span<const std::uint32_t> bonePaletteOffsets;
    // Dense per-vertex morph position deltas (3 floats per vertex), concatenated in model-index
    // order and sliced by `morphDeltaOffsets` (size modelCount + 1, in floats). Owned by the
    // FrameResource; zero for models with no active vertex morphs.
    std::span<const float> morphDeltas;
    std::span<const std::uint32_t> morphDeltaOffsets;
    // Physics debug bodies in model space, concatenated in model-index order and sliced by
    // `physicsBodyOffsets` (size modelCount + 1). Empty unless debug drawing is on.
    std::span<const PhysicsDebugBody> physicsBodies;
    std::span<const std::uint32_t> physicsBodyOffsets;
    PhysicsStats physicsStats;
};

// One sub-mesh draw command: a range of the index buffer plus the material that shades it and
// the slice of the skin-reference-bone table its vertices remap into.
struct DrawPacket
{
    std::uint32_t firstIndex;    // Offset into the index buffer, in indices.
    std::uint32_t indexCount;    // Number of indices in this range (multiple of 3).
    std::uint32_t materialIndex; // Index into the mesh asset's material array.
    std::uint32_t refBoneOffset; // Offset into the mesh asset's skin-reference-bone table.
    std::uint32_t refBoneCount;  // Number of skin-reference bones this submesh uses (<= 256).
};

// The compiled render work the RenderThread produces and the RhiThread consumes: the
// selected level's instances plus the spans and UI projection the RhiThread needs to build
// GPU resources for each referenced model and draw the imgui overlay.
struct RenderWorkBatch
{
    std::span<const ModelInstance> instances;
    std::span<const Model> models;
    std::span<const Level> levels;
    std::uint32_t selectedLevel = 0;
    std::uint32_t levelGeneration = 0;
    std::span<const MotionEntry> motions;
    std::uint32_t selectedMotion = kInvalidMotionIndex;
    bool motionPlaying = true;
    float motionTimeFrames = 0.0f;
    float motionDurationFrames = 0.0f;
    Camera camera;
    std::span<const DirectX::XMFLOAT4X4> bonePalette;
    std::span<const std::uint32_t> bonePaletteOffsets;
    std::span<const float> morphDeltas;
    std::span<const std::uint32_t> morphDeltaOffsets;
    std::span<const PhysicsDebugBody> physicsBodies;
    std::span<const std::uint32_t> physicsBodyOffsets;
    PhysicsStats physicsStats;
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
    // Frame-local copy of the selected level's instances, so the frame carries an immutable
    // snapshot of the mutable visibility flags instead of a span into the world's live state.
    std::vector<ModelInstance> instanceSnapshot;
    // Frame-local skinning palettes (concatenated in model-index order) plus their per-model
    // start offsets, so the frame carries an immutable snapshot of the GameThread's evaluation.
    std::vector<DirectX::XMFLOAT4X4> bonePaletteSnapshot;
    std::vector<std::uint32_t> bonePaletteOffsetSnapshot;
    // Frame-local morph deltas (concatenated, 3 floats per vertex) plus per-model start offsets.
    std::vector<float> morphDeltaSnapshot;
    std::vector<std::uint32_t> morphDeltaOffsetSnapshot;
    // Frame-local physics debug bodies (concatenated) plus per-model start offsets.
    std::vector<PhysicsDebugBody> physicsBodySnapshot;
    std::vector<std::uint32_t> physicsBodyOffsetSnapshot;
    // Fence value RhiThread records when it submits this frame's GPU work. It tells
    // RhiThread when the frame can be retired and the resource returned to the pool.
    uint64_t gpuFenceValue = 0;
};
} // namespace MmdLab
