#pragma once

#include <cstdint>

namespace MmdLab
{
// The number of logical processors visible to the process across all processor groups. This is
// the raw platform CPU budget that worker-group policies derive their sizes from; a policy
// subtracts the pipeline's needs before sizing a group.
[[nodiscard]] std::uint32_t PlatformLogicalProcessorCount();

// The number of I/O worker threads for asset loading, derived from the platform CPU budget.
// Placeholder policy: clamp to a small pool; revisit once measured topology justifies more (do
// not pin threads or reserve physical cores before profiling).
[[nodiscard]] std::uint32_t IoWorkerCount();

// The number of compute worker threads for CPU-bound frame work (per-model animation and
// physics), derived from the same budget as IoWorkerCount. The GameThread also runs compute jobs
// while it waits, so it is not counted here. Placeholder policy pending measured topology.
[[nodiscard]] std::uint32_t ComputeWorkerCount();
} // namespace MmdLab
