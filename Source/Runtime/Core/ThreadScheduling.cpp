#include "Runtime/Core/ThreadScheduling.h"

#include "Runtime/Core/Log.h"

#include <windows.h>

#include <format>
#include <limits>

namespace MmdLab
{
static_assert(sizeof(ULONG) == sizeof(std::uint32_t), "CPU set ids are passed to Windows as ULONG.");

SchedulingPolicy PolicyFor(const ExecutionClass executionClass)
{
    switch (executionClass)
    {
        case ExecutionClass::GameOwner:
            // The GameThread runs one of each ParallelFor batch's jobs, so it takes the compute
            // placement too; otherwise it can land on a worker's sibling and bring the co-placement
            // back.
            return { ThreadPriority::AboveNormal, ProcessorPlacement::OneLogicalProcessorPerCore };
        case ExecutionClass::RenderOwner:
        case ExecutionClass::RhiOwner:
            return { ThreadPriority::AboveNormal, ProcessorPlacement::OsManaged };
        case ExecutionClass::Compute:
            // Measured: left to the operating system, about half of the concurrent physics steps
            // ran next to another step on the same physical core and took 10-25% longer.
            return { ThreadPriority::Normal, ProcessorPlacement::OneLogicalProcessorPerCore };
        case ExecutionClass::AsyncIo:
            return { ThreadPriority::Normal, ProcessorPlacement::OsManaged };
    }
    return {};
}

std::vector<std::uint32_t> PrimaryCpuSetIds(const CpuTopology& topology)
{
    std::vector<std::uint32_t> ids;
    ids.reserve(topology.cores.size());
    for (const CpuCore& core : topology.cores)
    {
        if (!core.cpuSetIds.empty())
        {
            ids.push_back(core.cpuSetIds.front());
        }
    }
    return ids;
}

std::size_t MaxComputeWorkers(const SchedulingPolicy& computePolicy, const CpuTopology& topology)
{
    if (computePolicy.placement != ProcessorPlacement::OneLogicalProcessorPerCore || topology.cores.empty())
    {
        return std::numeric_limits<std::size_t>::max();
    }
    return topology.cores.size() - 1;
}

void ApplyThreadScheduling(const ExecutionClass executionClass, const CpuTopology& topology)
{
    const SchedulingPolicy policy = PolicyFor(executionClass);
    const HANDLE thread = GetCurrentThread();

    const int priority = policy.priority == ThreadPriority::AboveNormal ? THREAD_PRIORITY_ABOVE_NORMAL
                                                                         : THREAD_PRIORITY_NORMAL;
    if (!SetThreadPriority(thread, priority))
    {
        LogWarning("Core", std::format("SetThreadPriority failed (error {})", GetLastError()));
    }

    if (policy.placement == ProcessorPlacement::OneLogicalProcessorPerCore)
    {
        // CPU sets rather than an affinity mask: measured, a hard single-processor affinity made
        // a woken worker wait up to a millisecond for its one processor, while the CPU-set
        // restriction removed the co-placement at little start-latency cost.
        const std::vector<std::uint32_t> ids = PrimaryCpuSetIds(topology);
        if (!ids.empty()
            && !SetThreadSelectedCpuSets(thread, reinterpret_cast<const ULONG*>(ids.data()), static_cast<ULONG>(ids.size())))
        {
            LogWarning("Core", std::format("SetThreadSelectedCpuSets failed (error {})", GetLastError()));
        }
    }
}

void ApplyThreadScheduling(const ExecutionClass executionClass)
{
    ApplyThreadScheduling(executionClass, QueryCpuTopology());
}
} // namespace MmdLab
