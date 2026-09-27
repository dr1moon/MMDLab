#pragma once

#include "Runtime/Asset/AssetIo.h"
#include "Runtime/Scene/Camera.h"
#include "Runtime/Scene/WorldData.h"

#include <cstdint>
#include <filesystem>
#include <span>
#include <vector>

namespace MmdLab
{
class ModelRegistry;

// The GameThread-owned authoritative world: the composed levels, the orbit camera, and the
// current selection. The model pool lives in a separate ModelRegistry (the asset side); the
// world references models by index through each level's instances. Every model loads in two
// stages on the I/O threads -- parse its PMX, then decode each texture as a separate job -- and
// the GameThread assembles the pieces here before publishing a level to the renderer. Selection
// changes bump the generation, which the render pipeline compares to decide when the RhiThread
// must build GPU resources for the newly selected level.
class World final
{
public:
    // Scans every *.pmx file under the directory and groups them by their immediate parent
    // directory: each directory becomes one level and each .pmx becomes one model instance
    // (default identity transform) within that level. Reserves the registry to the total model
    // count, then enqueues every model's parse on `loadRequests`, initially-selected level first,
    // so all levels preload in the background after startup.
    void LoadFromDirectory(
        const std::filesystem::path& directory,
        ModelRegistry& registry,
        Channel<LoadRequest, kLoadRequestCapacity>& loadRequests);

    [[nodiscard]] std::size_t LevelCount() const { return levels_.size(); }
    [[nodiscard]] std::size_t SelectedLevel() const { return selectedLevel_; }
    [[nodiscard]] std::uint32_t LevelGeneration() const { return levelGeneration_; }
    [[nodiscard]] const std::vector<Level>& Levels() const { return levels_; }
    [[nodiscard]] Camera& GetCamera() { return camera_; }

    // Selects a level. A loaded level is published immediately (generation bump + camera frame);
    // an unloaded one is still loading in the background, and its completion publishes it.
    // Ignored when the index is out of range or already selected.
    void SelectLevel(std::size_t index);

    // Applies one load result from the I/O threads: a parse fans out texture-decode jobs, and a
    // texture decode installs one image. Finishes and publishes a model and its level as their
    // pieces complete. GameThread-only.
    void OnLoadResult(LoadResult&& result);

    // Toggles one model of the selected level's visibility. Does not bump the generation: the
    // model's GPU resources stay resident, and the renderer skips hidden instances per frame.
    void SetInstanceVisible(std::size_t index, bool visible);

    // The selected level's instances, projected into each frame. Returns an empty span when
    // no level is loaded or the selected level has not finished loading.
    [[nodiscard]] std::span<const ModelInstance> SelectedInstances() const;

private:
    // One model mid-assembly: parsed mesh plus textures being filled by decode results.
    struct PendingModel
    {
        bool parsed = false;
        bool ok = false;
        Model model;
        std::size_t remainingTextures = 0;
        std::size_t missingTextures = 0;
    };

    // Assembly bookkeeping for one level, parallel to `levels_`.
    struct PendingLevel
    {
        std::vector<PendingModel> models; // Parallel to the level's source .pmx files.
        std::size_t completed = 0;
        bool loaded = false;
    };

    void OnModelParsed(ModelParseResult& result);
    void OnTextureDecoded(TextureDecodeResult& result);
    void FinishModel(std::size_t levelIndex, std::size_t modelSlot);

    std::vector<Level> levels_;
    std::vector<PendingLevel> pending_;
    Camera camera_;
    ModelRegistry* registry_ = nullptr;
    Channel<LoadRequest, kLoadRequestCapacity>* loadRequests_ = nullptr;
    std::size_t selectedLevel_ = 0;
    std::uint32_t levelGeneration_ = 0;
};
} // namespace MmdLab
