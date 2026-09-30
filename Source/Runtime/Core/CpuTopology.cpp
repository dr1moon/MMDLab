#include "Runtime/Core/CpuTopology.h"

#include <windows.h>

#include <algorithm>
#include <cstddef>
#include <map>
#include <utility>

namespace MmdLab
{
std::size_t CpuTopology::LogicalProcessorCount() const
{
    std::size_t count = 0;
    for (const CpuCore& core : cores)
    {
        count += core.logicalProcessors.size();
    }
    return count;
}

CpuTopology QueryCpuTopology()
{
    ULONG length = 0;
    GetSystemCpuSetInformation(nullptr, 0, &length, GetCurrentProcess(), 0);
    if (length == 0)
    {
        return {};
    }
    std::vector<std::byte> buffer(length);
    auto* const first = reinterpret_cast<PSYSTEM_CPU_SET_INFORMATION>(buffer.data());
    if (!GetSystemCpuSetInformation(first, length, &length, GetCurrentProcess(), 0))
    {
        return {};
    }

    // Group the CPU sets by physical core; each entry is one logical processor.
    std::map<std::pair<std::uint16_t, std::uint8_t>, std::vector<std::pair<std::uint8_t, std::uint32_t>>> byCore;
    std::map<std::pair<std::uint16_t, std::uint8_t>, std::uint8_t> efficiency;
    for (std::size_t offset = 0; offset < length;)
    {
        const auto* entry = reinterpret_cast<const SYSTEM_CPU_SET_INFORMATION*>(buffer.data() + offset);
        if (entry->Size == 0)
        {
            break;
        }
        if (entry->Type == CpuSetInformation)
        {
            const auto key = std::make_pair(entry->CpuSet.Group, entry->CpuSet.CoreIndex);
            byCore[key].emplace_back(entry->CpuSet.LogicalProcessorIndex, entry->CpuSet.Id);
            efficiency[key] = entry->CpuSet.EfficiencyClass;
        }
        offset += entry->Size;
    }

    CpuTopology topology;
    topology.cores.reserve(byCore.size());
    for (auto& [key, processors] : byCore)
    {
        std::sort(processors.begin(), processors.end());
        CpuCore core;
        core.group = key.first;
        core.coreIndex = key.second;
        core.efficiencyClass = efficiency[key];
        for (const auto& [processor, cpuSetId] : processors)
        {
            core.logicalProcessors.push_back(processor);
            core.cpuSetIds.push_back(cpuSetId);
        }
        topology.cores.push_back(std::move(core));
    }
    return topology;
}
} // namespace MmdLab
