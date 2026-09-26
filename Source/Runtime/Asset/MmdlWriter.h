#pragma once

#include "Runtime/Asset/MmdlFormat.h"

#include <filesystem>

namespace MmdLab
{
// Serializes a mesh to a .mmdl file. Throws std::runtime_error on failure.
void WriteMmdl(const std::filesystem::path& path, const MmdlMeshData& mesh);
} // namespace MmdLab
