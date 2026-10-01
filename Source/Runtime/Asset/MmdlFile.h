#pragma once

#include "Runtime/Asset/MmdlFormat.h"

#include <cstdint>
#include <filesystem>
#include <span>
#include <vector>

namespace MmdLab
{
// Reads a .mmdl file's raw bytes. Throws std::runtime_error when the file cannot be opened or
// sized.
[[nodiscard]] std::vector<std::uint8_t> ReadMmdlBytes(const std::filesystem::path& path);

// Parses and validates a .mmdl byte buffer, returning its mesh data. Throws std::runtime_error on
// a malformed buffer (bad magic, bad version, out-of-range or overlapping chunks, or inconsistent
// counts). The buffer is read only during the call.
[[nodiscard]] MmdlMeshData ParseMmdl(std::span<const std::uint8_t> bytes);

// Reads and parses a .mmdl file (ReadMmdlBytes + ParseMmdl).
[[nodiscard]] MmdlMeshData ReadMmdl(const std::filesystem::path& path);
} // namespace MmdLab
