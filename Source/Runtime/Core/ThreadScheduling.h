#pragma once

#include "Runtime/Core/CpuTopology.h"
#include "Runtime/Core/ExecutionClass.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace MmdLab
{
// Where the operating system may run a thread.
enum class ProcessorPlacement : std::uint8_t
{
    OsManaged,                  // Any logical processor; the operating system decides.
    OneLogicalProcessorPerCore, // Only the lowest-numbered logical processor of each physical core.
};

enum class ThreadPriority : std::uint8_t
{
    Normal,
    AboveNormal,
};

// The concrete operating-system settings one execution class maps to on this platform.
struct SchedulingPolicy
{
    ThreadPriority priority = ThreadPriority::Normal;
    ProcessorPlacement placement = ProcessorPlacement::OsManaged;
};

// The Windows policy table. Each entry is justified by measurement, not taken as a general rule:
// see "Execution Classes and Scheduling Policy" in Source/README.md.
[[nodiscard]] SchedulingPolicy PolicyFor(ExecutionClass executionClass);

// The CPU set ids a OneLogicalProcessorPerCore thread may run on: the lowest-numbered logical
// processor of every core, so no two such threads can occupy the two siblings of one core.
[[nodiscard]] std::vector<std::uint32_t> PrimaryCpuSetIds(const CpuTopology& topology);

// The most compute workers `computePolicy` leaves room for on `topology`. With one logical
// processor per core, the workers and the GameThread (which runs compute jobs too) need a core
// each, so the workers get one core fewer than the topology has. Unlimited when OS-managed.
[[nodiscard]] std::size_t MaxComputeWorkers(const SchedulingPolicy& computePolicy, const CpuTopology& topology);

// Applies `executionClass`'s policy to the calling thread. A setting the operating system rejects
// is logged and skipped: a thread with the wrong placement is slower, never incorrect.
void ApplyThreadScheduling(ExecutionClass executionClass, const CpuTopology& topology);
void ApplyThreadScheduling(ExecutionClass executionClass);
} // namespace MmdLab
