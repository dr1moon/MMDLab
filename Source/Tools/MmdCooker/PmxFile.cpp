#include "Tools/MmdCooker/PmxFile.h"

#include <windows.h>

#include <cstring>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>

namespace MmdLab
{
namespace
{
// A little-endian reader over an in-memory byte buffer.
class Reader
{
public:
    Reader(const std::uint8_t* data, const std::size_t size)
        : data_(data)
        , size_(size)
        , offset_(0)
    {
    }

    std::uint8_t ReadU8()
    {
        Check(1);
        return data_[offset_++];
    }

    std::uint16_t ReadU16()
    {
        Check(2);
        const std::uint16_t value = static_cast<std::uint16_t>(data_[offset_])
            | (static_cast<std::uint16_t>(data_[offset_ + 1]) << 8);
        offset_ += 2;
        return value;
    }

    std::uint32_t ReadU32()
    {
        Check(4);
        const std::uint32_t value = static_cast<std::uint32_t>(data_[offset_])
            | (static_cast<std::uint32_t>(data_[offset_ + 1]) << 8)
            | (static_cast<std::uint32_t>(data_[offset_ + 2]) << 16)
            | (static_cast<std::uint32_t>(data_[offset_ + 3]) << 24);
        offset_ += 4;
        return value;
    }

    std::int32_t ReadI32()
    {
        return static_cast<std::int32_t>(ReadU32());
    }

    float ReadF32()
    {
        const std::uint32_t bits = ReadU32();
        float value;
        std::memcpy(&value, &bits, sizeof(value));
        return value;
    }

    void ReadBytes(std::uint8_t* out, const std::size_t count)
    {
        Check(count);
        std::memcpy(out, data_ + offset_, count);
        offset_ += count;
    }

    void Skip(const std::size_t count)
    {
        Check(count);
        offset_ += count;
    }

private:
    void Check(const std::size_t count) const
    {
        if (count > size_ - offset_)
        {
            throw std::runtime_error(
                "PMX parse: unexpected end of file (offset " + std::to_string(offset_)
                + " of " + std::to_string(size_) + ").");
        }
    }

    const std::uint8_t* data_;
    std::size_t size_;
    std::size_t offset_;
};

std::string Utf16LeToUtf8(const std::uint8_t* data, const std::size_t byteLength)
{
    if (byteLength == 0)
    {
        return {};
    }

    const int wideLength = static_cast<int>(byteLength / 2);
    const auto* wide = reinterpret_cast<const wchar_t*>(data);
    const int utf8Length = WideCharToMultiByte(CP_UTF8, 0, wide, wideLength, nullptr, 0, nullptr, nullptr);
    std::string utf8(static_cast<std::size_t>(utf8Length), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide, wideLength, utf8.data(), utf8Length, nullptr, nullptr);
    return utf8;
}

std::string ReadString(Reader& reader, const bool utf8)
{
    const std::int32_t byteLength = reader.ReadI32();
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(byteLength));
    reader.ReadBytes(bytes.data(), bytes.size());

    if (utf8)
    {
        return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    }
    return Utf16LeToUtf8(bytes.data(), bytes.size());
}

// Reads a signed 1/2/4-byte index and sign-extends it to int32.
std::int32_t ReadIndex(Reader& reader, const std::uint8_t size)
{
    switch (size)
    {
    case 1: return static_cast<std::int32_t>(static_cast<std::int8_t>(reader.ReadU8()));
    case 2: return static_cast<std::int32_t>(static_cast<std::int16_t>(reader.ReadU16()));
    case 4: return reader.ReadI32();
    default: throw std::runtime_error("Unsupported index size.");
    }
}
} // namespace

PmxStaticMesh ParsePmxStaticMesh(const std::filesystem::path& path)
{
    std::ifstream file(path, std::ios::binary);
    if (!file)
    {
        throw std::runtime_error("Failed to open the PMX file.");
    }
    std::vector<std::uint8_t> data(
        (std::istreambuf_iterator<char>(file)),
        std::istreambuf_iterator<char>());

    Reader reader(data.data(), data.size());

    // Header.
    const char magic[4] = {
        static_cast<char>(reader.ReadU8()),
        static_cast<char>(reader.ReadU8()),
        static_cast<char>(reader.ReadU8()),
        static_cast<char>(reader.ReadU8()),
    };
    if (std::memcmp(magic, "PMX ", 4) != 0)
    {
        throw std::runtime_error("Not a PMX file (bad magic).");
    }

    const float version = reader.ReadF32();
    if (version < 2.0f || version > 2.1f)
    {
        throw std::runtime_error("Unsupported PMX version.");
    }

    (void)reader.ReadU8(); // globals count (always 8).
    const std::uint8_t textEncoding = reader.ReadU8();
    const std::uint8_t additionalUvCount = reader.ReadU8();
    const std::uint8_t vertexIndexSize = reader.ReadU8();
    const std::uint8_t textureIndexSize = reader.ReadU8();
    (void)reader.ReadU8(); // material index size (unused for static geometry).
    const std::uint8_t boneIndexSize = reader.ReadU8();
    (void)reader.ReadU8(); // morph index size.
    (void)reader.ReadU8(); // rigid body index size.

    const bool utf8 = textEncoding == 1;

    // Model info.
    PmxStaticMesh mesh;
    mesh.modelName = ReadString(reader, utf8);
    (void)ReadString(reader, utf8); // model name EN.
    (void)ReadString(reader, utf8); // comment.
    (void)ReadString(reader, utf8); // comment EN.

    // Vertices.
    const std::uint32_t vertexCount = reader.ReadU32();
    mesh.vertices.reserve(vertexCount);
    for (std::uint32_t i = 0; i < vertexCount; ++i)
    {
        PmxVertex vertex{};
        vertex.position[0] = reader.ReadF32();
        vertex.position[1] = reader.ReadF32();
        vertex.position[2] = reader.ReadF32();
        vertex.normal[0] = reader.ReadF32();
        vertex.normal[1] = reader.ReadF32();
        vertex.normal[2] = reader.ReadF32();
        vertex.uv[0] = reader.ReadF32();
        vertex.uv[1] = reader.ReadF32();

        // Additional UVs (a vec4 each).
        reader.Skip(static_cast<std::size_t>(additionalUvCount) * 4 * 4);

        // Skinning is variable-length; skip it for static geometry.
        const std::uint8_t skinningType = reader.ReadU8();
        switch (skinningType)
        {
        case 0: reader.Skip(static_cast<std::size_t>(boneIndexSize) * 1); break;         // BDEF1.
        case 1: reader.Skip(static_cast<std::size_t>(boneIndexSize) * 2 + 4); break;     // BDEF2.
        case 2: reader.Skip(static_cast<std::size_t>(boneIndexSize) * 4 + 16); break;    // BDEF4.
        case 3: reader.Skip(static_cast<std::size_t>(boneIndexSize) * 4 + 52); break;    // SDEF.
        case 4: reader.Skip(static_cast<std::size_t>(boneIndexSize) * 4 + 16); break;    // QDEF.
        default: throw std::runtime_error("Unknown skinning type.");
        }

        reader.Skip(4); // edge scale.
        mesh.vertices.push_back(vertex);
    }

    // Indices.
    const std::uint32_t indexCount = reader.ReadU32();
    mesh.indices.reserve(indexCount);
    for (std::uint32_t i = 0; i < indexCount; ++i)
    {
        std::uint32_t index = 0;
        switch (vertexIndexSize)
        {
        case 1: index = reader.ReadU8(); break;
        case 2: index = reader.ReadU16(); break;
        case 4: index = reader.ReadU32(); break;
        default: throw std::runtime_error("Unsupported vertex index size.");
        }
        mesh.indices.push_back(index);
    }

    // Textures.
    const std::uint32_t textureCount = reader.ReadU32();
    mesh.textures.reserve(textureCount);
    for (std::uint32_t i = 0; i < textureCount; ++i)
    {
        mesh.textures.push_back(ReadString(reader, utf8));
    }

    // Materials.
    const std::uint32_t materialCount = reader.ReadU32();
    mesh.materials.reserve(materialCount);
    for (std::uint32_t i = 0; i < materialCount; ++i)
    {
        PmxMaterial material{};
        (void)ReadString(reader, utf8); // material name (local).
        (void)ReadString(reader, utf8); // material name (universal).
        material.diffuse[0] = reader.ReadF32();
        material.diffuse[1] = reader.ReadF32();
        material.diffuse[2] = reader.ReadF32();
        material.diffuse[3] = reader.ReadF32();
        material.specular[0] = reader.ReadF32();
        material.specular[1] = reader.ReadF32();
        material.specular[2] = reader.ReadF32();
        material.specularStrength = reader.ReadF32();
        material.ambient[0] = reader.ReadF32();
        material.ambient[1] = reader.ReadF32();
        material.ambient[2] = reader.ReadF32();
        material.drawFlags = reader.ReadU8();
        material.edgeColor[0] = reader.ReadF32();
        material.edgeColor[1] = reader.ReadF32();
        material.edgeColor[2] = reader.ReadF32();
        material.edgeColor[3] = reader.ReadF32();
        material.edgeSize = reader.ReadF32();

        material.textureIndex = ReadIndex(reader, textureIndexSize);
        material.sphereTextureIndex = ReadIndex(reader, textureIndexSize);
        material.sphereMode = reader.ReadU8();

        const std::uint8_t toonFlag = reader.ReadU8();
        if (toonFlag == 0)
        {
            reader.Skip(1); // shared toon index (unused).
        }
        else
        {
            material.toonTextureIndex = ReadIndex(reader, textureIndexSize);
        }

        (void)ReadString(reader, utf8); // memo.
        material.surfaceCount = reader.ReadI32();
        mesh.materials.push_back(material);
    }

    // Validate: each triangle index is in range, and material surface counts cover the
    // whole index buffer exactly.
    for (const std::uint32_t index : mesh.indices)
    {
        if (index >= vertexCount)
        {
            throw std::runtime_error("Vertex index out of range.");
        }
    }

    std::uint32_t totalSurfaceCount = 0;
    for (const PmxMaterial& material : mesh.materials)
    {
        totalSurfaceCount += static_cast<std::uint32_t>(material.surfaceCount);
    }
    if (totalSurfaceCount != indexCount)
    {
        throw std::runtime_error("Material surface counts do not sum to the index count.");
    }

    return mesh;
}
} // namespace MmdLab
