#pragma once

#include "Runtime/Asset/ImageLoader.h"
#include "Runtime/Asset/MeshAsset.h"

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace MmdLab
{
// One loaded model: its CPU mesh and decoded textures. The GPU resources are built on demand
// by the RhiThread from this data when the model is selected.
struct SceneModel
{
    MeshAsset mesh;
    std::vector<Image> textures;
};

// The GameThread-owned authoritative model set: every discovered model plus the current
// selection. Selection changes bump the generation, which the render pipeline compares to
// decide when the RhiThread must rebuild GPU resources. Models are loaded once at startup and
// never mutated afterward, so per-frame pointers/spans into them stay valid for the session.
class Scene final
{
public:
    // Loads every *.pmx file in the directory, sorted by filename. Missing textures are logged
    // and replaced with a 1x1 white image so material texture indices stay aligned.
    void LoadFromDirectory(const std::filesystem::path& directory);

    [[nodiscard]] std::size_t ModelCount() const { return models_.size(); }
    [[nodiscard]] std::size_t SelectedModel() const { return selectedModel_; }
    [[nodiscard]] std::uint32_t Generation() const { return modelGeneration_; }
    [[nodiscard]] const std::vector<std::string>& DisplayNames() const { return displayNames_; }

    // Selects a model and bumps the generation. Ignored when the index is out of range or
    // already selected.
    void Select(std::size_t index);

    // The selected model's CPU mesh and textures, projected into each frame. Returns nullptr /
    // an empty span when no model is loaded.
    [[nodiscard]] const MeshAsset* SelectedMesh() const;
    [[nodiscard]] std::span<const Image> SelectedTextures() const;

private:
    std::vector<SceneModel> models_;
    std::vector<std::string> displayNames_; // Parallel to models_, contiguous for the UI span.
    std::size_t selectedModel_ = 0;
    std::uint32_t modelGeneration_ = 0;
};
} // namespace MmdLab
