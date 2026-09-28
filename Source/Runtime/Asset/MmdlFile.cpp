#include "Runtime/Asset/MmdlFile.h"

#include <cstring>
#include <fstream>
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

    std::uint16_t ReadU16()
    {
        Check(2);
        const std::uint16_t value = static_cast<std::uint16_t>(data_[offset_])
            | (static_cast<std::uint16_t>(data_[offset_ + 1]) << 8);
        offset_ += 2;
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
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file)
    {
        throw std::runtime_error("Failed to open the .mmdl file.");
    }
    const std::streamsize fileSize = file.tellg();
    if (fileSize < 0)
    {
        throw std::runtime_error("Failed to size the .mmdl file.");
    }
    file.seekg(0, std::ios::beg);
    std::vector<std::uint8_t> data(static_cast<std::size_t>(fileSize));
    if (fileSize > 0)
    {
        file.read(reinterpret_cast<char*>(data.data()), fileSize);
    }

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
    const MmdlChunkDescriptor* skeletonChunk = nullptr;
    const MmdlChunkDescriptor* skinningChunk = nullptr;
    const MmdlChunkDescriptor* refBoneChunk = nullptr;

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
        case MmdlChunkType::Skeleton:
            skeletonChunk = &descriptor;
            break;
        case MmdlChunkType::SkinningVertexBuffer:
            skinningChunk = &descriptor;
            break;
        case MmdlChunkType::SubMeshBoneTable:
            refBoneChunk = &descriptor;
            break;
        case MmdlChunkType::MeshMetadata:
        {
            // The buffer chunks carry the counts; the metadata carries the cooked bounds, the
            // one per-mesh value not derivable from the buffers.
            if (descriptor.size >= sizeof(MmdlMeshMetadata))
            {
                MmdlMeshMetadata metadata{};
                std::memcpy(&metadata, chunk, sizeof(metadata));
                for (int axis = 0; axis < 3; ++axis)
                {
                    mesh.boundsMin[axis] = metadata.boundsMin[axis];
                    mesh.boundsMax[axis] = metadata.boundsMax[axis];
                }
            }
            break;
        }
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
    const std::size_t materialStride = sizeof(MMDToonMaterial);
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

    // Skeleton (each bone carries its name inline).
    if (skeletonChunk != nullptr)
    {
        Reader reader(data.data() + skeletonChunk->offset, static_cast<std::size_t>(skeletonChunk->size));
        const std::uint32_t boneCount = reader.ReadU32();
        mesh.bones.reserve(boneCount);
        for (std::uint32_t i = 0; i < boneCount; ++i)
        {
            MmdlBone bone{};
            const std::uint32_t nameLength = reader.ReadU32();
            std::string name(nameLength, '\0');
            reader.ReadBytes(reinterpret_cast<std::uint8_t*>(name.data()), nameLength);
            bone.name = std::move(name);
            for (float& value : bone.position) { value = reader.ReadF32(); }
            for (float& value : bone.tail) { value = reader.ReadF32(); }
            bone.parentIndex = reader.ReadU16();
            bone.hasLocalAxes = reader.ReadU32();
            for (float& value : bone.localX) { value = reader.ReadF32(); }
            for (float& value : bone.localZ) { value = reader.ReadF32(); }
            bone.hasInheritRotation = reader.ReadU32();
            bone.hasInheritTranslation = reader.ReadU32();
            bone.inheritParentIndex = reader.ReadU16();
            bone.inheritInfluence = reader.ReadF32();
            bone.hasFixedAxis = reader.ReadU32();
            for (float& value : bone.fixedAxis) { value = reader.ReadF32(); }
            bone.ikTargetIndex = reader.ReadU16();
            bone.ikLoopCount = reader.ReadI32();
            bone.ikLimitAngle = reader.ReadF32();
            const std::uint32_t linkCount = reader.ReadU32();
            bone.ikLinks.resize(linkCount);
            for (std::uint16_t& link : bone.ikLinks) { link = reader.ReadU16(); }
            mesh.bones.push_back(bone);
        }
    }

    // Per-vertex skinning, parallel to the vertex buffer.
    if (skinningChunk != nullptr)
    {
        const std::size_t stride = sizeof(MmdlSkinningVertex);
        if (skinningChunk->size % stride != 0)
        {
            throw std::runtime_error(".mmdl skinning buffer has a partial vertex.");
        }
        const std::size_t count = static_cast<std::size_t>(skinningChunk->size) / stride;
        if (count != vertexCount)
        {
            throw std::runtime_error(".mmdl skinning count does not match the vertex count.");
        }
        mesh.skinning.resize(count);
        std::memcpy(mesh.skinning.data(), data.data() + skinningChunk->offset, skinningChunk->size);
    }

    // Concatenated per-submesh skin-reference-bone lists (u16 global bone indices).
    if (refBoneChunk != nullptr)
    {
        if (refBoneChunk->size % sizeof(std::uint16_t) != 0)
        {
            throw std::runtime_error(".mmdl sub-mesh bone table has a partial entry.");
        }
        const std::size_t count = static_cast<std::size_t>(refBoneChunk->size) / sizeof(std::uint16_t);
        mesh.refBones.resize(count);
        std::memcpy(mesh.refBones.data(), data.data() + refBoneChunk->offset, refBoneChunk->size);
    }

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
