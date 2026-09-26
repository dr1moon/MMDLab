#pragma once

#include "Runtime/Asset/MmdlFormat.h"

#include <filesystem>

namespace MmdLab
{
// Reads and validates a .mmdl file, returning its mesh data. Throws std::runtime_error on a
// malformed file (bad magic, bad version, out-of-range or overlapping chunks, or
// inconsistent counts).
[[nodiscard]] MmdlMeshData ReadMmdl(const std::filesystem::path& path);
} // namespace MmdLab
