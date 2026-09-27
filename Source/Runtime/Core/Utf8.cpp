#include "Runtime/Core/Utf8.h"

#include <windows.h>

#include <cstddef>
#include <string>

namespace MmdLab
{
std::filesystem::path PathFromUtf8(const std::string& utf8)
{
    const int length = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), static_cast<int>(utf8.size()), nullptr, 0);
    std::wstring wide(static_cast<std::size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), static_cast<int>(utf8.size()), wide.data(), length);
    return std::filesystem::path(wide);
}

std::string WideToUtf8(const std::wstring& wide)
{
    const int length = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
    std::string utf8(static_cast<std::size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), static_cast<int>(wide.size()), utf8.data(), length, nullptr, nullptr);
    return utf8;
}
} // namespace MmdLab
