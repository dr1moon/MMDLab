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

std::uint32_t IoWorkerCount()
{
    // I/O loading is CPU-bound (PMX parse + image decode), so size the pool to most of the
    // platform budget, leaving a few processors for the Game/Render/RHI pipeline and the OS.
    // Placeholder policy pending measured topology.
    const std::uint32_t logical = PlatformLogicalProcessorCount();
    const std::uint32_t budget = logical > 4 ? logical - 4 : 1;
    return std::min(budget, 12u);
}
} // namespace MmdLab
