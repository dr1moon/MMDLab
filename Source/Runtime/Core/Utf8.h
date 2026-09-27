#pragma once

#include <filesystem>
#include <string>

namespace MmdLab
{
// Converts a UTF-8 string (a PMX string-table entry) into a wide filesystem path.
[[nodiscard]] std::filesystem::path PathFromUtf8(const std::string& utf8);

// Converts a wide filename into a UTF-8 display string.
[[nodiscard]] std::string WideToUtf8(const std::wstring& wide);
} // namespace MmdLab
