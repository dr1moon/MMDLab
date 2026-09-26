#pragma once

#include <cstdint>

namespace MmdLab
{
// Identifies which physical frame resource (0..2) a stage is currently handling.
// This is spatial identity: "where" in the rotating set of in-flight frames.
using FrameIndex = uint32_t;

// Identifies one logical frame (a single iteration of the render loop). This is
// temporal identity: the "when" of a frame, a monotonically increasing version. A single
// FrameResource is reused for many FrameIds over time.
using FrameId = uint64_t;

// The render-facing data for one frame: the sealed, immutable payload the GameThread
// projects and the RenderThread consumes. Populated with camera and instance data once
// the render milestone is reached.
struct RenderFrame
{
};

// The compiled render work the RenderThread produces and the RhiThread consumes.
// Populated with draw packets once the render milestone is reached.
struct RenderWorkBatch
{
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
