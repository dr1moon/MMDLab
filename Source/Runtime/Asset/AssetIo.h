#pragma once

#include "Runtime/Asset/ImageLoader.h"
#include "Runtime/Asset/Model.h"
#include "Runtime/Core/Channel.h"
#include "Runtime/Core/Runnable.h"

#include <cstddef>
#include <filesystem>
#include <memory>
#include <string>
#include <variant>
#include <vector>

namespace MmdLab
{
class Thread;

// Capacities for the async asset-load edges. The request queue carries the startup parse burst
// plus texture-decode jobs the GameThread fans out as parses complete; the result queue carries
// parse results and texture-decode results until the GameThread installs them.
inline constexpr std::size_t kLoadRequestCapacity = 512;
inline constexpr std::size_t kLoadResultCapacity = 512;

// GameThread -> worker: parse one model's PMX into its mesh, name, and texture paths.
struct ModelParseRequest
{
    std::size_t levelIndex = 0;
    std::size_t modelSlot = 0;
    std::filesystem::path path;
};

// GameThread -> worker: decode one texture into RGBA8.
struct TextureDecodeRequest
{
    std::size_t levelIndex = 0;
    std::size_t modelSlot = 0;
    std::size_t textureIndex = 0;
    std::filesystem::path path;
};

// One unit of work on the shared queue: either parse a model or decode one of its textures.
using LoadRequest = std::variant<ModelParseRequest, TextureDecodeRequest>;

// worker -> GameThread: one model parsed. `textures` is empty; the texture paths are the full
// paths in `texturePaths`, 1:1 with the decode order.
struct ModelParseResult
{
    std::size_t levelIndex = 0;
    std::size_t modelSlot = 0;
    bool ok = false;
    std::string error;
    Model model;
    std::vector<std::filesystem::path> texturePaths;
};

// worker -> GameThread: one texture decoded (or failed).
struct TextureDecodeResult
{
    std::size_t levelIndex = 0;
    std::size_t modelSlot = 0;
    std::size_t textureIndex = 0;
    bool ok = false;
    Image image;
};

using LoadResult = std::variant<ModelParseResult, TextureDecodeResult>;

// Dispatch helper for std::visit over the load request/result variants.
template <class... Ts>
struct Overloaded : Ts...
{
    using Ts::operator()...;
};
template <class... Ts>
Overloaded(Ts...) -> Overloaded<Ts...>;

// One I/O worker: drains the shared request queue (parse and texture-decode jobs) and pushes the
// corresponding results. Instances run inside an IoThreadsGroup's threads; both queues are
// mutex-guarded Channels, so sharing them across workers stays correct.
class IoWorker final : public Runnable
{
public:
    IoWorker(
        Channel<LoadRequest, kLoadRequestCapacity>& requests,
        Channel<LoadResult, kLoadResultCapacity>& results);

    uint32_t Run() override;

private:
    Channel<LoadRequest, kLoadRequestCapacity>* requests_;
    Channel<LoadResult, kLoadResultCapacity>* results_;
};

// The I/O execution resource: a group of worker threads that load assets off the GameThread.
// Parsing a model and decoding each of its textures are separate jobs on one shared queue, so
// texture decode spreads across every worker instead of serializing inside one model. The worker
// count comes from the platform CPU budget (see CpuBudget), not a hardcoded constant.
class IoThreadsGroup final
{
public:
    IoThreadsGroup(
        std::size_t threadCount,
        Channel<LoadRequest, kLoadRequestCapacity>& requests,
        Channel<LoadResult, kLoadResultCapacity>& results);
    ~IoThreadsGroup();

    IoThreadsGroup(const IoThreadsGroup&) = delete;
    IoThreadsGroup& operator=(const IoThreadsGroup&) = delete;

    // Closes the request queue and joins all workers.
    void Stop();

private:
    std::vector<std::unique_ptr<IoWorker>> workers_;
    std::vector<std::unique_ptr<Thread>> threads_;
    Channel<LoadRequest, kLoadRequestCapacity>* requests_;
    Channel<LoadResult, kLoadResultCapacity>* results_;
};
} // namespace MmdLab
