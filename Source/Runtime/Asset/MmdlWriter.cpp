#include "Runtime/Asset/MmdlWriter.h"

#include <cstring>
#include <fstream>
#include <limits>
#include <stdexcept>

namespace MmdLab
{
namespace
{
// Appends little-endian bytes to a buffer.
class ByteWriter
{
public:
    void U8(const std::uint8_t value) { data_.push_back(value); }

    void U32(const std::uint32_t value)
    {
        for (int i = 0; i < 4; ++i)
        {
            data_.push_back(static_cast<std::uint8_t>((value >> (8 * i)) & 0xFF));
        }
    }

    void I32(const std::int32_t value) { U32(static_cast<std::uint32_t>(value)); }

    void U64(const std::uint64_t value)
    {
        for (int i = 0; i < 8; ++i)
        {
            data_.push_back(static_cast<std::uint8_t>((value >> (8 * i)) & 0xFF));
        }
    }

    void F32(const float value)
    {
        std::uint32_t bits;
        std::memcpy(&bits, &value, sizeof(bits));
        U32(bits);
    }

    void Bytes(const void* data, const std::size_t count)
    {
        const auto* bytes = static_cast<const std::uint8_t*>(data);
        data_.insert(data_.end(), bytes, bytes + count);
    }

    const std::vector<std::uint8_t>& Data() const { return data_; }

private:
    std::vector<std::uint8_t> data_;
};
} // namespace

void WriteMmdl(const std::filesystem::path& path, const MmdlMeshData& mesh)
{
    // Serialize each chunk.
    ByteWriter stringTable;
    stringTable.U32(static_cast<std::uint32_t>(mesh.strings.size()));
    for (const std::string& text : mesh.strings)
    {
        stringTable.U32(static_cast<std::uint32_t>(text.size()));
        stringTable.Bytes(text.data(), text.size());
    }

    MmdlMeshMetadata metadata{};
    metadata.vertexCount = static_cast<std::uint32_t>(mesh.vertices.size());
    metadata.indexCount = static_cast<std::uint32_t>(mesh.indices.size());
    metadata.materialCount = static_cast<std::uint32_t>(mesh.materials.size());
    metadata.subMeshCount = static_cast<std::uint32_t>(mesh.drawPackets.size());
    metadata.vertexStride = sizeof(MmdlVertex);

    float boundsMin[3] = { std::numeric_limits<float>::infinity(),
                           std::numeric_limits<float>::infinity(),
                           std::numeric_limits<float>::infinity() };
    float boundsMax[3] = { -std::numeric_limits<float>::infinity(),
                           -std::numeric_limits<float>::infinity(),
                           -std::numeric_limits<float>::infinity() };
    for (const MmdlVertex& vertex : mesh.vertices)
    {
        for (int axis = 0; axis < 3; ++axis)
        {
            boundsMin[axis] = std::min(boundsMin[axis], vertex.position[axis]);
            boundsMax[axis] = std::max(boundsMax[axis], vertex.position[axis]);
        }
    }
    for (int axis = 0; axis < 3; ++axis)
    {
        metadata.boundsMin[axis] = boundsMin[axis];
        metadata.boundsMax[axis] = boundsMax[axis];
    }

    ByteWriter vertexBuffer;
    vertexBuffer.Bytes(mesh.vertices.data(), mesh.vertices.size() * sizeof(MmdlVertex));

    ByteWriter indexBuffer;
    for (const std::uint32_t index : mesh.indices)
    {
        indexBuffer.U32(index);
    }

    ByteWriter materialTable;
    for (const Material& material : mesh.materials)
    {
        for (const float component : material.baseColor)
        {
            materialTable.F32(component);
        }
        for (const float component : material.specularColor)
        {
            materialTable.F32(component);
        }
        materialTable.F32(material.specularStrength);
        for (const float component : material.ambientColor)
        {
            materialTable.F32(component);
        }
        for (const float component : material.edgeColor)
        {
            materialTable.F32(component);
        }
        materialTable.F32(material.edgeSize);
        materialTable.I32(material.baseColorTexture);
        materialTable.I32(material.toonTexture);
        materialTable.I32(material.sphereTexture);
        materialTable.U32(material.flags);
        materialTable.U32(material.sphereMode);
    }

    ByteWriter subMeshTable;
    for (const DrawPacket& packet : mesh.drawPackets)
    {
        subMeshTable.U32(packet.firstIndex);
        subMeshTable.U32(packet.indexCount);
        subMeshTable.U32(packet.materialIndex);
    }

    // Assemble the file: header, chunk table, then the chunks.
    const std::uint32_t chunkCount = 6;
    const std::uint64_t chunkTableOffset = sizeof(MmdlHeader);

    // Serialize the metadata chunk.
    ByteWriter metadataBytes;
    metadataBytes.U32(metadata.vertexCount);
    metadataBytes.U32(metadata.indexCount);
    metadataBytes.U32(metadata.materialCount);
    metadataBytes.U32(metadata.subMeshCount);
    metadataBytes.U32(metadata.vertexStride);
    for (const float value : metadata.boundsMin)
    {
        metadataBytes.F32(value);
    }
    for (const float value : metadata.boundsMax)
    {
        metadataBytes.F32(value);
    }

    const ByteWriter* chunkPayloads[] = { &stringTable, &metadataBytes, &vertexBuffer, &indexBuffer, &materialTable, &subMeshTable };
    const std::uint32_t chunkTypes[] = {
        static_cast<std::uint32_t>(MmdlChunkType::StringTable),
        static_cast<std::uint32_t>(MmdlChunkType::MeshMetadata),
        static_cast<std::uint32_t>(MmdlChunkType::VertexBuffer),
        static_cast<std::uint32_t>(MmdlChunkType::IndexBuffer),
        static_cast<std::uint32_t>(MmdlChunkType::MaterialTable),
        static_cast<std::uint32_t>(MmdlChunkType::SubMeshTable),
    };

    std::uint64_t dataOffset = chunkTableOffset + static_cast<std::uint64_t>(chunkCount) * sizeof(MmdlChunkDescriptor);
    std::vector<MmdlChunkDescriptor> descriptors(chunkCount);
    for (std::uint32_t i = 0; i < chunkCount; ++i)
    {
        descriptors[i].type = chunkTypes[i];
        descriptors[i].version = 1;
        descriptors[i].offset = dataOffset;
        descriptors[i].size = chunkPayloads[i]->Data().size();
        descriptors[i].alignment = 4;
        descriptors[i].reserved = 0;
        dataOffset += descriptors[i].size;
    }

    const std::uint64_t totalFileSize = dataOffset;

    std::ofstream file(path, std::ios::binary);
    if (!file)
    {
        throw std::runtime_error("Failed to create the .mmdl file.");
    }

    MmdlHeader header{};
    header.magic = MmdlMagic;
    header.version = MmdlVersion;
    header.chunkTableOffset = chunkTableOffset;
    header.chunkCount = chunkCount;
    header.reserved = 0;
    header.totalFileSize = totalFileSize;
    file.write(reinterpret_cast<const char*>(&header), sizeof(header));
    file.write(reinterpret_cast<const char*>(descriptors.data()), descriptors.size() * sizeof(MmdlChunkDescriptor));
    for (std::uint32_t i = 0; i < chunkCount; ++i)
    {
        const auto& bytes = chunkPayloads[i]->Data();
        file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    }
}
} // namespace MmdLab
