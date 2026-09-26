#include "Runtime/Asset/ImageLoader.h"

// stb_image: single-header image decoder (MIT / public domain). STBI_WINDOWS_UTF8 makes
// stbi_load open files via UTF-8 paths (_wfopen), so non-ASCII texture names resolve.
#define STB_IMAGE_IMPLEMENTATION
#define STBI_WINDOWS_UTF8
#include "ThirdParty/stb/stb_image.h"

#include <windows.h>

#include <stdexcept>
#include <string>

namespace MmdLab
{
namespace
{
// Converts a UTF-16 wide path to UTF-8, which stbi_load expects under STBI_WINDOWS_UTF8.
std::string WideToUtf8(const std::wstring& wide)
{
    if (wide.empty())
    {
        return {};
    }
    const int length = WideCharToMultiByte(
        CP_UTF8, 0, wide.c_str(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
    std::string utf8(static_cast<std::size_t>(length), '\0');
    WideCharToMultiByte(
        CP_UTF8, 0, wide.c_str(), static_cast<int>(wide.size()), utf8.data(), length, nullptr, nullptr);
    return utf8;
}
} // namespace

Image LoadImage(const std::filesystem::path& path)
{
    const std::string utf8Path = WideToUtf8(path.wstring());

    int width = 0;
    int height = 0;
    int channels = 0;
    stbi_uc* pixels = stbi_load(utf8Path.c_str(), &width, &height, &channels, 4); // 4 = force RGBA8.
    if (pixels == nullptr)
    {
        const char* reason = stbi_failure_reason();
        throw std::runtime_error(
            std::string("Failed to load texture image: ") + (reason != nullptr ? reason : "unknown error"));
    }

    Image image;
    image.width = static_cast<std::uint32_t>(width);
    image.height = static_cast<std::uint32_t>(height);
    image.pixels.assign(pixels, pixels + static_cast<std::size_t>(width) * height * 4);
    stbi_image_free(pixels);
    return image;
}
} // namespace MmdLab
