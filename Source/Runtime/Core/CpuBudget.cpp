#include "Runtime/Core/CpuBudget.h"

#include <windows.h>

#include <algorithm>

namespace MmdLab
{
std::uint32_t PlatformLogicalProcessorCount()
{
    const DWORD count = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
    return count == 0 ? 1u : static_cast<std::uint32_t>(count);
}

namespace
{
// The processors a worker group may use: the platform budget minus a few for the Game/Render/RHI
// ownership contexts and the OS. Placeholder policy pending measured topology.
std::uint32_t WorkerBudget()
{
    const std::uint32_t logical = PlatformLogicalProcessorCount();
    return logical > 4 ? logical - 4 : 1;
}
} // namespace

std::uint32_t IoWorkerCount()
{
    // Loading is a short startup burst of cooked reads (PMX parse and texture decode still run
    // here), and the CPU-bound frame work belongs to the compute group, so the I/O group stays
    // small instead of claiming the whole budget alongside it.
    return std::min(WorkerBudget(), 4u);
}

std::uint32_t ComputeWorkerCount()
{
    return WorkerBudget();
}
} // namespace MmdLab
