#pragma once

namespace MmdLab
{
struct Model;

// Builds a procedural reflective-floor model: a large quad in the XZ plane (normal +Y) with a
// single identity-skinned root bone and one opaque toon material. The World installs it in the
// registry, injects a `reflective` instance into every level, and positions it under each level's
// models when they finish loading. The renderer derives the reflection plane from the instance
// transform and shades it with a reflection pipeline.
[[nodiscard]] Model BuildReflectiveFloorModel();
} // namespace MmdLab
