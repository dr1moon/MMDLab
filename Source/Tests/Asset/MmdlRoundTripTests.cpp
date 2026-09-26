#include "Runtime/Asset/MmdlFile.h"
#include "Runtime/Asset/MmdlFormat.h"
#include "Runtime/Asset/MmdlWriter.h"
#include "Runtime/Core/TestFramework.h"

#include <filesystem>

namespace
{
MmdLab::MmdlMeshData MakeTestMesh()
{
    MmdLab::MmdlMeshData mesh;

    // A unit quad: 4 vertices, 2 triangles, split across 2 materials.
    mesh.vertices = {
        { { 0.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, 1.0f }, { 0.0f, 0.0f } },
        { { 1.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, 1.0f }, { 1.0f, 0.0f } },
        { { 1.0f, 1.0f, 0.0f }, { 0.0f, 0.0f, 1.0f }, { 1.0f, 1.0f } },
        { { 0.0f, 1.0f, 0.0f }, { 0.0f, 0.0f, 1.0f }, { 0.0f, 1.0f } },
    };
    mesh.indices = { 0, 1, 2, 0, 2, 3 };

    MmdLab::Material red;
    red.baseColor[0] = 1.0f;
    red.baseColor[1] = 0.0f;
    red.baseColor[2] = 0.0f;
    red.baseColor[3] = 1.0f;
    red.specularStrength = 0.5f;
    red.baseColorTexture = 0;
    red.sphereTexture = 1;
    red.sphereMode = 2u; // sphere add.
    red.flags = 0x01u; // double-sided.

    MmdLab::Material green;
    green.baseColor[0] = 0.0f;
    green.baseColor[1] = 1.0f;
    green.baseColor[2] = 0.0f;
    green.baseColor[3] = 1.0f;
    green.toonTexture = 0;
    green.sphereMode = 1u; // sphere multiply.
    green.flags = 0x00u; // single-sided.

    mesh.materials = { red, green };
    mesh.drawPackets = {
        { 0, 3, 0 }, // first triangle -> red.
        { 3, 3, 1 }, // second triangle -> green.
    };
    mesh.strings = { "red.png", "green.png" };
    return mesh;
}
} // namespace

MMDLAB_TEST(Asset.Mmdl, RoundTripPreservesMesh)
{
    const MmdLab::MmdlMeshData expected = MakeTestMesh();
    const std::filesystem::path path = std::filesystem::temp_directory_path() / "mmdl_roundtrip.mmdl";

    MmdLab::WriteMmdl(path, expected);
    const MmdLab::MmdlMeshData actual = MmdLab::ReadMmdl(path);
    std::filesystem::remove(path);

    MMDLAB_CHECK_EQUAL(expected.vertices.size(), actual.vertices.size());
    MMDLAB_CHECK_EQUAL(expected.indices.size(), actual.indices.size());
    MMDLAB_CHECK_EQUAL(expected.materials.size(), actual.materials.size());
    MMDLAB_CHECK_EQUAL(expected.drawPackets.size(), actual.drawPackets.size());
    MMDLAB_CHECK_EQUAL(expected.strings.size(), actual.strings.size());

    for (std::size_t i = 0; i < expected.vertices.size(); ++i)
    {
        MMDLAB_CHECK_EQUAL(expected.vertices[i].position[0], actual.vertices[i].position[0]);
        MMDLAB_CHECK_EQUAL(expected.vertices[i].position[1], actual.vertices[i].position[1]);
        MMDLAB_CHECK_EQUAL(expected.vertices[i].position[2], actual.vertices[i].position[2]);
        MMDLAB_CHECK_EQUAL(expected.vertices[i].uv[0], actual.vertices[i].uv[0]);
        MMDLAB_CHECK_EQUAL(expected.vertices[i].uv[1], actual.vertices[i].uv[1]);
    }

    for (std::size_t i = 0; i < expected.indices.size(); ++i)
    {
        MMDLAB_CHECK_EQUAL(expected.indices[i], actual.indices[i]);
    }

    for (std::size_t i = 0; i < expected.materials.size(); ++i)
    {
        for (int c = 0; c < 4; ++c)
        {
            MMDLAB_CHECK_EQUAL(expected.materials[i].baseColor[c], actual.materials[i].baseColor[c]);
        }
        MMDLAB_CHECK_EQUAL(expected.materials[i].specularStrength, actual.materials[i].specularStrength);
        MMDLAB_CHECK_EQUAL(expected.materials[i].baseColorTexture, actual.materials[i].baseColorTexture);
        MMDLAB_CHECK_EQUAL(expected.materials[i].toonTexture, actual.materials[i].toonTexture);
        MMDLAB_CHECK_EQUAL(expected.materials[i].sphereTexture, actual.materials[i].sphereTexture);
        MMDLAB_CHECK_EQUAL(expected.materials[i].sphereMode, actual.materials[i].sphereMode);
        MMDLAB_CHECK_EQUAL(expected.materials[i].flags, actual.materials[i].flags);
    }

    for (std::size_t i = 0; i < expected.drawPackets.size(); ++i)
    {
        MMDLAB_CHECK_EQUAL(expected.drawPackets[i].firstIndex, actual.drawPackets[i].firstIndex);
        MMDLAB_CHECK_EQUAL(expected.drawPackets[i].indexCount, actual.drawPackets[i].indexCount);
        MMDLAB_CHECK_EQUAL(expected.drawPackets[i].materialIndex, actual.drawPackets[i].materialIndex);
    }

    MMDLAB_CHECK_EQUAL(expected.strings[0], actual.strings[0]);
    MMDLAB_CHECK_EQUAL(expected.strings[1], actual.strings[1]);
}
