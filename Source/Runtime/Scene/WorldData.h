#pragma once

#include <DirectXMath.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace MmdLab
{
// One model placed within a level: a reference into the model registry plus a world transform
// and a visibility flag. The transform is identity until a manifest or editor drives
// per-instance placement.
struct ModelInstance
{
    std::size_t modelIndex = 0;
    float translation[3] = { 0.0f, 0.0f, 0.0f };
    float rotation[3] = { 0.0f, 0.0f, 0.0f }; // Euler degrees; YXZ order (MMD convention).
    bool visible = true;
};

// An instance's world matrix: YXZ Euler rotation then translation, in DirectXMath row-vector
// form. Identity for the default zero transform.
[[nodiscard]] inline DirectX::XMMATRIX InstanceWorldMatrix(const ModelInstance& instance)
{
    using namespace DirectX;
    const XMMATRIX rotation = XMMatrixRotationRollPitchYaw(
        XMConvertToRadians(instance.rotation[0]), // pitch (X).
        XMConvertToRadians(instance.rotation[1]), // yaw (Y).
        XMConvertToRadians(instance.rotation[2])); // roll (Z).
    const XMMATRIX translation = XMMatrixTranslation(
        instance.translation[0], instance.translation[1], instance.translation[2]);
    return XMMatrixMultiply(translation, rotation);
}

// One level, e.g. a folder under the models directory: an ordered group of model instances
// (the characters and objects that compose it). Each instance references a model in the
// registry, so a model may be shared across levels once a manifest provides placements.
struct Level
{
    std::string name;
    std::vector<ModelInstance> instances;
    float boundsMin[3] = { 0.0f, 0.0f, 0.0f }; // Union bounds of the level's models, for camera framing.
    float boundsMax[3] = { 0.0f, 0.0f, 0.0f };
};

// Invalid motion index: "no motion scanned or selected yet", mirroring kInvalidBoneIndex's role
// for "no bone". The Motion combo and frame projection use it before the first selection.
inline constexpr std::uint32_t kInvalidMotionIndex = 0xFFFFFFFFu;

// One VMD motion file discovered under the Motions directory and selectable for playback. The
// GameThread uses the path to parse the motion on selection; only the name is projected into the
// frame so the UI can list motions. VMD is a MikuMikuDance file-format identifier.
struct MotionEntry
{
    std::string name;            // UTF-8 display name (the file stem).
    std::filesystem::path path;  // Absolute path used to parse the motion on selection.
};
} // namespace MmdLab
