#include "App/MmdViewer/World.h"

#include "Runtime/Asset/ModelRegistry.h"
#include "Runtime/Asset/VmdFile.h"
#include "Runtime/Core/Log.h"
#include "Runtime/Core/Utf8.h"
#include "Runtime/Scene/StaticFloor.h"

#include <algorithm>
#include <exception>
#include <filesystem>
#include <format>
#include <map>
#include <string>

namespace MmdLab
{
void World::LoadFromDirectory(
    const std::filesystem::path& directory,
    ModelRegistry& registry,
    Channel<LoadRequest, kLoadRequestCapacity>& loadRequests)
{
    registry_ = &registry;
    loadRequests_ = &loadRequests;

    // Group .pmx files by their immediate parent directory so each directory becomes one
    // level (e.g. Models/<character>/{body.pmx, hat.pmx}). A .pmx nested directly in the scan
    // root groups under the root directory's name. Only paths are recorded here; the models
    // load asynchronously on the I/O threads.
    std::map<std::filesystem::path, std::vector<std::filesystem::path>> byDirectory;
    if (std::filesystem::is_directory(directory))
    {
        for (const auto& entry : std::filesystem::recursive_directory_iterator(directory))
        {
            if (entry.is_regular_file() && entry.path().extension() == L".pmx")
            {
                byDirectory[entry.path().parent_path()].push_back(entry.path());
            }
        }
    }

    std::vector<std::vector<std::filesystem::path>> levelFiles;
    std::size_t totalModels = 0;
    for (auto& [dir, files] : byDirectory)
    {
        std::sort(files.begin(), files.end());

        Level level;
        level.name = WideToUtf8(dir.filename().wstring());
        level.boundsMin[0] = level.boundsMin[1] = level.boundsMin[2] = 3.4e38f;
        level.boundsMax[0] = level.boundsMax[1] = level.boundsMax[2] = -3.4e38f;
        levels_.push_back(std::move(level));

        PendingLevel pending;
        pending.models.resize(files.size());
        pending_.push_back(std::move(pending));

        totalModels += files.size();
        levelFiles.push_back(std::move(files));
    }

    // Reserve the model pool so installs during the render loop never reallocate the vector the
    // renderer holds spans into across frames. The +1 is the procedural reflective floor, added
    // by InjectReflectiveFloor before the loop starts.
    registry_->Reserve(totalModels + 1);

    // Enqueue every model's parse, level by level so the initially-selected level's models get a
    // head start on the shared queue.
    for (std::size_t levelIndex = 0; levelIndex < levelFiles.size(); ++levelIndex)
    {
        for (std::size_t modelSlot = 0; modelSlot < levelFiles[levelIndex].size(); ++modelSlot)
        {
            ModelParseRequest request;
            request.levelIndex = levelIndex;
            request.modelSlot = modelSlot;
            request.path = levelFiles[levelIndex][modelSlot];
            loadRequests_->Push(std::move(request));
        }
    }
}

void World::OnLoadResult(LoadResult&& result)
{
    std::visit(Overloaded{
        [this](ModelParseResult& value) { OnModelParsed(value); },
        [this](TextureDecodeResult& value) { OnTextureDecoded(value); },
    }, result);
}

void World::OnModelParsed(ModelParseResult& result)
{
    PendingModel& pendingModel = pending_[result.levelIndex].models[result.modelSlot];
    pendingModel.parsed = true;

    if (!result.ok)
    {
        pendingModel.ok = false;
        FinishModel(result.levelIndex, result.modelSlot);
        return;
    }

    pendingModel.ok = true;
    pendingModel.model = std::move(result.model);
    pendingModel.model.textures.resize(result.texturePaths.size());
    pendingModel.remainingTextures = result.texturePaths.size();

    for (std::size_t i = 0; i < result.texturePaths.size(); ++i)
    {
        TextureDecodeRequest request;
        request.levelIndex = result.levelIndex;
        request.modelSlot = result.modelSlot;
        request.textureIndex = i;
        request.path = result.texturePaths[i];
        loadRequests_->Push(std::move(request));
    }

    if (pendingModel.remainingTextures == 0)
    {
        FinishModel(result.levelIndex, result.modelSlot);
    }
}

void World::OnTextureDecoded(TextureDecodeResult& result)
{
    PendingModel& pendingModel = pending_[result.levelIndex].models[result.modelSlot];
    if (result.ok)
    {
        pendingModel.model.textures[result.textureIndex] = std::move(result.image);
    }
    else
    {
        pendingModel.model.textures[result.textureIndex] = MissingTextureImage();
        ++pendingModel.missingTextures;
    }

    --pendingModel.remainingTextures;
    if (pendingModel.remainingTextures == 0)
    {
        FinishModel(result.levelIndex, result.modelSlot);
    }
}

void World::FinishModel(const std::size_t levelIndex, const std::size_t modelSlot)
{
    PendingLevel& pending = pending_[levelIndex];
    PendingModel& pendingModel = pending.models[modelSlot];

    if (pendingModel.ok)
    {
        if (pendingModel.missingTextures > 0)
        {
            LogWarning("Asset", std::format("{} missing texture(s) for {} (magenta fallback)",
                                            pendingModel.missingTextures, pendingModel.model.name));
        }

        Level& level = levels_[levelIndex];
        const std::string modelName = pendingModel.model.name;
        ModelInstance instance;
        instance.modelIndex = registry_->AddModel(std::move(pendingModel.model));
        level.instances.push_back(instance);
        LogInfo("Asset", std::format("Loaded model '{}'", modelName));

        // The registry is reserved up front, so the model (and the skeleton the scene
        // references) never moves.
        const Model& model = registry_->Models()[instance.modelIndex];
        if (physicsScenes_.size() <= instance.modelIndex)
        {
            physicsScenes_.resize(instance.modelIndex + 1);
        }
        if (!model.physics.bodies.empty())
        {
            physicsScenes_[instance.modelIndex] =
                std::make_unique<PhysicsScene>(model.physics, model.skeleton, model.bindPose);
            physicsScenes_[instance.modelIndex]->SetGroundEnabled(physicsGround_);
            LogInfo("Physics", std::format("'{}': {} rigid bodies, {} joints", modelName,
                model.physics.bodies.size(), model.physics.constraints.size()));
        }

        const MeshAsset& mesh = model.mesh;
        for (int axis = 0; axis < 3; ++axis)
        {
            level.boundsMin[axis] = std::min(level.boundsMin[axis], mesh.boundsMin[axis]);
            level.boundsMax[axis] = std::max(level.boundsMax[axis], mesh.boundsMax[axis]);
        }
    }

    ++pending.completed;
    if (pending.completed == pending.models.size())
    {
        pending.loaded = true;
        PositionReflectiveFloors(levelIndex);
        LogInfo("Asset", std::format("Level {} ready", levelIndex));
        if (levelIndex == selectedLevel_)
        {
            ++levelGeneration_;
            camera_.FrameTo(levels_[levelIndex].boundsMin, levels_[levelIndex].boundsMax);
        }
    }
}

void World::SelectLevel(const std::size_t index)
{
    if (index >= levels_.size() || index == selectedLevel_)
    {
        return;
    }
    selectedLevel_ = index;
    // The newly selected level's simulated bones (hair, cloth) restart from the pose instead of
    // resuming a simulation that has been frozen since the level was last shown.
    physicsResetRequested_ = true;
    if (pending_[index].loaded)
    {
        ++levelGeneration_;
        camera_.FrameTo(levels_[index].boundsMin, levels_[index].boundsMax);
    }
    // Otherwise the level is still loading; FinishModel publishes it when its models complete.
}

void World::SetInstanceVisible(const std::size_t index, const bool visible)
{
    if (levels_.empty())
    {
        return;
    }
    std::vector<ModelInstance>& instances = levels_[selectedLevel_].instances;
    if (index >= instances.size())
    {
        return;
    }
    instances[index].visible = visible;
}

void World::InjectReflectiveFloor()
{
    if (registry_ == nullptr || levels_.empty())
    {
        return;
    }

    const std::size_t modelIndex = registry_->AddModel(BuildReflectiveFloorModel());
    for (Level& level : levels_)
    {
        ModelInstance instance;
        instance.modelIndex = modelIndex;
        instance.reflective = true;
        level.instances.push_back(instance);
    }
}

void World::PositionReflectiveFloors(const std::size_t levelIndex)
{
    Level& level = levels_[levelIndex];
    for (ModelInstance& instance : level.instances)
    {
        if (!instance.reflective)
        {
            continue;
        }
        // Place the floor's top (its local y=0 plane) at the level's lowest bound, centered on
        // the models' horizontal extent, so it sits directly under any character. The floor's own
        // huge bounds are deliberately not folded into level framing.
        instance.translation[0] = (level.boundsMin[0] + level.boundsMax[0]) * 0.5f;
        instance.translation[1] = level.boundsMin[1];
        instance.translation[2] = (level.boundsMin[2] + level.boundsMax[2]) * 0.5f;
        instance.rotation[0] = 0.0f;
        instance.rotation[1] = 0.0f;
        instance.rotation[2] = 0.0f;
    }
}

std::span<const ModelInstance> World::SelectedInstances() const
{
    if (levels_.empty())
    {
        return {};
    }
    return levels_[selectedLevel_].instances;
}

PhysicsScene* World::PhysicsFor(const std::size_t modelIndex)
{
    return modelIndex < physicsScenes_.size() ? physicsScenes_[modelIndex].get() : nullptr;
}

void World::SetPhysicsEnabled(const bool enabled)
{
    if (enabled && !physicsEnabled_)
    {
        physicsResetRequested_ = true;
    }
    physicsEnabled_ = enabled;
}

void World::SetPhysicsGround(const bool enabled)
{
    physicsGround_ = enabled;
    for (const std::unique_ptr<PhysicsScene>& scene : physicsScenes_)
    {
        if (scene != nullptr)
        {
            scene->SetGroundEnabled(enabled);
        }
    }
}

bool World::ConsumePhysicsReset()
{
    const bool requested = physicsResetRequested_;
    physicsResetRequested_ = false;
    return requested;
}

void World::LoadMotionsFromDirectory(const std::filesystem::path& directory)
{
    if (!std::filesystem::is_directory(directory))
    {
        return;
    }

    std::vector<std::filesystem::path> paths;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(directory))
    {
        if (entry.is_regular_file() && entry.path().extension() == L".vmd")
        {
            paths.push_back(entry.path());
        }
    }
    std::sort(paths.begin(), paths.end());

    motions_.clear();
    motions_.reserve(paths.size());
    for (const std::filesystem::path& path : paths)
    {
        MotionEntry entry;
        entry.name = WideToUtf8(path.stem().wstring());
        entry.path = path;
        motions_.push_back(std::move(entry));
    }

    LogInfo("App", std::format("Scanned {} motion(s) in {}", motions_.size(), WideToUtf8(directory.wstring())));
}

void World::SelectMotion(const std::uint32_t index)
{
    if (index >= motions_.size() || index == selectedMotion_)
    {
        return;
    }

    try
    {
        VmdMotion motion = ParseVmdFile(motions_[index].path);
        const std::size_t trackCount = motion.boneTracks.size();
        animator_.SetMotion(std::move(motion));
        selectedMotion_ = index;
        LogInfo("App", std::format("Loaded motion '{}' ({} bone tracks)", motions_[index].name, trackCount));
    }
    catch (const std::exception& exception)
    {
        LogError("Asset", std::format("Failed to load motion '{}': {}",
            motions_[index].path.string(), exception.what()));
    }
}

void World::SetMotionPlaying(const bool playing)
{
    animator_.SetPlaying(playing);
}

void World::SeekMotion(const float frames)
{
    animator_.SeekFrames(frames);
}
} // namespace MmdLab
