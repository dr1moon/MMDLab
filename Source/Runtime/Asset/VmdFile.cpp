#include "Runtime/Asset/VmdFile.h"

#include "Runtime/Core/Utf8.h"

#include <windows.h>

#include <algorithm>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

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
            throw std::runtime_error(
                "VMD parse: unexpected end of file (offset " + std::to_string(offset_)
                + " of " + std::to_string(size_) + ").");
        }
    }

    const std::uint8_t* data_;
    std::size_t size_;
    std::size_t offset_;
};

// Decodes a fixed-length Shift-JIS field, truncating at the first null byte. VMD names are
// Shift-JIS encoded (unlike PMX, which stores UTF-16).
std::string ShiftJisToUtf8(const std::uint8_t* data, const std::size_t length)
{
    std::size_t terminator = 0;
    while (terminator < length && data[terminator] != 0)
    {
        ++terminator;
    }
    if (terminator == 0)
    {
        return {};
    }

    const int wideLength = MultiByteToWideChar(
        932, 0, reinterpret_cast<const char*>(data), static_cast<int>(terminator), nullptr, 0); // 932 = Shift-JIS.
    if (wideLength <= 0)
    {
        return {};
    }
    std::wstring wide(static_cast<std::size_t>(wideLength), L'\0');
    MultiByteToWideChar(
        932, 0, reinterpret_cast<const char*>(data), static_cast<int>(terminator), wide.data(), wideLength);
    return WideToUtf8(wide);
}
} // namespace

VmdMotion ParseVmdFile(const std::filesystem::path& path)
{
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file)
    {
        throw std::runtime_error("Failed to open the VMD file.");
    }
    const std::streamsize fileSize = file.tellg();
    if (fileSize < 0)
    {
        throw std::runtime_error("Failed to size the VMD file.");
    }
    file.seekg(0, std::ios::beg);
    std::vector<std::uint8_t> data(static_cast<std::size_t>(fileSize));
    if (fileSize > 0)
    {
        file.read(reinterpret_cast<char*>(data.data()), fileSize);
    }

    Reader reader(data.data(), data.size());

    // Header: a fixed 30-byte signature, null-padded.
    std::uint8_t signature[30];
    reader.ReadBytes(signature, sizeof(signature));
    constexpr char kVmdSignature[] = "Vocaloid Motion Data 0002";
    if (std::memcmp(signature, kVmdSignature, sizeof(kVmdSignature) - 1) != 0)
    {
        throw std::runtime_error("Not a VMD file (bad signature).");
    }

    VmdMotion motion;

    std::uint8_t modelName[20];
    reader.ReadBytes(modelName, sizeof(modelName));
    motion.modelName = ShiftJisToUtf8(modelName, sizeof(modelName));

    const std::uint32_t boneCount = reader.ReadU32();
    motion.boneTracks.reserve(boneCount);
    for (std::uint32_t i = 0; i < boneCount; ++i)
    {
        std::uint8_t name[15];
        reader.ReadBytes(name, sizeof(name));
        const std::string boneName = ShiftJisToUtf8(name, sizeof(name));

        VmdBoneKey key;
        key.frame = reader.ReadU32();
        key.position[0] = reader.ReadF32();
        key.position[1] = reader.ReadF32();
        key.position[2] = reader.ReadF32();
        key.rotation[0] = reader.ReadF32();
        key.rotation[1] = reader.ReadF32();
        key.rotation[2] = reader.ReadF32();
        key.rotation[3] = reader.ReadF32();
        reader.ReadBytes(key.interpolation, sizeof(key.interpolation));

        // MMD's encoder writes an all-zero quaternion for the identity rotation.
        if (key.rotation[0] == 0.0f && key.rotation[1] == 0.0f
            && key.rotation[2] == 0.0f && key.rotation[3] == 0.0f)
        {
            key.rotation[3] = 1.0f;
        }

        // Append to the matching track, or start one. A linear scan keeps the parser simple;
        // VMD files emit a bone's keyframes contiguously, so this stays near O(n).
        auto track = std::find_if(motion.boneTracks.begin(), motion.boneTracks.end(),
            [&](const VmdBoneTrack& candidate) { return candidate.boneName == boneName; });
        if (track == motion.boneTracks.end())
        {
            VmdBoneTrack newTrack;
            newTrack.boneName = boneName;
            newTrack.keys.push_back(key);
            motion.boneTracks.push_back(std::move(newTrack));
        }
        else
        {
            track->keys.push_back(key);
        }
    }

    // Sort each track by frame so sampling can binary-search or walk surrounding keyframes.
    for (VmdBoneTrack& track : motion.boneTracks)
    {
        std::sort(track.keys.begin(), track.keys.end(),
            [](const VmdBoneKey& a, const VmdBoneKey& b) { return a.frame < b.frame; });
    }

    // Morph, camera, light, self-shadow, and IK sections follow in the file but are skipped.
    return motion;
}
} // namespace MmdLab
