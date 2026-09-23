---
kind: technique
tags:
  - hardware/cpu
  - cpu-topology
  - mermaid
  - obsidian
status: stable
source:
  - Local Windows topology APIs
  - "D:\\MMDLab\\Tools\\Generate-CpuTopology.ps1"
created: 2026-09-23
updated: 2026-09-23
---

# CPU Topology Note Format

Example: [[AMD-Ryzen-7-9700X]]

> [!abstract]
> This format converts local Windows CPU topology information into an Obsidian note with Mermaid diagrams, tables, provenance, and explicit uncertainty boundaries.

## Purpose

- Record the CPU topology visible to the current process.
- Make core, logical-processor, cache, NUMA, and operating-system scheduling-group relationships inspectable in Obsidian.
- Preserve the topology as a standalone local hardware observation.
- Provide reproducible input for CPU scheduling and locality investigations.

## Generated Note Contract

Every generated CPU note contains:

```text
Frontmatter
  kind: fieldnote
  tags: hardware/cpu, generated
  source: local Windows topology APIs
  generated and updated dates

Body
  CPU model title
  Processor summary table
  Mermaid core and processor-group graph
  Core-to-logical-processor table
  Mermaid shared-cache graph
  Cache detail table
  NUMA table
  Operating-system scheduling-group table
```

## Mermaid Conventions

### Core Graph

```text
CPU
  -> Operating-System Scheduling Group
  -> Physical Core
  -> Logical Processor
```

The core graph is centered on operating-system scheduling visibility. It does not claim that a processor group, core, or logical processor is a performance recommendation.

### Shared-Cache Graph

Only caches shared by multiple reported cores are drawn as Mermaid edges. Private first-level and second-level caches remain in the cache table so the graph stays readable.

```text
Shared Cache
  -> Core 0
  -> Core 1
  -> Core N
```

## Data Fidelity Rules

- The generator reports only relationships returned by standard Windows topology APIs and basic local system inventory.
- It records cache sharing by logical processor, then derives sharing cores for display.
- It does not infer AMD core complexes, ARM clusters, efficiency-core groups, or other vendor-specific clusters from cache size alone.
- Mermaid output is an inspection aid, not a performance recommendation.

## Refresh Procedure

Run the generator after hardware, firmware, operating-system, or process-affinity changes:

```powershell
& 'D:\MMDLab\Tools\Generate-CpuTopology.ps1' `
  -OutputPath 'D:\Drimoon\CPU\AMD-Ryzen-7-9700X.md'
```

For another machine, use a filename derived from its CPU model. The generated heading is read from the local processor inventory.
