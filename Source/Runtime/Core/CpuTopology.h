#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace MmdLab
{
// One physical core and the logical processors (simultaneous-multithreading siblings) it runs,
// as the operating system reports them. `logicalProcessors` and `cpuSetIds` are parallel and
// sorted by logical processor, so the first entry is the core's lowest-numbered sibling.
struct CpuCore
{
    std::uint16_t group = 0; // Operating-system processor group.
    std::uint8_t coreIndex = 0; // Group-relative core index reported by the operating system.
    std::uint8_t efficiencyClass = 0; // Higher is more performant; equal on homogeneous CPUs.
    std::vector<std::uint8_t> logicalProcessors; // Group-relative logical processor indices.
    std::vector<std::uint32_t> cpuSetIds; // Operating-system CPU set ids, parallel to the above.
};

// The part of the platform CPU topology the scheduling policy needs: which logical processors
// share a physical core. Caches, NUMA nodes, and clusters are separate relations (see the CPU
// topology model in Source/README.md) and are added when a policy consumes them.
struct CpuTopology
{
    std::vector<CpuCore> cores; // Sorted by (group, coreIndex).

    [[nodiscard]] std::size_t LogicalProcessorCount() const;
};

// Queries the topology visible to this process from the operating system's CPU-set information.
// Returns an empty topology if the query fails.
[[nodiscard]] CpuTopology QueryCpuTopology();
} // namespace MmdLab
