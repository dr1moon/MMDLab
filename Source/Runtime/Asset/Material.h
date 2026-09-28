#pragma once

#include <cstdint>

namespace MmdLab
{
// The material kind, so the .mmdl reader and renderer can dispatch on it. MMD toon is the only
// kind today; scene props (PBR, ...) will add their own structs and kind values later.
enum class MaterialType : std::uint32_t
{
    MMDToon = 0,
};

// Sphere-map blend mode, matching the PMX material's sphere mode byte.
enum class SphereMode : std::uint32_t
{
    Off = 0,
    Multiply = 1,
    Add = 2,
    SubTexture = 3, // A second texture layer sampled with the additional UV (UV1), not the normal.
};

// The engine runtime material for MMD-style toon shading, decoupled from the PMX source format.
// The cooker translates PMX materials onto this, the .mmdl mesh serializes it, and the renderer
// consumes it. Texture fields are indices into the mesh string table (-1 = none).
struct MMDToonMaterial
{
    MaterialType type = MaterialType::MMDToon;

    float baseColor[4] = { 1.0f, 1.0f, 1.0f, 1.0f };  // diffuse albedo (RGBA).

    float specularColor[3] = { 0.0f, 0.0f, 0.0f };    // stylized specular tint.
    float specularStrength = 0.0f;                    // larger = sharper highlight.
    float ambientColor[3] = { 0.0f, 0.0f, 0.0f };     // unlit ambient tint.
    float edgeColor[4] = { 0.0f, 0.0f, 0.0f, 1.0f };  // outline color (RGBA).
    float edgeSize = 1.0f;                            // outline width.

    std::int32_t baseColorTexture = -1; // diffuse texture index, -1 = none.
    std::int32_t toonTexture = -1;      // 1D toon ramp index, -1 = none.
    std::int32_t sphereTexture = -1;    // sphere-map texture index, -1 = none.

    std::uint32_t flags = 0; // bit 0x01 = double-sided.

    std::uint32_t sphereMode = 0; // SphereMap blend mode; see SphereMode.
};
static_assert(sizeof(MMDToonMaterial) == 88);
} // namespace MmdLab
