#include "Runtime/Asset/PmxFile.h"

#include <windows.h>

#include <algorithm>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <string>
#include <utility>

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

// Reads a bone index and narrows it to the runtime's two-byte representation. PMX's -1 "no bone"
// sentinel becomes kInvalidBoneIndex (0xFFFF); a valid 0..N-1 index passes through unchanged.
std::uint16_t ReadBoneIndex(Reader& reader, const std::uint8_t boneIndexSize)
{
    return static_cast<std::uint16_t>(ReadIndex(reader, boneIndexSize));
}

// Reads an unsigned index of the given size (1, 2, or 4 bytes), zero-extending to uint32. PMX
// vertex indices are unsigned (a morph offset never references "no vertex").
std::uint32_t ReadUIndex(Reader& reader, const std::uint8_t size)
{
    switch (size)
    {
    case 1: return reader.ReadU8();
    case 2: return reader.ReadU16();
    case 4: return reader.ReadU32();
    default: throw std::runtime_error("Unsupported index size.");
    }
}
} // namespace

PmxStaticMesh ParsePmxStaticMesh(const std::filesystem::path& path)
{
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file)
    {
        throw std::runtime_error("Failed to open the PMX file.");
    }
    const std::streamsize fileSize = file.tellg();
    if (fileSize < 0)
    {
        throw std::runtime_error("Failed to size the PMX file.");
    }
    file.seekg(0, std::ios::beg);
    std::vector<std::uint8_t> data(static_cast<std::size_t>(fileSize));
    if (fileSize > 0)
    {
        file.read(reinterpret_cast<char*>(data.data()), fileSize);
    }

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
    const std::uint8_t materialIndexSize = reader.ReadU8();
    const std::uint8_t boneIndexSize = reader.ReadU8();
    const std::uint8_t morphIndexSize = reader.ReadU8();
    const std::uint8_t rigidBodyIndexSize = reader.ReadU8();

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

        // Additional UVs (a vec4 each). Keep the first one (UV1) for the sphere subtexture.
        if (additionalUvCount > 0)
        {
            vertex.uv1[0] = reader.ReadF32();
            vertex.uv1[1] = reader.ReadF32();
            reader.Skip(2 * 4); // The remaining two floats of the first additional UV.
            reader.Skip(static_cast<std::size_t>(additionalUvCount - 1) * 4 * 4);
        }
        else
        {
            vertex.uv1[0] = vertex.uv[0];
            vertex.uv1[1] = vertex.uv[1];
        }

        // Skinning: read up to four bone indices and their weights. SDEF is read as BDEF2 with
        // its spherical C/R0/R1 dropped, the standard linear-blend-skinning approximation.
        const std::uint8_t skinningType = reader.ReadU8();
        switch (skinningType)
        {
        case 0: // BDEF1: one bone, implicit weight 1.
            vertex.boneIndices[0] = ReadIndex(reader, boneIndexSize);
            vertex.boneWeights[0] = 1.0f;
            break;
        case 1: // BDEF2: two bones, one weight (the second is 1 - first).
            vertex.boneIndices[0] = ReadIndex(reader, boneIndexSize);
            vertex.boneIndices[1] = ReadIndex(reader, boneIndexSize);
            vertex.boneWeights[0] = reader.ReadF32();
            vertex.boneWeights[1] = 1.0f - vertex.boneWeights[0];
            break;
        case 2: // BDEF4: four bones and weights.
            for (int bone = 0; bone < 4; ++bone)
            {
                vertex.boneIndices[bone] = ReadIndex(reader, boneIndexSize);
            }
            for (int bone = 0; bone < 4; ++bone)
            {
                vertex.boneWeights[bone] = reader.ReadF32();
            }
            break;
        case 3: // SDEF: read as BDEF2, dropping the spherical C/R0/R1.
            vertex.boneIndices[0] = ReadIndex(reader, boneIndexSize);
            vertex.boneIndices[1] = ReadIndex(reader, boneIndexSize);
            vertex.boneWeights[0] = reader.ReadF32();
            vertex.boneWeights[1] = 1.0f - vertex.boneWeights[0];
            reader.Skip(36); // SDEF-C, SDEF-R0, SDEF-R1 (9 floats).
            break;
        case 4: // QDEF (PMX 2.1): four bones and weights, same layout as BDEF4.
            for (int bone = 0; bone < 4; ++bone)
            {
                vertex.boneIndices[bone] = ReadIndex(reader, boneIndexSize);
            }
            for (int bone = 0; bone < 4; ++bone)
            {
                vertex.boneWeights[bone] = reader.ReadF32();
            }
            break;
        default:
            throw std::runtime_error("Unknown skinning type.");
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
            // A custom toon texture, referenced by a texture index.
            material.toonTextureIndex = ReadIndex(reader, textureIndexSize);
        }
        else
        {
            // A shared system toon (index 0-9 into toon01.bmp..toon10.bmp); unused for now.
            reader.Skip(1);
        }

        (void)ReadString(reader, utf8); // memo.
        material.indexCount = reader.ReadI32();
        mesh.materials.push_back(material);
    }

    // Bones.
    const std::uint32_t boneCount = reader.ReadU32();
    mesh.bones.reserve(boneCount);
    for (std::uint32_t i = 0; i < boneCount; ++i)
    {
        PmxBone bone;
        bone.name = ReadString(reader, utf8);
        bone.nameEn = ReadString(reader, utf8);
        bone.position[0] = reader.ReadF32();
        bone.position[1] = reader.ReadF32();
        bone.position[2] = reader.ReadF32();
        bone.parentIndex = ReadBoneIndex(reader, boneIndexSize);
        bone.deformLayer = reader.ReadI32();
        bone.flags = reader.ReadU16();

        if ((bone.flags & PmxBoneFlags::TailIndex) != 0)
        {
            bone.tailIndex = ReadBoneIndex(reader, boneIndexSize);
        }
        else
        {
            bone.tailOffset[0] = reader.ReadF32();
            bone.tailOffset[1] = reader.ReadF32();
            bone.tailOffset[2] = reader.ReadF32();
        }

        if ((bone.flags & (PmxBoneFlags::InheritRotation | PmxBoneFlags::InheritTranslation)) != 0)
        {
            bone.inheritParentIndex = ReadBoneIndex(reader, boneIndexSize);
            bone.inheritInfluence = reader.ReadF32();
        }

        if ((bone.flags & PmxBoneFlags::FixedAxis) != 0)
        {
            bone.fixedAxis[0] = reader.ReadF32();
            bone.fixedAxis[1] = reader.ReadF32();
            bone.fixedAxis[2] = reader.ReadF32();
        }

        if ((bone.flags & PmxBoneFlags::LocalCoordinate) != 0)
        {
            bone.localX[0] = reader.ReadF32();
            bone.localX[1] = reader.ReadF32();
            bone.localX[2] = reader.ReadF32();
            bone.localZ[0] = reader.ReadF32();
            bone.localZ[1] = reader.ReadF32();
            bone.localZ[2] = reader.ReadF32();
        }

        if ((bone.flags & PmxBoneFlags::ExternalParentDeform) != 0)
        {
            bone.externalParentKey = reader.ReadI32();
        }

        if ((bone.flags & PmxBoneFlags::Ik) != 0)
        {
            bone.ikTargetIndex = ReadBoneIndex(reader, boneIndexSize);
            bone.ikLoopCount = reader.ReadI32();
            bone.ikLimitAngle = reader.ReadF32();
            const std::int32_t linkCount = reader.ReadI32();
            bone.ikLinks.reserve(static_cast<std::size_t>(linkCount));
            for (std::int32_t link = 0; link < linkCount; ++link)
            {
                PmxIkLink ikLink;
                ikLink.boneIndex = ReadBoneIndex(reader, boneIndexSize);
                ikLink.hasLimit = reader.ReadU8() != 0;
                if (ikLink.hasLimit)
                {
                    ikLink.limitMin[0] = reader.ReadF32();
                    ikLink.limitMin[1] = reader.ReadF32();
                    ikLink.limitMin[2] = reader.ReadF32();
                    ikLink.limitMax[0] = reader.ReadF32();
                    ikLink.limitMax[1] = reader.ReadF32();
                    ikLink.limitMax[2] = reader.ReadF32();
                }
                bone.ikLinks.push_back(std::move(ikLink));
            }
        }

        mesh.bones.push_back(std::move(bone));
    }

    // Morphs: parse and store vertex, bone, and group morphs (the expression-critical kinds);
    // UV, additional-UV, material, flip, and impulse morphs are read and discarded until a
    // runtime consumer needs them.
    const std::uint32_t morphCount = reader.ReadU32();
    mesh.morphs.reserve(morphCount);
    for (std::uint32_t m = 0; m < morphCount; ++m)
    {
        Morph morph;
        morph.name = ReadString(reader, utf8);
        morph.nameEn = ReadString(reader, utf8);
        morph.panel = reader.ReadU8();
        const std::uint8_t kindByte = reader.ReadU8();
        morph.kind = static_cast<MorphKind>(kindByte);
        const std::uint32_t offsetCount = reader.ReadU32();
        for (std::uint32_t o = 0; o < offsetCount; ++o)
        {
            switch (kindByte)
            {
            case 0: // Group.
            {
                GroupMorphItem item;
                item.morphIndex = static_cast<std::uint32_t>(ReadIndex(reader, morphIndexSize));
                item.ratio = reader.ReadF32();
                morph.groupItems.push_back(item);
                break;
            }
            case 1: // Vertex.
            {
                VertexMorphDelta delta;
                delta.vertexIndex = ReadUIndex(reader, vertexIndexSize);
                for (float& value : delta.positionDelta) { value = reader.ReadF32(); }
                morph.vertexDeltas.push_back(delta);
                break;
            }
            case 2: // Bone.
            {
                BoneMorphDelta delta;
                delta.boneIndex = static_cast<std::uint16_t>(ReadIndex(reader, boneIndexSize));
                for (float& value : delta.positionDelta) { value = reader.ReadF32(); }
                for (float& value : delta.rotationDelta) { value = reader.ReadF32(); }
                morph.boneDeltas.push_back(delta);
                break;
            }
            case 3: // UV.
                ReadUIndex(reader, vertexIndexSize);
                reader.Skip(4 * 4);
                break;
            case 4: // Additional UV slots (one per slot).
            case 5:
            case 6:
            case 7:
                ReadUIndex(reader, vertexIndexSize);
                reader.Skip(4 * 4);
                break;
            case 8: // Material.
                ReadIndex(reader, materialIndexSize);
                reader.Skip(1);      // Offset operator (multiply/add).
                reader.Skip(28 * 4); // Diffuse, specular, ambient, edge, texture tints (28 floats).
                break;
            case 9: // Flip: like a group morph referencing another morph.
                ReadIndex(reader, morphIndexSize);
                reader.Skip(4); // Ratio.
                break;
            case 10: // Impulse: rigid-body reference plus velocities.
                ReadIndex(reader, rigidBodyIndexSize);
                reader.Skip(1);     // Is-local flag.
                reader.Skip(6 * 4); // Movement velocity and rotation torque.
                break;
            default:
                throw std::runtime_error("Unknown PMX morph kind.");
            }
        }
        mesh.morphs.push_back(std::move(morph));
    }

    // Validate morph references (vertex, bone, and group morphs only).
    for (const Morph& morph : mesh.morphs)
    {
        for (const VertexMorphDelta& delta : morph.vertexDeltas)
        {
            if (delta.vertexIndex >= vertexCount)
            {
                throw std::runtime_error("PMX vertex morph references an out-of-range vertex.");
            }
        }
        for (const BoneMorphDelta& delta : morph.boneDeltas)
        {
            if (delta.boneIndex >= mesh.bones.size())
            {
                throw std::runtime_error("PMX bone morph references an out-of-range bone.");
            }
        }
        for (const GroupMorphItem& item : morph.groupItems)
        {
            if (item.morphIndex >= morphCount)
            {
                throw std::runtime_error("PMX group morph references an out-of-range morph.");
            }
        }
    }

    // Validate: each triangle index is in range, and the materials' index counts cover the
    // whole index buffer exactly.
    for (const std::uint32_t index : mesh.indices)
    {
        if (index >= vertexCount)
        {
            throw std::runtime_error("Vertex index out of range.");
        }
    }

    std::uint32_t totalIndexCount = 0;
    for (const PmxMaterial& material : mesh.materials)
    {
        totalIndexCount += static_cast<std::uint32_t>(material.indexCount);
    }
    if (totalIndexCount != indexCount)
    {
        throw std::runtime_error("Material index counts do not sum to the vertex index count.");
    }

    return mesh;
}

MmdlMeshData ConvertPmxToMmdl(const PmxStaticMesh& pmx)
{
    MmdlMeshData mesh;

    // Union the bounds over the source vertices (the per-submesh split below duplicates vertices
    // but not their positions).
    for (const PmxVertex& vertex : pmx.vertices)
    {
        for (int axis = 0; axis < 3; ++axis)
        {
            mesh.boundsMin[axis] = std::min(mesh.boundsMin[axis], vertex.position[axis]);
            mesh.boundsMax[axis] = std::max(mesh.boundsMax[axis], vertex.position[axis]);
        }
    }

    mesh.strings = pmx.textures;
    mesh.vertices.reserve(pmx.vertices.size());
    mesh.skinning.reserve(pmx.vertices.size());
    mesh.indices.reserve(pmx.indices.size());

    mesh.materials.reserve(pmx.materials.size());
    for (const PmxMaterial& material : pmx.materials)
    {
        MMDToonMaterial out{};
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

    // Cook the skeleton and per-vertex skinning so the .mmdl is a complete asset. The bone name
    // is stored inline (not in the string table, which stays texture paths only); the tail is
    // resolved here (a tail-index reference becomes the target bone's head).
    mesh.bones.reserve(pmx.bones.size());
    for (std::size_t i = 0; i < pmx.bones.size(); ++i)
    {
        const PmxBone& source = pmx.bones[i];
        MmdlBone bone{};
        bone.name = source.name;
        for (int axis = 0; axis < 3; ++axis)
        {
            bone.position[axis] = source.position[axis];
        }

        const bool hasTailBone = (source.flags & PmxBoneFlags::TailIndex) != 0
            && source.tailIndex != kInvalidBoneIndex
            && static_cast<std::size_t>(source.tailIndex) < pmx.bones.size();
        for (int axis = 0; axis < 3; ++axis)
        {
            bone.tail[axis] = hasTailBone
                ? pmx.bones[static_cast<std::size_t>(source.tailIndex)].position[axis]
                : source.position[axis] + source.tailOffset[axis];
        }

        bone.parentIndex = source.parentIndex;
        bone.hasLocalAxes = (source.flags & PmxBoneFlags::LocalCoordinate) != 0 ? 1u : 0u;
        for (int axis = 0; axis < 3; ++axis)
        {
            bone.localX[axis] = source.localX[axis];
            bone.localZ[axis] = source.localZ[axis];
        }

        bone.hasInheritRotation = (source.flags & PmxBoneFlags::InheritRotation) != 0 ? 1u : 0u;
        bone.hasInheritTranslation = (source.flags & PmxBoneFlags::InheritTranslation) != 0 ? 1u : 0u;
        bone.inheritParentIndex = source.inheritParentIndex;
        bone.inheritInfluence = source.inheritInfluence;

        bone.hasFixedAxis = (source.flags & PmxBoneFlags::FixedAxis) != 0 ? 1u : 0u;
        for (int axis = 0; axis < 3; ++axis)
        {
            bone.fixedAxis[axis] = source.fixedAxis[axis];
        }

        if ((source.flags & PmxBoneFlags::Ik) != 0)
        {
            bone.ikTargetIndex = source.ikTargetIndex;
            bone.ikLoopCount = source.ikLoopCount;
            bone.ikLimitAngle = source.ikLimitAngle;
            bone.ikLinks.reserve(source.ikLinks.size());
            for (const PmxIkLink& link : source.ikLinks)
            {
                bone.ikLinks.push_back(link.boneIndex);
            }
        }
        mesh.bones.push_back(bone);
    }

    // Per-submesh expansion: build disjoint vertex, skinning, and index buffers plus the
    // per-submesh skin-reference-bone table. PMX materials may share vertices, so each submesh
    // gets its own copies with u8 indices local to that submesh (a shared vertex cannot carry one
    // set of local indices that is valid for every submesh).
    mesh.drawPackets.reserve(pmx.materials.size());
    mesh.refBones.clear();
    // Global PMX vertex -> every local (.mmdl) vertex that duplicates it, accumulated across the
    // submesh loop below so vertex morphs can be fanned out afterward.
    std::vector<std::vector<std::uint32_t>> globalToLocal(pmx.vertices.size());
    std::uint32_t materialFirstIndex = 0;
    for (std::uint32_t m = 0; m < pmx.materials.size(); ++m)
    {
        const std::uint32_t indexCount = static_cast<std::uint32_t>(pmx.materials[m].indexCount);

        DrawPacket packet{};
        packet.firstIndex = static_cast<std::uint32_t>(mesh.indices.size());
        packet.indexCount = indexCount;
        packet.materialIndex = m;

        std::vector<std::int32_t> vertexRemap(pmx.vertices.size(), -1); // Global -> local vertex.
        std::vector<std::uint32_t> localToGlobal;                       // Local vertex -> global vertex.
        std::vector<std::uint32_t> localIndices;
        localIndices.reserve(indexCount);
        std::vector<char> used(pmx.bones.size(), 0);

        for (std::uint32_t k = 0; k < indexCount; ++k)
        {
            const std::uint32_t globalVertex = pmx.indices[materialFirstIndex + k];
            if (vertexRemap[globalVertex] < 0)
            {
                vertexRemap[globalVertex] = static_cast<std::int32_t>(localToGlobal.size());
                localToGlobal.push_back(globalVertex);
                globalToLocal[globalVertex].push_back(static_cast<std::uint32_t>(mesh.vertices.size()));

                const PmxVertex& source = pmx.vertices[globalVertex];
                MmdlVertex out{};
                out.position[0] = source.position[0];
                out.position[1] = source.position[1];
                out.position[2] = source.position[2];
                out.position[3] = 1.0f;
                out.normal[0] = source.normal[0];
                out.normal[1] = source.normal[1];
                out.normal[2] = source.normal[2];
                out.normal[3] = 0.0f;
                out.uv[0] = source.uv[0];
                out.uv[1] = source.uv[1];
                out.uv1[0] = source.uv1[0];
                out.uv1[1] = source.uv1[1];
                mesh.vertices.push_back(out);

                for (int slot = 0; slot < 4; ++slot)
                {
                    const std::int32_t bone = source.boneIndices[slot];
                    if (bone >= 0 && static_cast<std::size_t>(bone) < used.size() && source.boneWeights[slot] > 0.0f)
                    {
                        used[static_cast<std::size_t>(bone)] = 1;
                    }
                }
            }
            localIndices.push_back(static_cast<std::uint32_t>(vertexRemap[globalVertex]));
        }

        // Ascending global -> local bone map for this submesh.
        std::vector<std::uint8_t> boneLocal(pmx.bones.size(), 0);
        std::uint16_t refCount = 0;
        for (std::size_t b = 0; b < used.size(); ++b)
        {
            if (used[b])
            {
                boneLocal[b] = static_cast<std::uint8_t>(refCount);
                ++refCount;
            }
        }
        if (refCount > 256)
        {
            throw std::runtime_error("PMX submesh references more than 256 bones; cannot remap to u8 indices.");
        }

        packet.refBoneOffset = static_cast<std::uint32_t>(mesh.refBones.size());
        packet.refBoneCount = refCount;
        for (std::size_t b = 0; b < used.size(); ++b)
        {
            if (used[b])
            {
                mesh.refBones.push_back(static_cast<std::uint16_t>(b));
            }
        }

        // Append this submesh's skinning (u8 local indices), then its remapped indices.
        for (const std::uint32_t globalVertex : localToGlobal)
        {
            const PmxVertex& source = pmx.vertices[globalVertex];
            MmdlSkinningVertex skin{};
            for (int slot = 0; slot < 4; ++slot)
            {
                skin.boneWeights[slot] = source.boneWeights[slot];
                const std::int32_t bone = source.boneIndices[slot];
                skin.boneIndices[slot] = (bone >= 0 && static_cast<std::size_t>(bone) < used.size())
                    ? boneLocal[static_cast<std::size_t>(bone)]
                    : static_cast<std::uint8_t>(0);
            }
            mesh.skinning.push_back(skin);
        }

        const std::uint32_t vertexBase = static_cast<std::uint32_t>(mesh.vertices.size() - localToGlobal.size());
        for (const std::uint32_t localIndex : localIndices)
        {
            mesh.indices.push_back(vertexBase + localIndex);
        }

        mesh.drawPackets.push_back(packet);
        materialFirstIndex += indexCount;
    }

    // Cook morphs: fan each vertex morph's global vertex offset out to every per-submesh copy of
    // that vertex, so the runtime representation references mesh-local vertices directly. Bone and
    // group morphs reference global bone/morph indices and pass through unchanged.
    mesh.morphs.reserve(pmx.morphs.size());
    for (const Morph& source : pmx.morphs)
    {
        Morph morph = source; // Name, panel, kind, bone, and group offsets carry straight through.
        if (morph.kind == MorphKind::Vertex)
        {
            morph.vertexDeltas.clear();
            for (const VertexMorphDelta& delta : source.vertexDeltas)
            {
                for (const std::uint32_t localIndex : globalToLocal[delta.vertexIndex])
                {
                    VertexMorphDelta fan;
                    fan.vertexIndex = localIndex;
                    for (int axis = 0; axis < 3; ++axis)
                    {
                        fan.positionDelta[axis] = delta.positionDelta[axis];
                    }
                    morph.vertexDeltas.push_back(fan);
                }
            }
        }
        mesh.morphs.push_back(std::move(morph));
    }

    return mesh;
}
} // namespace MmdLab
