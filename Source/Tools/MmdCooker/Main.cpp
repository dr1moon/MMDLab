#include "Runtime/Asset/ImageLoader.h"
#include "Runtime/Asset/MmdlFormat.h"
#include "Runtime/Asset/MmdlWriter.h"
#include "Runtime/Asset/PmxFile.h"
#include "Runtime/Asset/TextureCooker.h"
#include "Runtime/Core/Utf8.h"

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <vector>

int wmain(const int argc, wchar_t* argv[])
{
    if (argc != 3)
    {
        std::printf("Usage: MmdCooker.exe <input.pmx> <output.mmdl>\n");
        return 1;
    }

    SetConsoleOutputCP(CP_UTF8);

    try
    {
        const MmdLab::PmxStaticMesh pmx = MmdLab::ParsePmxStaticMesh(argv[1]);
        const MmdLab::MmdlMeshData mesh = MmdLab::ConvertPmxToMmdl(pmx);

        MmdLab::WriteMmdl(argv[2], mesh);

        // Cook the textures the model references: decode -> role-aware BC compression -> .mmtex
        // written next to the source image, so the viewer can load them without a codec decode.
        const std::filesystem::path base = std::filesystem::path(argv[1]).parent_path();
        const std::vector<MmdLab::TextureRole> roles =
            MmdLab::ComputeTextureRoles(mesh.materials, mesh.strings.size());
        std::size_t cookedTextures = 0;
        for (std::size_t i = 0; i < mesh.strings.size(); ++i)
        {
            const std::filesystem::path source = base / MmdLab::PathFromUtf8(mesh.strings[i]);
            try
            {
                const MmdLab::Image decoded = MmdLab::DecodeImage(source);
                const MmdLab::Image compressed =
                    MmdLab::CompressToBc(decoded, roles[i], MmdLab::kDefaultTextureCookSettings);
                std::filesystem::path cooked = source;
                cooked.replace_extension(L".mmtex");
                MmdLab::WriteCookedTexture(cooked, compressed);
                ++cookedTextures;
            }
            catch (const std::exception& exception)
            {
                std::printf("  warning: failed to cook texture '%s': %s\n",
                    MmdLab::WideToUtf8(source.filename().wstring()).c_str(), exception.what());
            }
        }

        // Summarize the skinning so the skeleton can be inspected without loading the model. The
        // submesh-local u8 indices have already been remapped into `refBones`, so the summary
        // reads the global bone set directly from the skin-reference-bone table.
        std::uint32_t maxSkinIndex = 0;
        std::vector<char> weighted(mesh.bones.size(), 0);
        for (const std::uint16_t bone : mesh.refBones)
        {
            weighted[static_cast<std::size_t>(bone)] = 1;
            maxSkinIndex = std::max(maxSkinIndex, static_cast<std::uint32_t>(bone));
        }
        std::size_t weightedCount = 0;
        for (const char c : weighted)
        {
            if (c) { ++weightedCount; }
        }

        std::uint32_t maxSubmeshBones = 0;
        std::uint32_t submeshWithMax = 0;
        for (std::size_t s = 0; s < mesh.drawPackets.size(); ++s)
        {
            if (mesh.drawPackets[s].refBoneCount > maxSubmeshBones)
            {
                maxSubmeshBones = mesh.drawPackets[s].refBoneCount;
                submeshWithMax = static_cast<std::uint32_t>(s);
            }
        }

        std::printf("Cooked:\n");
        std::printf("  vertices=%zu triangles=%zu materials=%zu textures=%zu (cooked %zu)\n",
            mesh.vertices.size(),
            mesh.indices.size() / 3,
            mesh.materials.size(),
            mesh.strings.size(),
            cookedTextures);
        std::printf("  bones=%zu skin-referenced=%zu max-skin-index=%u\n",
            mesh.bones.size(),
            weightedCount,
            maxSkinIndex);
        std::printf("  submeshes=%zu max-submesh-bones=%u (submesh %u)\n",
            mesh.drawPackets.size(),
            maxSubmeshBones,
            submeshWithMax);
        return 0;
    }
    catch (const std::exception& exception)
    {
        std::printf("Cook failed: %s\n", exception.what());
        return 1;
    }
}
