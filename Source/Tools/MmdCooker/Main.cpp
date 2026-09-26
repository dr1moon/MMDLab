#include "Runtime/Asset/MmdlFormat.h"
#include "Runtime/Asset/MmdlWriter.h"
#include "Tools/MmdCooker/PmxFile.h"

#include <windows.h>

#include <cstdio>
#include <exception>

namespace
{
MmdLab::MmdlMeshData ConvertToMmdl(const MmdLab::PmxStaticMesh& pmx)
{
    MmdLab::MmdlMeshData mesh;

    mesh.vertices.reserve(pmx.vertices.size());
    for (const MmdLab::PmxVertex& vertex : pmx.vertices)
    {
        MmdLab::MmdlVertex out{};
        out.position[0] = vertex.position[0];
        out.position[1] = vertex.position[1];
        out.position[2] = vertex.position[2];
        out.position[3] = 1.0f;
        out.normal[0] = vertex.normal[0];
        out.normal[1] = vertex.normal[1];
        out.normal[2] = vertex.normal[2];
        out.normal[3] = 0.0f;
        out.uv[0] = vertex.uv[0];
        out.uv[1] = vertex.uv[1];
        mesh.vertices.push_back(out);
    }

    mesh.indices = pmx.indices;
    mesh.strings = pmx.textures;

    mesh.materials.reserve(pmx.materials.size());
    for (const MmdLab::PmxMaterial& material : pmx.materials)
    {
        MmdLab::Material out{};
        out.baseColor[0] = material.diffuse[0];
        out.baseColor[1] = material.diffuse[1];
        out.baseColor[2] = material.diffuse[2];
        out.baseColor[3] = material.diffuse[3];
        out.specularColor[0] = material.specular[0];
        out.specularColor[1] = material.specular[1];
        out.specularColor[2] = material.specular[2];
        out.specularStrength = material.specularStrength;
        out.ambientColor[0] = material.ambient[0];
        out.ambientColor[1] = material.ambient[1];
        out.ambientColor[2] = material.ambient[2];
        out.edgeColor[0] = material.edgeColor[0];
        out.edgeColor[1] = material.edgeColor[1];
        out.edgeColor[2] = material.edgeColor[2];
        out.edgeColor[3] = material.edgeColor[3];
        out.edgeSize = material.edgeSize;
        out.baseColorTexture = material.textureIndex;
        out.toonTexture = material.toonTextureIndex;
        out.sphereTexture = material.sphereTextureIndex;
        out.flags = material.drawFlags;
        out.sphereMode = material.sphereMode;
        mesh.materials.push_back(out);
    }

    // Each PMX material is one contiguous index range; emit a draw packet per material.
    mesh.drawPackets.reserve(pmx.materials.size());
    std::uint32_t firstIndex = 0;
    for (std::uint32_t i = 0; i < pmx.materials.size(); ++i)
    {
        MmdLab::DrawPacket packet{};
        packet.firstIndex = firstIndex;
        packet.indexCount = static_cast<std::uint32_t>(pmx.materials[i].surfaceCount);
        packet.materialIndex = i;
        firstIndex += packet.indexCount;
        mesh.drawPackets.push_back(packet);
    }

    return mesh;
}
} // namespace

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
        const MmdLab::MmdlMeshData mesh = ConvertToMmdl(pmx);

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
