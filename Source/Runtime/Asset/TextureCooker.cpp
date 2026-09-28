#include "Runtime/Asset/TextureCooker.h"

// stb_dxt: single-header BC1/DXT1 compressor (public domain), used for opaque textures.
#define STB_DXT_IMPLEMENTATION
#include "ThirdParty/stb/stb_dxt.h"

// bc7enc: BC7 block compressor (MIT/public domain), used for textures with alpha and matcaps.
#include "ThirdParty/bc7enc/bc7enc.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <stdexcept>

namespace MmdLab
{
namespace
{
// Gathers a 4x4 block of RGBA8 at block (bx, by), clamping to the image bounds.
void GatherBlock(const std::uint8_t* rgba, const std::uint32_t width, const std::uint32_t height,
    const std::uint32_t bx, const std::uint32_t by, std::uint8_t out[64])
{
    for (int y = 0; y < 4; ++y)
    {
        for (int x = 0; x < 4; ++x)
        {
            const std::uint32_t px = std::min(bx * 4 + x, width - 1);
            const std::uint32_t py = std::min(by * 4 + y, height - 1);
            std::memcpy(out + (y * 4 + x) * 4, rgba + (static_cast<std::size_t>(py) * width + px) * 4, 4);
        }
    }
}

// True when any pixel has alpha < 255, so BC7 is required instead of BC1.
bool HasAlpha(const std::uint8_t* rgba, const std::size_t pixelCount)
{
    for (std::size_t i = 0; i < pixelCount; ++i)
    {
        if (rgba[i * 4 + 3] != 255)
        {
            return true;
        }
    }
    return false;
}

// Ensures bc7enc's global state is initialized once (required before any block encode).
void EnsureBc7Initialized()
{
    static bool initialized = false;
    if (!initialized)
    {
        bc7enc_compress_block_init();
        initialized = true;
    }
}

Image CompressBc1(const std::uint8_t* rgba, const std::uint32_t width, const std::uint32_t height,
    const bool highQuality)
{
    const std::uint32_t blocksX = (width + 3) / 4;
    const std::uint32_t blocksY = (height + 3) / 4;
    Image image;
    image.width = width;
    image.height = height;
    image.format = TextureFormat::BC1;
    image.pixels.resize(static_cast<std::size_t>(blocksX) * blocksY * 8);
    std::uint8_t block[64];
    const int mode = highQuality ? STB_DXT_HIGHQUAL : STB_DXT_NORMAL;
    for (std::uint32_t by = 0; by < blocksY; ++by)
    {
        for (std::uint32_t bx = 0; bx < blocksX; ++bx)
        {
            GatherBlock(rgba, width, height, bx, by, block);
            stb_compress_dxt_block(
                image.pixels.data() + (static_cast<std::size_t>(by) * blocksX + bx) * 8,
                block, 0, mode);
        }
    }
    return image;
}

Image CompressBc7(const std::uint8_t* rgba, const std::uint32_t width, const std::uint32_t height,
    const std::uint32_t quality)
{
    const std::uint32_t blocksX = (width + 3) / 4;
    const std::uint32_t blocksY = (height + 3) / 4;
    Image image;
    image.width = width;
    image.height = height;
    image.format = TextureFormat::BC7;
    image.pixels.resize(static_cast<std::size_t>(blocksX) * blocksY * 16);
    std::uint8_t block[64];
    bc7enc_compress_block_params params;
    bc7enc_compress_block_params_init(&params);
    params.m_uber_level = std::min(quality, static_cast<std::uint32_t>(BC7ENC_MAX_UBER_LEVEL));
    for (std::uint32_t by = 0; by < blocksY; ++by)
    {
        for (std::uint32_t bx = 0; bx < blocksX; ++bx)
        {
            GatherBlock(rgba, width, height, bx, by, block);
            bc7enc_compress_block(
                image.pixels.data() + (static_cast<std::size_t>(by) * blocksX + bx) * 16,
                block, &params);
        }
    }
    return image;
}

int RolePriority(const TextureRole role)
{
    switch (role)
    {
    case TextureRole::Toon: return 4;
    case TextureRole::SphereMatcap: return 3;
    case TextureRole::SphereSubtexture: return 2;
    case TextureRole::BaseColor: return 1;
    case TextureRole::Unknown:
    default: return 0;
    }
}
} // namespace

std::vector<TextureRole> ComputeTextureRoles(
    const std::vector<MMDToonMaterial>& materials, const std::size_t textureCount)
{
    std::vector<TextureRole> roles(textureCount, TextureRole::Unknown);
    for (const MMDToonMaterial& material : materials)
    {
        const auto assign = [&](const std::int32_t index, const TextureRole role)
        {
            if (index >= 0 && RolePriority(role) > RolePriority(roles[static_cast<std::size_t>(index)]))
            {
                roles[static_cast<std::size_t>(index)] = role;
            }
        };
        assign(material.baseColorTexture, TextureRole::BaseColor);
        assign(material.toonTexture, TextureRole::Toon);
        if (material.sphereTexture >= 0)
        {
            assign(material.sphereTexture,
                material.sphereMode == 3 ? TextureRole::SphereSubtexture : TextureRole::SphereMatcap);
        }
    }
    return roles;
}

Image CompressToBc(const Image& rgba, const TextureRole role, const TextureCookSettings& settings)
{
    if (rgba.format != TextureFormat::RGBA8 || rgba.width == 0 || rgba.height == 0)
    {
        return rgba;
    }
    // A 1D toon ramp stays RGBA8: block compression needs 4x4 2D blocks and would waste space and
    // precision on a smooth gradient.
    if (role == TextureRole::Toon)
    {
        return rgba;
    }
    EnsureBc7Initialized();
    // Matcaps are smooth gradients; BC1's 5:6:5 endpoints would band, so always use BC7.
    if (role == TextureRole::SphereMatcap)
    {
        return CompressBc7(rgba.pixels.data(), rgba.width, rgba.height, settings.bc7Quality);
    }
    const std::size_t pixelCount = static_cast<std::size_t>(rgba.width) * rgba.height;
    const bool hasAlpha = HasAlpha(rgba.pixels.data(), pixelCount);
    return hasAlpha
        ? CompressBc7(rgba.pixels.data(), rgba.width, rgba.height, settings.bc7Quality)
        : CompressBc1(rgba.pixels.data(), rgba.width, rgba.height, settings.bc1HighQuality);
}

void WriteCookedTexture(const std::filesystem::path& path, const Image& image)
{
    CookedTextureHeader header{};
    header.magic = CookedTextureMagic;
    header.version = CookedTextureVersion;
    header.width = image.width;
    header.height = image.height;
    header.format = static_cast<std::uint32_t>(image.format);
    header.dataBytes = static_cast<std::uint32_t>(image.pixels.size());

    std::ofstream file(path, std::ios::binary);
    if (!file)
    {
        throw std::runtime_error("Failed to create the cooked texture file.");
    }
    file.write(reinterpret_cast<const char*>(&header), sizeof(header));
    file.write(reinterpret_cast<const char*>(image.pixels.data()),
        static_cast<std::streamsize>(image.pixels.size()));
}
} // namespace MmdLab
