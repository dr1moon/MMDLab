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

// Decodes a texture file (PNG, BMP, TGA, JPEG, ...) via stb_image. Throws
// std::runtime_error on failure.
[[nodiscard]] Image LoadImage(const std::filesystem::path& path);
} // namespace MmdLab
