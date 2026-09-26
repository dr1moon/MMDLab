#pragma once

#include "Runtime/Core/FrameResource.h"

#include <condition_variable>
#include <cstddef>
#include <mutex>

namespace MmdLab
{
// Owns the fixed set of three FrameResource objects and hands out FrameIndex handles.
//
// GameThread acquires a free FrameIndex before writing a frame; RhiThread releases the
// index after the GPU has finished with the frame (fence retirement). When all three
// resources are in flight, Acquire() blocks; this is the bounded back-pressure gate that
// stops GameThread from running more than three frames ahead of the GPU.
class FrameResourcePool final
{
public:
    static constexpr std::size_t kFrameCount = 3;

    FrameResourcePool();

    FrameResourcePool(const FrameResourcePool&) = delete;
    FrameResourcePool& operator=(const FrameResourcePool&) = delete;

    // Blocks until a frame resource is free, then returns its index.
    FrameIndex Acquire();

    // Returns a frame resource to the free pool. Called only after GPU retirement.
    // Throws if index is out of range or was not acquired (double release).
    void Release(FrameIndex index);

    // Access to the frame resource at index. index must have come from Acquire().
    [[nodiscard]] FrameResource& Get(FrameIndex index);
    [[nodiscard]] const FrameResource& Get(FrameIndex index) const;

    [[nodiscard]] std::size_t FreeCount() const;

private:
    FrameResource frames_[kFrameCount];
    FrameIndex freeIndices_[kFrameCount];
    bool inUse_[kFrameCount] = {};
    std::size_t freeCount_ = kFrameCount;
    mutable std::mutex mutex_;
    std::condition_variable notEmpty_;
};
} // namespace MmdLab
