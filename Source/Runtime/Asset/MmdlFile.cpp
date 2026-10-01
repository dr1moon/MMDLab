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

std::vector<std::uint8_t> ReadMmdlBytes(const std::filesystem::path& path)
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
    return data;
}

MmdlMeshData ParseMmdl(const std::span<const std::uint8_t> data)
{
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
    const MmdlChunkDescriptor* morphChunk = nullptr;
    const MmdlChunkDescriptor* physicsChunk = nullptr;

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
        case MmdlChunkType::Morph:
            morphChunk = &descriptor;
            break;
        case MmdlChunkType::Physics:
            physicsChunk = &descriptor;
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
            bone.deformLayer = reader.ReadI32();
            bone.afterPhysics = reader.ReadU32();
            bone.ikTargetIndex = reader.ReadU16();
            bone.ikLoopCount = reader.ReadI32();
            bone.ikLimitAngle = reader.ReadF32();
            const std::uint32_t linkCount = reader.ReadU32();
            bone.ikLinks.resize(linkCount);
            for (MmdlIkLink& link : bone.ikLinks)
            {
                link.boneIndex = reader.ReadU16();
                link.hasLimit = reader.ReadU32();
                for (float& value : link.limitMin) { value = reader.ReadF32(); }
                for (float& value : link.limitMax) { value = reader.ReadF32(); }
            }
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

    // Morphs (each carries its names inline, like the skeleton).
    if (morphChunk != nullptr)
    {
        Reader reader(data.data() + morphChunk->offset, static_cast<std::size_t>(morphChunk->size));
        const std::uint32_t morphCount = reader.ReadU32();
        mesh.morphs.reserve(morphCount);
        for (std::uint32_t m = 0; m < morphCount; ++m)
        {
            Morph morph;
            const std::uint32_t nameLength = reader.ReadU32();
            std::string name(nameLength, '\0');
            reader.ReadBytes(reinterpret_cast<std::uint8_t*>(name.data()), nameLength);
            morph.name = std::move(name);
            const std::uint32_t nameEnLength = reader.ReadU32();
            std::string nameEn(nameEnLength, '\0');
            reader.ReadBytes(reinterpret_cast<std::uint8_t*>(nameEn.data()), nameEnLength);
            morph.nameEn = std::move(nameEn);
            morph.panel = reader.ReadU8();
            morph.kind = static_cast<MorphKind>(reader.ReadU8());

            const std::uint32_t vertexDeltaCount = reader.ReadU32();
            morph.vertexDeltas.reserve(vertexDeltaCount);
            for (std::uint32_t i = 0; i < vertexDeltaCount; ++i)
            {
                VertexMorphDelta delta;
                delta.vertexIndex = reader.ReadU32();
                for (float& value : delta.positionDelta) { value = reader.ReadF32(); }
                morph.vertexDeltas.push_back(delta);
            }

            const std::uint32_t boneDeltaCount = reader.ReadU32();
            morph.boneDeltas.reserve(boneDeltaCount);
            for (std::uint32_t i = 0; i < boneDeltaCount; ++i)
            {
                BoneMorphDelta delta;
                delta.boneIndex = reader.ReadU16();
                for (float& value : delta.positionDelta) { value = reader.ReadF32(); }
                for (float& value : delta.rotationDelta) { value = reader.ReadF32(); }
                morph.boneDeltas.push_back(delta);
            }

            const std::uint32_t groupItemCount = reader.ReadU32();
            morph.groupItems.reserve(groupItemCount);
            for (std::uint32_t i = 0; i < groupItemCount; ++i)
            {
                GroupMorphItem item;
                item.morphIndex = reader.ReadU32();
                item.ratio = reader.ReadF32();
                morph.groupItems.push_back(item);
            }

            mesh.morphs.push_back(std::move(morph));
        }
    }

    // Rigid bodies and joints (names inline, like the skeleton).
    if (physicsChunk != nullptr)
    {
        Reader reader(data.data() + physicsChunk->offset, static_cast<std::size_t>(physicsChunk->size));
        const auto readString = [&reader]()
        {
            const std::uint32_t length = reader.ReadU32();
            std::string text(length, '\0');
            reader.ReadBytes(reinterpret_cast<std::uint8_t*>(text.data()), length);
            return text;
        };
        const auto readVector3 = [&reader](float (&out)[3])
        {
            for (float& value : out) { value = reader.ReadF32(); }
        };

        const std::uint32_t bodyCount = reader.ReadU32();
        mesh.physics.bodies.reserve(bodyCount);
        for (std::uint32_t i = 0; i < bodyCount; ++i)
        {
            BodySetup body;
            body.name = readString();
            body.boneIndex = reader.ReadU16();
            body.group = reader.ReadU8();
            body.collisionMask = reader.ReadU16();
            const std::uint8_t shape = reader.ReadU8();
            const std::uint8_t mode = reader.ReadU8();
            if (shape > 2 || mode > 2 || body.group > 15)
            {
                throw std::runtime_error(".mmdl rigid body has an invalid shape, mode, or group.");
            }
            body.shape = static_cast<BodyShape>(shape);
            body.mode = static_cast<BodyMode>(mode);
            readVector3(body.size);
            readVector3(body.position);
            readVector3(body.rotation);
            body.mass = reader.ReadF32();
            body.linearDamping = reader.ReadF32();
            body.angularDamping = reader.ReadF32();
            body.restitution = reader.ReadF32();
            body.friction = reader.ReadF32();
            if (body.boneIndex != kInvalidBoneIndex && body.boneIndex >= mesh.bones.size())
            {
                throw std::runtime_error(".mmdl rigid body references an out-of-range bone.");
            }
            mesh.physics.bodies.push_back(std::move(body));
        }

        const std::uint32_t constraintCount = reader.ReadU32();
        mesh.physics.constraints.reserve(constraintCount);
        for (std::uint32_t i = 0; i < constraintCount; ++i)
        {
            ConstraintSetup constraint;
            constraint.name = readString();
            constraint.bodyA = reader.ReadU32();
            constraint.bodyB = reader.ReadU32();
            readVector3(constraint.position);
            readVector3(constraint.rotation);
            readVector3(constraint.linearLowerLimit);
            readVector3(constraint.linearUpperLimit);
            readVector3(constraint.angularLowerLimit);
            readVector3(constraint.angularUpperLimit);
            readVector3(constraint.linearStiffness);
            readVector3(constraint.angularStiffness);
            if (constraint.bodyA >= bodyCount || constraint.bodyB >= bodyCount)
            {
                throw std::runtime_error(".mmdl joint references an out-of-range rigid body.");
            }
            mesh.physics.constraints.push_back(std::move(constraint));
        }
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

    // Validate morph references.
    for (const Morph& morph : mesh.morphs)
    {
        for (const VertexMorphDelta& delta : morph.vertexDeltas)
        {
            if (delta.vertexIndex >= vertexCount)
            {
                throw std::runtime_error(".mmdl vertex morph references an out-of-range vertex.");
            }
        }
        for (const BoneMorphDelta& delta : morph.boneDeltas)
        {
            if (delta.boneIndex >= mesh.bones.size())
            {
                throw std::runtime_error(".mmdl bone morph references an out-of-range bone.");
            }
        }
        for (const GroupMorphItem& item : morph.groupItems)
        {
            if (item.morphIndex >= mesh.morphs.size())
            {
                throw std::runtime_error(".mmdl group morph references an out-of-range morph.");
            }
        }
    }

    return mesh;
}

MmdlMeshData ReadMmdl(const std::filesystem::path& path)
{
    return ParseMmdl(ReadMmdlBytes(path));
}
} // namespace MmdLab
