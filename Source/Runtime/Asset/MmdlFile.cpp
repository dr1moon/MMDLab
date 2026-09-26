#include "Runtime/Asset/MmdlFile.h"

#include <cstring>
#include <fstream>
#include <iterator>
#include <stdexcept>

namespace MmdLab
{
namespace
{
class Reader
{
public:
    Reader(const std::uint8_t* data, const std::size_t size)
        : data_(data)
        , size_(size)
        , offset_(0)
    {
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

private:
    void Check(const std::size_t count) const
    {
        if (count > size_ - offset_)
        {
            throw std::runtime_error(".mmdl parse: unexpected end of file.");
        }
    }

    const std::uint8_t* data_;
    std::size_t size_;
    std::size_t offset_;
};
} // namespace

MmdlMeshData ReadMmdl(const std::filesystem::path& path)
{
    std::ifstream file(path, std::ios::binary);
    if (!file)
    {
        throw std::runtime_error("Failed to open the .mmdl file.");
    }
    std::vector<std::uint8_t> data(
        (std::istreambuf_iterator<char>(file)),
        std::istreambuf_iterator<char>());

    if (data.size() < sizeof(MmdlHeader))
    {
        throw std::runtime_error(".mmdl file is too small.");
    }

    MmdlHeader header{};
    std::memcpy(&header, data.data(), sizeof(header));
    if (header.magic != MmdlMagic)
    {
        throw std::runtime_error("Not a .mmdl file (bad magic).");
    }
    if (header.version != MmdlVersion)
    {
        throw std::runtime_error("Unsupported .mmdl version.");
    }
    if (header.totalFileSize != data.size())
    {
        throw std::runtime_error(".mmdl total file size does not match.");
    }

    const std::size_t chunkTableSize = static_cast<std::size_t>(header.chunkCount) * sizeof(MmdlChunkDescriptor);
    if (header.chunkTableOffset > data.size() || chunkTableSize > data.size() - header.chunkTableOffset)
    {
        throw std::runtime_error(".mmdl chunk table is out of range.");
    }

    std::vector<MmdlChunkDescriptor> descriptors(header.chunkCount);
    std::memcpy(descriptors.data(), data.data() + header.chunkTableOffset, chunkTableSize);

    // Validate chunk offsets and sizes (in range, no overlap).
    std::uint64_t previousEnd = 0;
    for (const MmdlChunkDescriptor& descriptor : descriptors)
    {
        if (descriptor.offset > data.size() || descriptor.size > data.size() - descriptor.offset)
        {
            throw std::runtime_error(".mmdl chunk is out of range.");
        }
        if (descriptor.offset < previousEnd)
        {
            throw std::runtime_error(".mmdl chunks overlap.");
        }
        previousEnd = descriptor.offset + descriptor.size;
    }

    MmdlMeshData mesh;
    const MmdlChunkDescriptor* vertexChunk = nullptr;
    const MmdlChunkDescriptor* indexChunk = nullptr;
    const MmdlChunkDescriptor* materialChunk = nullptr;
    const MmdlChunkDescriptor* subMeshChunk = nullptr;

    for (const MmdlChunkDescriptor& descriptor : descriptors)
    {
        const std::uint8_t* chunk = data.data() + descriptor.offset;
        const auto type = static_cast<MmdlChunkType>(descriptor.type);

        switch (type)
        {
        case MmdlChunkType::StringTable:
        {
            Reader reader(chunk, static_cast<std::size_t>(descriptor.size));
            const std::uint32_t count = reader.ReadU32();
            mesh.strings.reserve(count);
            for (std::uint32_t i = 0; i < count; ++i)
            {
                const std::uint32_t length = reader.ReadU32();
                std::string text(length, '\0');
                reader.ReadBytes(reinterpret_cast<std::uint8_t*>(text.data()), length);
                mesh.strings.push_back(std::move(text));
            }
            break;
        }
        case MmdlChunkType::VertexBuffer:
            vertexChunk = &descriptor;
            break;
        case MmdlChunkType::IndexBuffer:
            indexChunk = &descriptor;
            break;
        case MmdlChunkType::MaterialTable:
            materialChunk = &descriptor;
            break;
        case MmdlChunkType::SubMeshTable:
            subMeshChunk = &descriptor;
            break;
        case MmdlChunkType::MeshMetadata:
            break; // Counts are re-derived from the buffer chunks below.
        }
    }

    if (vertexChunk == nullptr || indexChunk == nullptr || materialChunk == nullptr || subMeshChunk == nullptr)
    {
        throw std::runtime_error(".mmdl is missing a required chunk.");
    }

    // Vertices.
    const std::size_t vertexStride = sizeof(MmdlVertex);
    if (vertexChunk->size % vertexStride != 0)
    {
        throw std::runtime_error(".mmdl vertex buffer has a partial vertex.");
    }
    const std::size_t vertexCount = static_cast<std::size_t>(vertexChunk->size) / vertexStride;
    mesh.vertices.resize(vertexCount);
    std::memcpy(mesh.vertices.data(), data.data() + vertexChunk->offset, vertexChunk->size);

    // Indices.
    if (indexChunk->size % sizeof(std::uint32_t) != 0)
    {
        throw std::runtime_error(".mmdl index buffer has a partial index.");
    }
    const std::size_t indexCount = static_cast<std::size_t>(indexChunk->size) / sizeof(std::uint32_t);
    mesh.indices.resize(indexCount);
    std::memcpy(mesh.indices.data(), data.data() + indexChunk->offset, indexChunk->size);

    // Materials.
    const std::size_t materialStride = sizeof(Material);
    if (materialChunk->size % materialStride != 0)
    {
        throw std::runtime_error(".mmdl material table has a partial material.");
    }
    const std::size_t materialCount = static_cast<std::size_t>(materialChunk->size) / materialStride;
    mesh.materials.resize(materialCount);
    std::memcpy(mesh.materials.data(), data.data() + materialChunk->offset, materialChunk->size);

    // Sub-meshes (draw ranges).
    const std::size_t subMeshStride = sizeof(DrawPacket);
    if (subMeshChunk->size % subMeshStride != 0)
    {
        throw std::runtime_error(".mmdl sub-mesh table has a partial sub-mesh.");
    }
    const std::size_t subMeshCount = static_cast<std::size_t>(subMeshChunk->size) / subMeshStride;
    mesh.drawPackets.resize(subMeshCount);
    std::memcpy(mesh.drawPackets.data(), data.data() + subMeshChunk->offset, subMeshChunk->size);

    // Validate indices and sub-mesh ranges.
    for (const std::uint32_t index : mesh.indices)
    {
        if (index >= vertexCount)
        {
            throw std::runtime_error(".mmdl vertex index out of range.");
        }
    }
    std::uint64_t totalIndices = 0;
    for (const DrawPacket& packet : mesh.drawPackets)
    {
        if (packet.firstIndex + static_cast<std::uint64_t>(packet.indexCount) > indexCount)
        {
            throw std::runtime_error(".mmdl sub-mesh draw range is out of range.");
        }
        if (packet.materialIndex >= materialCount)
        {
            throw std::runtime_error(".mmdl sub-mesh material index is out of range.");
        }
        totalIndices += packet.indexCount;
    }
    if (totalIndices != indexCount)
    {
        throw std::runtime_error(".mmdl sub-mesh ranges do not sum to the index count.");
    }

    return mesh;
}
} // namespace MmdLab
