#pragma once

#include "Runtime/Asset/Model.h"

#include <cstddef>
#include <filesystem>
#include <vector>

namespace MmdLab
{
// The authoritative registry of loaded Model assets, owned by the GameThread alongside the
// World. Models are loaded once and never mutated, so spans into them stay valid for the
// session. The World references models by index; keeping the registry here rather than in the
// World mirrors the engine split between content assets and the runtime world.
//
// Loading is split into a pure, thread-safe "parse the mesh" step (ParseModelFile, run on the
// I/O threads; textures are decoded separately) and a GameThread-only "install it" step
// (AddModel), so the registry stays single-owner while the work happens off the game loop.
class ModelRegistry final
{
public:
    // Reserves the model pool to the scan's total model count, so loads arriving during the
    // render loop never reallocate the vector the renderer holds spans into across frames.
    void Reserve(std::size_t count);

    // Parses one .pmx into a Model's mesh and name without decoding textures, so it can run on
    // the I/O thread; the texture paths stay in model.mesh.textures and are decoded separately.
    // Throws std::runtime_error when the file cannot be parsed or converted.
    [[nodiscard]] static Model ParseModelFile(const std::filesystem::path& path);

    // Loads one cooked .mmdl into a Model the same way, without any PMX parse. This is the fast
    // path once an asset has been cooked offline.
    [[nodiscard]] static Model ParseModelFileFromMmdl(const std::filesystem::path& path);

    // Installs a finished model into the registry and returns its index. GameThread-only.
    std::size_t AddModel(Model&& model);

    [[nodiscard]] std::size_t ModelCount() const { return models_.size(); }
    [[nodiscard]] const std::vector<Model>& Models() const { return models_; }

private:
    std::vector<Model> models_;
};
} // namespace MmdLab
