#include "Runtime/Core/CpuTopology.h"
#include "Runtime/Core/TestFramework.h"
#include "Runtime/Core/ThreadScheduling.h"

#include <windows.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <set>
#include <thread>
#include <vector>

namespace
{
// A homogeneous topology of `coreCount` two-way SMT cores: core c runs logical processors 2c and
// 2c + 1, whose CPU set ids are 256 + 2c and 256 + 2c + 1 (Windows numbers CPU sets from 256).
MmdLab::CpuTopology SmtTopology(const std::size_t coreCount)
{
    MmdLab::CpuTopology topology;
    for (std::size_t c = 0; c < coreCount; ++c)
    {
        MmdLab::CpuCore core;
        core.coreIndex = static_cast<std::uint8_t>(c);
        core.logicalProcessors = { static_cast<std::uint8_t>(2 * c), static_cast<std::uint8_t>(2 * c + 1) };
        core.cpuSetIds = { static_cast<std::uint32_t>(256 + 2 * c), static_cast<std::uint32_t>(256 + 2 * c + 1) };
        topology.cores.push_back(core);
    }
    return topology;
}

// The calling thread's selected CPU sets, sorted; empty when none are selected (OS-managed).
std::vector<std::uint32_t> SelectedCpuSets()
{
    ULONG required = 0;
    GetThreadSelectedCpuSets(GetCurrentThread(), nullptr, 0, &required);
    std::vector<ULONG> ids(required);
    if (required > 0)
    {
        GetThreadSelectedCpuSets(GetCurrentThread(), ids.data(), required, &required);
    }
    std::vector<std::uint32_t> result(ids.begin(), ids.end());
    std::sort(result.begin(), result.end());
    return result;
}

struct AppliedScheduling
{
    int priority = 0;
    std::vector<std::uint32_t> cpuSets;
};

// Applies `executionClass`'s policy on a fresh thread (so the test runner's own thread is never
// modified) and reads back what the operating system recorded for it.
AppliedScheduling ApplyOnFreshThread(const MmdLab::ExecutionClass executionClass)
{
    AppliedScheduling applied;
    std::thread thread([&]
    {
        MmdLab::ApplyThreadScheduling(executionClass);
        applied.priority = GetThreadPriority(GetCurrentThread());
        applied.cpuSets = SelectedCpuSets();
    });
    thread.join();
    return applied;
}

std::vector<std::uint32_t> SortedPrimaryIds()
{
    std::vector<std::uint32_t> ids = MmdLab::PrimaryCpuSetIds(MmdLab::QueryCpuTopology());
    std::sort(ids.begin(), ids.end());
    return ids;
}
} // namespace

MMDLAB_TEST(Core.ThreadScheduling, PrimaryCpuSetsTakeTheLowestSiblingOfEachCore)
{
    const std::vector<std::uint32_t> ids = MmdLab::PrimaryCpuSetIds(SmtTopology(4));

    MMDLAB_CHECK((ids == std::vector<std::uint32_t>{ 256, 258, 260, 262 }));
}

MMDLAB_TEST(Core.ThreadScheduling, OneProcessorPerCoreLeavesACoreForTheGameThread)
{
    const MmdLab::SchedulingPolicy compute{ MmdLab::ThreadPriority::Normal,
        MmdLab::ProcessorPlacement::OneLogicalProcessorPerCore };

    MMDLAB_CHECK_EQUAL(static_cast<std::size_t>(7), MmdLab::MaxComputeWorkers(compute, SmtTopology(8)));
    MMDLAB_CHECK_EQUAL(static_cast<std::size_t>(0), MmdLab::MaxComputeWorkers(compute, SmtTopology(1)));
}

MMDLAB_TEST(Core.ThreadScheduling, OsManagedOrUnknownTopologyDoesNotCapWorkers)
{
    const MmdLab::SchedulingPolicy osManaged{};
    const MmdLab::SchedulingPolicy compute{ MmdLab::ThreadPriority::Normal,
        MmdLab::ProcessorPlacement::OneLogicalProcessorPerCore };
    constexpr std::size_t kUnlimited = std::numeric_limits<std::size_t>::max();

    MMDLAB_CHECK_EQUAL(kUnlimited, MmdLab::MaxComputeWorkers(osManaged, SmtTopology(8)));
    MMDLAB_CHECK_EQUAL(kUnlimited, MmdLab::MaxComputeWorkers(compute, MmdLab::CpuTopology{}));
}

MMDLAB_TEST(Core.ThreadScheduling, PolicyTableMatchesTheMeasuredChoices)
{
    using MmdLab::ExecutionClass;
    using MmdLab::ProcessorPlacement;
    using MmdLab::ThreadPriority;
    MMDLAB_CHECK(MmdLab::PolicyFor(ExecutionClass::Compute).placement == ProcessorPlacement::OneLogicalProcessorPerCore);
    MMDLAB_CHECK(MmdLab::PolicyFor(ExecutionClass::GameOwner).placement == ProcessorPlacement::OneLogicalProcessorPerCore);
    MMDLAB_CHECK(MmdLab::PolicyFor(ExecutionClass::RenderOwner).placement == ProcessorPlacement::OsManaged);
    MMDLAB_CHECK(MmdLab::PolicyFor(ExecutionClass::RhiOwner).placement == ProcessorPlacement::OsManaged);
    MMDLAB_CHECK(MmdLab::PolicyFor(ExecutionClass::AsyncIo).placement == ProcessorPlacement::OsManaged);
    MMDLAB_CHECK(MmdLab::PolicyFor(ExecutionClass::GameOwner).priority == ThreadPriority::AboveNormal);
    MMDLAB_CHECK(MmdLab::PolicyFor(ExecutionClass::Compute).priority == ThreadPriority::Normal);
}

MMDLAB_TEST(Core.CpuTopology, ListsEveryLogicalProcessorOnce)
{
    const MmdLab::CpuTopology topology = MmdLab::QueryCpuTopology();
    std::set<std::pair<std::uint16_t, std::uint8_t>> seen;
    std::set<std::uint32_t> ids;
    bool wellFormed = !topology.cores.empty();
    for (const MmdLab::CpuCore& core : topology.cores)
    {
        wellFormed = wellFormed && !core.logicalProcessors.empty()
            && core.logicalProcessors.size() == core.cpuSetIds.size()
            && std::is_sorted(core.logicalProcessors.begin(), core.logicalProcessors.end());
        for (std::size_t i = 0; i < core.logicalProcessors.size(); ++i)
        {
            wellFormed = seen.insert({ core.group, core.logicalProcessors[i] }).second && wellFormed;
            wellFormed = ids.insert(core.cpuSetIds[i]).second && wellFormed;
        }
    }

    MMDLAB_CHECK(wellFormed);
    MMDLAB_CHECK_EQUAL(static_cast<std::size_t>(GetActiveProcessorCount(ALL_PROCESSOR_GROUPS)),
        topology.LogicalProcessorCount());
}

MMDLAB_TEST(Core.ThreadScheduling, ComputeThreadIsRestrictedToOneProcessorPerCore)
{
    const AppliedScheduling applied = ApplyOnFreshThread(MmdLab::ExecutionClass::Compute);

    MMDLAB_CHECK(applied.cpuSets == SortedPrimaryIds());
    MMDLAB_CHECK_EQUAL(THREAD_PRIORITY_NORMAL, applied.priority);
}

MMDLAB_TEST(Core.ThreadScheduling, GameThreadIsRestrictedAndRaised)
{
    const AppliedScheduling applied = ApplyOnFreshThread(MmdLab::ExecutionClass::GameOwner);

    MMDLAB_CHECK(applied.cpuSets == SortedPrimaryIds());
    MMDLAB_CHECK_EQUAL(THREAD_PRIORITY_ABOVE_NORMAL, applied.priority);
}

MMDLAB_TEST(Core.ThreadScheduling, OsManagedThreadSelectsNoCpuSets)
{
    const AppliedScheduling io = ApplyOnFreshThread(MmdLab::ExecutionClass::AsyncIo);
    const AppliedScheduling rhi = ApplyOnFreshThread(MmdLab::ExecutionClass::RhiOwner);

    MMDLAB_CHECK(io.cpuSets.empty());
    MMDLAB_CHECK_EQUAL(THREAD_PRIORITY_NORMAL, io.priority);
    MMDLAB_CHECK(rhi.cpuSets.empty());
    MMDLAB_CHECK_EQUAL(THREAD_PRIORITY_ABOVE_NORMAL, rhi.priority);
}
