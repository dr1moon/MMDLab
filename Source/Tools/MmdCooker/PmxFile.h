#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace MmdLab
{
struct PmxVertex
{
    float position[3];
    float normal[3];
    float uv[2];
};

struct PmxMaterial
{
    float diffuse[4];
    float specular[3];
    float specularStrength;
    float ambient[3];
    float edgeColor[4];
    float edgeSize;
    std::int32_t textureIndex = -1;  // -1 when there is no diffuse texture.
    std::int32_t sphereTextureIndex = -1; // -1 when there is no sphere map.
    std::int32_t toonTextureIndex = -1;   // -1 when there is no toon ramp.
    std::int32_t surfaceCount;  // Number of vertex indices (3 per triangle).
    std::uint8_t drawFlags = 0; // PMX drawing flags; bit 0x01 = double-sided.
    std::uint8_t sphereMode = 0; // 0 = off, 1 = multiply, 2 = add, 3 = subtexture.
};

// The static geometry of a PMX model: vertices, triangle indices, texture paths, and
// materials. Bones, morphs, and physics sections are not parsed yet.
struct PmxStaticMesh
{
    std::string modelName;
    std::vector<PmxVertex> vertices;
    std::vector<std::uint32_t> indices;
    std::vector<std::string> textures;
    std::vector<PmxMaterial> materials;
};

// Parses the static geometry of a PMX 2.0 file: header, model info, vertices, indices,
// textures, and materials. Skips skinning, bones, morphs, and physics. Throws
// std::runtime_error on malformed or unsupported input.
[[nodiscard]] PmxStaticMesh ParsePmxStaticMesh(const std::filesystem::path& path);
} // namespace MmdLab
