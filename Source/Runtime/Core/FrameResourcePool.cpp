#include "Runtime/Core/FrameResourcePool.h"

#include <stdexcept>

namespace MmdLab
{
FrameResourcePool::FrameResourcePool()
{
    for (std::size_t i = 0; i < kFrameCount; ++i)
    {
        freeIndices_[i] = static_cast<FrameIndex>(i);
    }
}

FrameIndex FrameResourcePool::Acquire()
{
    std::unique_lock lock(mutex_);
    notEmpty_.wait(lock, [this] { return freeCount_ > 0; });
    --freeCount_;
    const FrameIndex index = freeIndices_[freeCount_];
    inUse_[index] = true;
    return index;
}

void FrameResourcePool::Release(const FrameIndex index)
{
    {
        std::lock_guard lock(mutex_);
        if (index >= kFrameCount)
        {
            throw std::runtime_error("FrameResourcePool::Release: index out of range.");
        }
        if (!inUse_[index])
        {
            throw std::runtime_error("FrameResourcePool::Release: the frame is not in use.");
        }
        inUse_[index] = false;
        freeIndices_[freeCount_] = index;
        ++freeCount_;
    }
    notEmpty_.notify_one();
}

FrameResource& FrameResourcePool::Get(const FrameIndex index)
{
    return frames_[index];
}

const FrameResource& FrameResourcePool::Get(const FrameIndex index) const
{
    return frames_[index];
}

std::size_t FrameResourcePool::FreeCount() const
{
    std::lock_guard lock(mutex_);
    return freeCount_;
}
} // namespace MmdLab
