#pragma once

#include <cstdint>
#include <filesystem>
#include <span>
#include <vector>

namespace MmdLab
{
// Texture storage format. RGBA8 is the uncompressed decode result; BC1 (opaque) and BC7 (alpha)
// are block-compressed formats the GPU samples directly.
enum class TextureFormat : std::uint32_t
{
    RGBA8 = 0,
    BC1 = 1, // DXT1: 4 bits/pixel, opaque.
    BC7 = 2, // 8 bits/pixel, high quality + alpha.
};

// A decoded or cooked CPU-side image. `pixels` is tightly packed RGBA8 for RGBA8, or the block
// data for BC1/BC7.
struct Image
{
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    TextureFormat format = TextureFormat::RGBA8;
    std::vector<std::uint8_t> pixels;
};

// Cooked texture format: a fixed header followed by the tightly packed RGBA8 pixels or the
// block-compressed data, so a cooked texture loads with one fread and a memcpy.
inline constexpr std::uint32_t CookedTextureMagic = 0x54584D4D; // "MMTX".
inline constexpr std::uint32_t CookedTextureVersion = 2;

struct CookedTextureHeader
{
    std::uint32_t magic;
    std::uint32_t version;
    std::uint32_t width;
    std::uint32_t height;
    std::uint32_t format; // TextureFormat.
    std::uint32_t dataBytes;
};
static_assert(sizeof(CookedTextureHeader) == 24);

// Reads a source image file's raw encoded bytes (PNG, BMP, TGA, JPEG, ...). Throws
// std::runtime_error when the file cannot be opened or sized.
[[nodiscard]] std::vector<std::uint8_t> ReadImageBytes(const std::filesystem::path& path);

// Decodes encoded image bytes (PNG, BMP, TGA, JPEG, ...) into tightly packed RGBA8. Uses the
// Windows Imaging Component (WIC) first and falls back to stb_image: WIC's system codecs decode
// the large PNGs MMD models ship far faster than stb_image's bundled, unoptimized zlib, so the
// fallback only covers formats WIC cannot decode. WIC is a COM API, so the calling thread
// initializes the multi-threaded apartment lazily. Throws std::runtime_error when neither path can
// decode the bytes. The buffer is read only during the call.
[[nodiscard]] Image DecodeImageFromBytes(std::span<const std::uint8_t> bytes);

// Decodes a texture file into tightly packed RGBA8 (ReadImageBytes + DecodeImageFromBytes).
// Named DecodeImage (not LoadImage) to avoid colliding with the <windows.h> LoadImage macro.
[[nodiscard]] Image DecodeImage(const std::filesystem::path& path);

// A 1x1 magenta image (Unity's missing-texture color), substituted when a texture cannot be
// decoded, so a broken asset is obvious instead of silently white.
[[nodiscard]] Image MissingTextureImage();

// Reads a cooked texture file's raw bytes (.mmtex: header + packed pixels or blocks). Throws
// std::runtime_error when the file cannot be opened or sized.
[[nodiscard]] std::vector<std::uint8_t> ReadCookedTextureBytes(const std::filesystem::path& path);

// Decodes cooked-texture bytes (one memcpy after validating the header). Throws std::runtime_error
// on a bad magic, version, format, or truncated data. The buffer is read only during the call.
[[nodiscard]] Image DecodeCookedTexture(std::span<const std::uint8_t> bytes);

// Loads a texture, preferring a current cooked .mmtex next to the source image; falls back to
// decoding the source into RGBA8 (no block compression at runtime — cooking is the asset
// pipeline's job).
[[nodiscard]] Image LoadTexture(const std::filesystem::path& path);
} // namespace MmdLab
