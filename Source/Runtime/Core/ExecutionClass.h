#pragma once

#include <cstdint>

namespace MmdLab
{
// What an operating-system thread is for, declared by whoever creates it. The runtime states only
// intent; the platform scheduling policy (ThreadScheduling) turns each class into concrete
// operating-system settings such as priority and processor placement. A thread's class never
// names a processor.
enum class ExecutionClass : std::uint8_t
{
    GameOwner,   // GameThread: mutable authoritative state, and it runs compute jobs in ParallelFor.
    RenderOwner, // RenderThread: render work compilation and render-frame ownership.
    RhiOwner,    // RhiThread: native DX12 resource lifetime, submission, and presentation.
    Compute,     // ComputeThreadsGroup workers: CPU-bound per-frame jobs.
    AsyncIo,     // IoThreadsGroup workers: file reads and the decode they still perform.
};
} // namespace MmdLab
