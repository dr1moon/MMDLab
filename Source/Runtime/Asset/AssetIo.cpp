#include "Runtime/Asset/AssetIo.h"

#include "Runtime/Asset/ModelRegistry.h"
#include "Runtime/Core/Log.h"
#include "Runtime/Core/Thread.h"
#include "Runtime/Core/Utf8.h"

#include <chrono>
#include <exception>
#include <format>
#include <utility>

namespace MmdLab
{
IoWorker::IoWorker(
    Channel<LoadRequest, kLoadRequestCapacity>& requests,
    Channel<LoadResult, kLoadResultCapacity>& results)
    : requests_(&requests)
    , results_(&results)
{
}

uint32_t IoWorker::Run()
{
    while (const auto request = requests_->Pop())
    {
        std::visit(Overloaded{
            [&](const ModelParseRequest& r)
            {
                const auto start = std::chrono::steady_clock::now();
                ModelParseResult result;
                result.levelIndex = r.levelIndex;
                result.modelSlot = r.modelSlot;
                try
                {
                    result.model = ModelRegistry::ParseModelFile(r.path);
                    const std::filesystem::path base = r.path.parent_path();
                    result.texturePaths.reserve(result.model.mesh.textures.size());
                    for (const std::string& relative : result.model.mesh.textures)
                    {
                        result.texturePaths.push_back(base / PathFromUtf8(relative));
                    }
                    result.ok = true;
                }
                catch (const std::exception& exception)
                {
                    result.ok = false;
                    result.error = exception.what();
                    LogError("Asset", std::format("Failed to parse PMX model '{}': {}", r.path.string(), exception.what()));
                }
                const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - start).count();
                if (result.ok)
                {
                    LogInfo("Asset", std::format("Parsed model '{}' in {} ms", result.model.name, ms));
                }
                results_->Push(std::move(result));
            },
            [&](const TextureDecodeRequest& r)
            {
                const auto start = std::chrono::steady_clock::now();
                TextureDecodeResult result;
                result.levelIndex = r.levelIndex;
                result.modelSlot = r.modelSlot;
                result.textureIndex = r.textureIndex;
                try
                {
                    result.image = LoadTexture(r.path);
                    result.ok = true;
                }
                catch (const std::exception&)
                {
                    result.ok = false;
                }
                const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - start).count();
                const std::string fileName = WideToUtf8(r.path.filename().wstring());
                if (result.ok)
                {
                    LogInfo("Asset", std::format("Decoded texture '{}' in {} ms", fileName, ms));
                }
                else
                {
                    LogWarning("Asset", std::format("Failed to decode texture '{}'", fileName));
                }
                results_->Push(std::move(result));
            },
        }, *request);
    }
    return 0;
}

IoThreadsGroup::IoThreadsGroup(
    const std::size_t threadCount,
    Channel<LoadRequest, kLoadRequestCapacity>& requests,
    Channel<LoadResult, kLoadResultCapacity>& results)
    : requests_(&requests)
    , results_(&results)
{
    const std::size_t count = threadCount == 0 ? 1 : threadCount;
    LogInfo("Core", std::format("IoThreadsGroup: {} worker(s)", count));

    workers_.reserve(count);
    threads_.reserve(count);
    for (std::size_t i = 0; i < count; ++i)
    {
        workers_.push_back(std::make_unique<IoWorker>(requests, results));
        threads_.push_back(std::make_unique<Thread>(*workers_.back(), L"IoThreadsGroup-" + std::to_wstring(i)));
    }
}

IoThreadsGroup::~IoThreadsGroup()
{
    Stop();
}

void IoThreadsGroup::Stop()
{
    if (requests_ != nullptr)
    {
        requests_->Close();
    }
    threads_.clear(); // Each Thread's destructor joins its worker (draining the closed queue).
    workers_.clear();
}
} // namespace MmdLab
