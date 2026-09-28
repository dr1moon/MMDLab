#pragma once

#include "Runtime/Asset/ImageLoader.h"
#include "Runtime/Asset/MmdlFormat.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <vector>

namespace MmdLab
{
// The role a texture plays in a material, which drives the compression format choice. This is a
// cook-time concept only; the runtime reads the cooked format from the .mmtex header.
enum class TextureRole : std::uint32_t
{
    BaseColor = 0,        // diffuse albedo: BC1 (opaque) / BC7 (alpha).
    SphereMatcap = 1,     // sphere mode 1/2: smooth gradients, always BC7.
    SphereSubtexture = 2, // sphere mode 3: a UV1 layer, BC1/BC7 by alpha.
    Toon = 3,             // 1D toon ramp: keep RGBA8 (block compression wastes it).
    Unknown = 4,          // Unused or unknown: BC1/BC7 by alpha.
};

// Build options for texture cooking: the encoder quality knobs. The format per role is a fixed
// policy (toon -> RGBA8, matcap -> BC7, otherwise BC1/BC7 by alpha); the settings only trade
// quality against cook time.
struct TextureCookSettings
{
    std::uint32_t bc7Quality = 1; // bc7enc uber_level (0..4): higher = better, slower.
    bool bc1HighQuality = true;   // stb_dxt high-quality mode (refinement pass).
};

// The default cook profile the cook tool uses.
inline constexpr TextureCookSettings kDefaultTextureCookSettings{};

// Derives each texture's material role (base / sphere / toon), priority-ordered so a texture
// referenced in several roles takes the most conservative one.
[[nodiscard]] std::vector<TextureRole> ComputeTextureRoles(
    const std::vector<MMDToonMaterial>& materials, std::size_t textureCount);

// Compresses an RGBA8 image to BC1 or BC7 (or leaves it RGBA8 for a toon ramp), using the role to
// pick the format and the settings to control quality. Dimensions that are not a multiple of 4
// are padded by edge clamping.
[[nodiscard]] Image CompressToBc(
    const Image& rgba, TextureRole role, const TextureCookSettings& settings);

// Serializes an image (RGBA8 or block-compressed) to the cooked texture format.
void WriteCookedTexture(const std::filesystem::path& path, const Image& image);
} // namespace MmdLab
