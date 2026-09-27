#pragma once

#include <cstdint>
#include <filesystem>
#include <vector>

namespace MmdLab
{
// A decoded CPU-side image: tightly packed RGBA8, row-major (4 bytes per pixel).
struct Image
{
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<std::uint8_t> pixels; // size = width * height * 4.
};

// Decodes a texture file (PNG, BMP, TGA, JPEG, ...) into tightly packed RGBA8. Uses the Windows
// Imaging Component (WIC) first and falls back to stb_image: WIC's system codecs decode the
// large PNGs MMD models ship far faster than stb_image's bundled, unoptimized zlib, so the
// fallback only covers formats WIC cannot decode. WIC is a COM API, so DecodeImage initializes
// the multi-threaded apartment on the calling thread lazily. Throws std::runtime_error when
// neither path can decode the file. Named DecodeImage (not LoadImage) to avoid colliding with
// the <windows.h> LoadImage macro.
[[nodiscard]] Image DecodeImage(const std::filesystem::path& path);

// A 1x1 magenta image (Unity's missing-texture color), substituted when a texture cannot be
// decoded, so a broken asset is obvious instead of silently white.
[[nodiscard]] Image MissingTextureImage();
} // namespace MmdLab
