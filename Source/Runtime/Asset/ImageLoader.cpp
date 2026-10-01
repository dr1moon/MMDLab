#include "Runtime/Asset/ImageLoader.h"

// stb_image: single-header image decoder (MIT / public domain). Kept as the fallback for formats
// WIC cannot decode (or when WIC is unavailable). Only stbi_load_from_memory is used: the file is
// read by ReadImageBytes, so the path-based loaders (and their UTF-8 path handling) are not needed.
#define STB_IMAGE_IMPLEMENTATION
#include "ThirdParty/stb/stb_image.h"

#include "tracy/Tracy.hpp"

#include <windows.h>

#include <objbase.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <cstring>
#include <fstream>
#include <stdexcept>
#include <string>

#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "ole32.lib")

namespace MmdLab
{
namespace
{
// WIC needs COM on the calling thread. The asset I/O workers and the cooker call
// DecodeImageFromBytes, so initialize the multi-threaded apartment lazily, once per thread.
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
// (leaving `image` untouched) when WIC cannot decode the bytes.
bool TryDecodeWic(const std::span<const std::uint8_t> bytes, Image& image)
{
    using Microsoft::WRL::ComPtr;

    EnsureComInitialized();

    ComPtr<IWICImagingFactory> factory;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&factory))))
    {
        return false;
    }

    // Decode from the in-memory bytes so the caller, not WIC, owns the file read.
    ComPtr<IWICStream> stream;
    if (FAILED(factory->CreateStream(&stream)))
    {
        return false;
    }
    if (FAILED(stream->InitializeFromMemory(const_cast<WICInProcPointer>(bytes.data()),
                                            static_cast<DWORD>(bytes.size()))))
    {
        return false;
    }

    ComPtr<IWICBitmapDecoder> decoder;
    if (FAILED(factory->CreateDecoderFromStream(stream.Get(), nullptr,
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

std::vector<std::uint8_t> ReadImageBytes(const std::filesystem::path& path)
{
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file)
    {
        throw std::runtime_error("Failed to open the texture image file.");
    }
    const std::streamsize fileSize = file.tellg();
    if (fileSize < 0)
    {
        throw std::runtime_error("Failed to size the texture image file.");
    }
    file.seekg(0, std::ios::beg);
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(fileSize));
    if (fileSize > 0)
    {
        file.read(reinterpret_cast<char*>(bytes.data()), fileSize);
    }
    return bytes;
}

Image DecodeImageFromBytes(const std::span<const std::uint8_t> bytes)
{
    ZoneScopedN("DecodeImage");
    Image image;
    if (TryDecodeWic(bytes, image))
    {
        return image;
    }

    // Fall back to stb_image for formats WIC cannot decode.
    int width = 0;
    int height = 0;
    int channels = 0;
    stbi_uc* pixels = stbi_load_from_memory(
        bytes.data(), static_cast<int>(bytes.size()), &width, &height, &channels, 4); // 4 = force RGBA8.
    if (pixels == nullptr)
    {
        const char* reason = stbi_failure_reason();
        throw std::runtime_error(
            std::string("Failed to decode texture image: ") + (reason != nullptr ? reason : "unknown error"));
    }

    image.width = static_cast<std::uint32_t>(width);
    image.height = static_cast<std::uint32_t>(height);
    image.pixels.assign(pixels, pixels + static_cast<std::size_t>(width) * height * 4);
    stbi_image_free(pixels);
    return image;
}

Image DecodeImage(const std::filesystem::path& path)
{
    return DecodeImageFromBytes(ReadImageBytes(path));
}

Image MissingTextureImage()
{
    Image image;
    image.width = 1;
    image.height = 1;
    image.pixels = { 255, 0, 255, 255 }; // Magenta, opaque.
    return image;
}

std::vector<std::uint8_t> ReadCookedTextureBytes(const std::filesystem::path& path)
{
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file)
    {
        throw std::runtime_error("Failed to open the cooked texture file.");
    }
    const std::streamsize fileSize = file.tellg();
    if (fileSize < 0)
    {
        throw std::runtime_error("Failed to size the cooked texture file.");
    }
    file.seekg(0, std::ios::beg);
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(fileSize));
    if (fileSize > 0)
    {
        file.read(reinterpret_cast<char*>(bytes.data()), fileSize);
    }
    return bytes;
}

Image DecodeCookedTexture(const std::span<const std::uint8_t> bytes)
{
    if (bytes.size() < sizeof(CookedTextureHeader))
    {
        throw std::runtime_error("Cooked texture is too small.");
    }

    CookedTextureHeader header{};
    std::memcpy(&header, bytes.data(), sizeof(header));
    if (header.magic != CookedTextureMagic)
    {
        throw std::runtime_error("Not a cooked texture (bad magic).");
    }
    if (header.version != CookedTextureVersion)
    {
        throw std::runtime_error("Unsupported cooked texture version.");
    }
    if (header.format > static_cast<std::uint32_t>(TextureFormat::BC7))
    {
        throw std::runtime_error("Unsupported cooked texture format.");
    }
    if (bytes.size() < sizeof(header) + header.dataBytes)
    {
        throw std::runtime_error("Cooked texture data is truncated.");
    }

    // Copy the pixel/block data straight into the image (one memcpy, no decode).
    Image image;
    image.width = header.width;
    image.height = header.height;
    image.format = static_cast<TextureFormat>(header.format);
    image.pixels.assign(bytes.data() + sizeof(header), bytes.data() + sizeof(header) + header.dataBytes);
    return image;
}

Image LoadTexture(const std::filesystem::path& path)
{
    std::filesystem::path cookedPath = path;
    cookedPath.replace_extension(L".mmtex");
    if (std::filesystem::exists(cookedPath))
    {
        try
        {
            return DecodeCookedTexture(ReadCookedTextureBytes(cookedPath));
        }
        catch (const std::exception&)
        {
            // Stale or corrupt; fall through and decode.
        }
    }

    // No current .mmtex: decode into RGBA8. Block compression is the asset pipeline's job, not the
    // runtime's.
    return DecodeImageFromBytes(ReadImageBytes(path));
}
} // namespace MmdLab
