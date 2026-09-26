#include "Runtime/Asset/MmdlFormat.h"
#include "Runtime/Asset/MmdlWriter.h"
#include "Runtime/Asset/PmxFile.h"

#include <windows.h>

#include <cstdio>
#include <exception>

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

        std::printf("Cooked:\n");
        std::printf("  vertices=%zu triangles=%zu materials=%zu textures=%zu\n",
            mesh.vertices.size(),
            mesh.indices.size() / 3,
            mesh.materials.size(),
            mesh.strings.size());
        return 0;
    }
    catch (const std::exception& exception)
    {
        std::printf("Cook failed: %s\n", exception.what());
        return 1;
    }
}
