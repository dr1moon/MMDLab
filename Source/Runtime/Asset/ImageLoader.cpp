#include "Runtime/Asset/ImageLoader.h"

// stb_image: single-header image decoder (MIT / public domain). Kept as the fallback for formats
// WIC cannot decode (or when WIC is unavailable). STBI_WINDOWS_UTF8 makes stbi_load open files via
// UTF-8 paths (_wfopen), so non-ASCII texture names resolve.
#define STB_IMAGE_IMPLEMENTATION
#define STBI_WINDOWS_UTF8
#include "ThirdParty/stb/stb_image.h"

#include <windows.h>

#include <objbase.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <stdexcept>
#include <string>

#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "ole32.lib")

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

// WIC needs COM on the calling thread. The asset I/O workers are the only callers of DecodeImage,
// so initialize the multi-threaded apartment lazily, once per thread.
void EnsureComInitialized()
{
    thread_local bool initialized = false;
    if (!initialized)
    {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        initialized = true;
    }
}

// Decodes any image WIC understands (PNG/JPEG/BMP/...) into tightly packed RGBA8. WIC's codecs
// are far faster than stb_image's bundled zlib for the large PNGs MMD models ship. Returns false
// (leaving `image` untouched) when WIC cannot decode the file.
bool TryDecodeWic(const std::filesystem::path& path, Image& image)
{
    using Microsoft::WRL::ComPtr;

    EnsureComInitialized();

    ComPtr<IWICImagingFactory> factory;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&factory))))
    {
        return false;
    }

    ComPtr<IWICBitmapDecoder> decoder;
    if (FAILED(factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
                                                  WICDecodeMetadataCacheOnDemand, &decoder)))
    {
        return false;
    }

    ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(decoder->GetFrame(0, &frame)))
    {
        return false;
    }

    UINT width = 0;
    UINT height = 0;
    if (FAILED(frame->GetSize(&width, &height)))
    {
        return false;
    }

    ComPtr<IWICFormatConverter> converter;
    if (FAILED(factory->CreateFormatConverter(&converter)))
    {
        return false;
    }
    // 32bppRGBA puts R,G,B,A in memory, matching stb_image's RGBA8 output the renderer expects.
    if (FAILED(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppRGBA,
                                     WICBitmapDitherTypeNone, nullptr, 0.0,
                                     WICBitmapPaletteTypeCustom)))
    {
        return false;
    }

    image.width = width;
    image.height = height;
    image.pixels.resize(static_cast<std::size_t>(width) * height * 4);
    const UINT stride = width * 4;
    if (FAILED(converter->CopyPixels(nullptr, stride, static_cast<UINT>(image.pixels.size()),
                                     image.pixels.data())))
    {
        image.pixels.clear();
        return false;
    }
    return true;
}
} // namespace

Image DecodeImage(const std::filesystem::path& path)
{
    Image image;
    if (TryDecodeWic(path, image))
    {
        return image;
    }

    // Fall back to stb_image for formats WIC cannot decode.
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

    image.width = static_cast<std::uint32_t>(width);
    image.height = static_cast<std::uint32_t>(height);
    image.pixels.assign(pixels, pixels + static_cast<std::size_t>(width) * height * 4);
    stbi_image_free(pixels);
    return image;
}

Image MissingTextureImage()
{
    Image image;
    image.width = 1;
    image.height = 1;
    image.pixels = { 255, 0, 255, 255 }; // Magenta, opaque.
    return image;
}
} // namespace MmdLab
