#pragma once

#include <cstddef>
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
} // namespace MmdLab
