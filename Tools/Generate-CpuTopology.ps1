param(
    [Parameter(Mandatory = $true)]
    [string]$OutputPath
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$typeDefinition = @'
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;

public sealed class GroupMaskRecord
{
    public ushort Group { get; set; }
    public ulong Mask { get; set; }
}

public sealed class ProcessorRecord
{
    public byte EfficiencyClass { get; set; }
    public List<GroupMaskRecord> Masks { get; set; }

    public ProcessorRecord()
    {
        Masks = new List<GroupMaskRecord>();
    }
}

public sealed class CacheRecord
{
    public byte Level { get; set; }
    public byte Associativity { get; set; }
    public ushort LineSizeBytes { get; set; }
    public uint SizeBytes { get; set; }
    public int Kind { get; set; }
    public GroupMaskRecord Mask { get; set; }
}

public sealed class NumaRecord
{
    public uint NodeNumber { get; set; }
    public GroupMaskRecord Mask { get; set; }
}

public sealed class ProcessorGroupRecord
{
    public ushort GroupNumber { get; set; }
    public uint ActiveProcessorCount { get; set; }
}

public sealed class TopologySnapshot
{
    public List<ProcessorRecord> Cores { get; set; }
    public List<ProcessorRecord> Packages { get; set; }
    public List<ProcessorRecord> Dies { get; set; }
    public List<CacheRecord> Caches { get; set; }
    public List<NumaRecord> NumaNodes { get; set; }
    public List<ProcessorGroupRecord> ProcessorGroups { get; set; }

    public TopologySnapshot()
    {
        Cores = new List<ProcessorRecord>();
        Packages = new List<ProcessorRecord>();
        Dies = new List<ProcessorRecord>();
        Caches = new List<CacheRecord>();
        NumaNodes = new List<NumaRecord>();
        ProcessorGroups = new List<ProcessorGroupRecord>();
    }
}

public static class CpuTopologyProbe
{
    private const int RelationProcessorCore = 0;
    private const int RelationNumaNode = 1;
    private const int RelationCache = 2;
    private const int RelationProcessorPackage = 3;
    private const int RelationProcessorDie = 5;
    private const int RelationAll = 0xffff;

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool GetLogicalProcessorInformationEx(
        int relationshipType,
        IntPtr buffer,
        ref int returnedLength);

    [DllImport("kernel32.dll")]
    private static extern ushort GetActiveProcessorGroupCount();

    [DllImport("kernel32.dll")]
    private static extern uint GetActiveProcessorCount(ushort groupNumber);

    public static TopologySnapshot Query()
    {
        int byteCount = 0;
        GetLogicalProcessorInformationEx(RelationAll, IntPtr.Zero, ref byteCount);
        int error = Marshal.GetLastWin32Error();
        if (byteCount <= 0)
        {
            throw new InvalidOperationException("GetLogicalProcessorInformationEx did not return a buffer size. Win32 error: " + error);
        }

        IntPtr buffer = Marshal.AllocHGlobal(byteCount);
        try
        {
            if (!GetLogicalProcessorInformationEx(RelationAll, buffer, ref byteCount))
            {
                throw new InvalidOperationException("GetLogicalProcessorInformationEx failed. Win32 error: " + Marshal.GetLastWin32Error());
            }

            var snapshot = new TopologySnapshot();
            int offset = 0;
            while (offset < byteCount)
            {
                IntPtr record = IntPtr.Add(buffer, offset);
                int relationship = Marshal.ReadInt32(record, 0);
                int recordSize = Marshal.ReadInt32(record, 4);
                if (recordSize < 8 || offset + recordSize > byteCount)
                {
                    throw new InvalidOperationException("Invalid SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX record size.");
                }

                switch (relationship)
                {
                    case RelationProcessorCore:
                        snapshot.Cores.Add(ReadProcessorRecord(record));
                        break;
                    case RelationProcessorPackage:
                        snapshot.Packages.Add(ReadProcessorRecord(record));
                        break;
                    case RelationProcessorDie:
                        snapshot.Dies.Add(ReadProcessorRecord(record));
                        break;
                    case RelationCache:
                        snapshot.Caches.Add(ReadCacheRecord(record));
                        break;
                    case RelationNumaNode:
                        snapshot.NumaNodes.Add(ReadNumaRecord(record));
                        break;
                }

                offset += recordSize;
            }

            ushort groupCount = GetActiveProcessorGroupCount();
            for (ushort group = 0; group < groupCount; ++group)
            {
                snapshot.ProcessorGroups.Add(new ProcessorGroupRecord
                {
                    GroupNumber = group,
                    ActiveProcessorCount = GetActiveProcessorCount(group)
                });
            }

            return snapshot;
        }
        finally
        {
            Marshal.FreeHGlobal(buffer);
        }
    }

    public static List<string> ExpandMask(ushort group, ulong mask)
    {
        var result = new List<string>();
        for (int bit = 0; bit < 64; ++bit)
        {
            if ((mask & (1UL << bit)) != 0)
            {
                result.Add("G" + group + ":LP" + bit);
            }
        }
        return result;
    }

    private static ProcessorRecord ReadProcessorRecord(IntPtr record)
    {
        const int dataOffset = 8;
        const int groupCountOffset = dataOffset + 22;
        const int groupMaskOffset = dataOffset + 24;
        int groupAffinitySize = IntPtr.Size + 8;

        var result = new ProcessorRecord
        {
            EfficiencyClass = Marshal.ReadByte(record, dataOffset + 1)
        };

        ushort groupCount = ReadUInt16(record, groupCountOffset);
        for (int index = 0; index < groupCount; ++index)
        {
            result.Masks.Add(ReadGroupMask(IntPtr.Add(record, groupMaskOffset + index * groupAffinitySize)));
        }

        return result;
    }

    private static CacheRecord ReadCacheRecord(IntPtr record)
    {
        const int dataOffset = 8;
        const int groupMaskOffset = dataOffset + 32;
        return new CacheRecord
        {
            Level = Marshal.ReadByte(record, dataOffset),
            Associativity = Marshal.ReadByte(record, dataOffset + 1),
            LineSizeBytes = ReadUInt16(record, dataOffset + 2),
            SizeBytes = ReadUInt32(record, dataOffset + 4),
            Kind = Marshal.ReadInt32(record, dataOffset + 8),
            Mask = ReadGroupMask(IntPtr.Add(record, groupMaskOffset))
        };
    }

    private static NumaRecord ReadNumaRecord(IntPtr record)
    {
        const int dataOffset = 8;
        const int groupMaskOffset = dataOffset + 24;
        return new NumaRecord
        {
            NodeNumber = ReadUInt32(record, dataOffset),
            Mask = ReadGroupMask(IntPtr.Add(record, groupMaskOffset))
        };
    }

    private static GroupMaskRecord ReadGroupMask(IntPtr pointer)
    {
        return new GroupMaskRecord
        {
            Mask = ReadAffinity(pointer),
            Group = ReadUInt16(pointer, IntPtr.Size)
        };
    }

    private static ushort ReadUInt16(IntPtr pointer, int offset)
    {
        return unchecked((ushort)Marshal.ReadInt16(pointer, offset));
    }

    private static uint ReadUInt32(IntPtr pointer, int offset)
    {
        return unchecked((uint)Marshal.ReadInt32(pointer, offset));
    }

    private static ulong ReadAffinity(IntPtr pointer)
    {
        if (IntPtr.Size == 8)
        {
            return unchecked((ulong)Marshal.ReadInt64(pointer));
        }

        return unchecked((uint)Marshal.ReadInt32(pointer));
    }
}
'@

Add-Type -TypeDefinition $typeDefinition -Language CSharp

function ConvertTo-MermaidLabel {
    param([string]$Value)

    return $Value.Replace('&', 'and').Replace('"', "'").Replace('[', '(').Replace(']', ')').Replace("`r", ' ').Replace("`n", ' ')
}

function Get-LogicalProcessorLabels {
    param([GroupMaskRecord]$GroupMask)

    return [CpuTopologyProbe]::ExpandMask($GroupMask.Group, $GroupMask.Mask)
}

function Format-ByteSize {
    param([uint64]$Value)

    if ($Value -ge 1GB) {
        return ('{0:N2} GiB' -f ($Value / 1GB))
    }
    if ($Value -ge 1MB) {
        return ('{0:N2} MiB' -f ($Value / 1MB))
    }
    if ($Value -ge 1KB) {
        return ('{0:N2} KiB' -f ($Value / 1KB))
    }
    return "$Value B"
}

function Get-CacheKindName {
    param([int]$Kind)

    switch ($Kind) {
        0 { return 'Unified' }
        1 { return 'Instruction' }
        2 { return 'Data' }
        3 { return 'Trace' }
        default { return "Unknown($Kind)" }
    }
}

$snapshot = [CpuTopologyProbe]::Query()
$processor = Get-CimInstance -ClassName Win32_Processor | Select-Object -First 1
$processorName = $processor.Name.Trim()
$computerSystem = Get-CimInstance -ClassName Win32_ComputerSystem
$generatedAt = Get-Date -Format 'yyyy-MM-dd HH:mm:ss K'

$logicalToCore = @{}
$coreRows = @()
for ($coreIndex = 0; $coreIndex -lt $snapshot.Cores.Count; ++$coreIndex) {
    $core = $snapshot.Cores[$coreIndex]
    $logicalProcessors = @()
    foreach ($mask in $core.Masks) {
        $logicalProcessors += Get-LogicalProcessorLabels -GroupMask $mask
    }
    foreach ($logicalProcessor in $logicalProcessors) {
        $logicalToCore[$logicalProcessor] = $coreIndex
    }

    $coreRows += [PSCustomObject]@{
        CoreId = $coreIndex
        EfficiencyClass = $core.EfficiencyClass
        LogicalProcessors = $logicalProcessors
    }
}

$numaByLogicalProcessor = @{}
foreach ($numa in $snapshot.NumaNodes) {
    foreach ($logicalProcessor in (Get-LogicalProcessorLabels -GroupMask $numa.Mask)) {
        $numaByLogicalProcessor[$logicalProcessor] = $numa.NodeNumber
    }
}

$mermaidCoreLines = [System.Collections.Generic.List[string]]::new()
$mermaidCoreLines.Add('flowchart TB')
$cpuLabel = ConvertTo-MermaidLabel -Value $processorName
$mermaidCoreLines.Add(('  CPU["{0}"]' -f $cpuLabel))
foreach ($group in $snapshot.ProcessorGroups) {
    $groupNode = "Group$($group.GroupNumber)"
    $mermaidCoreLines.Add(('  {0}["OS Scheduling Group {1}<br/>{2} active logical processors"]' -f $groupNode, $group.GroupNumber, $group.ActiveProcessorCount))
    $mermaidCoreLines.Add("  CPU --> $groupNode")
}
foreach ($coreRow in $coreRows) {
    $groupNumbers = @($coreRow.LogicalProcessors | ForEach-Object { ($_ -split ':')[0].Substring(1) } | Sort-Object -Unique)
    foreach ($groupNumber in $groupNumbers) {
        $coreNode = "Core$($coreRow.CoreId)"
        $logicalLabel = [string]::Join('<br/>', $coreRow.LogicalProcessors)
        $mermaidCoreLines.Add(('  {0}["Core {1}<br/>Efficiency {2}<br/>{3}"]' -f $coreNode, $coreRow.CoreId, $coreRow.EfficiencyClass, $logicalLabel))
        $mermaidCoreLines.Add("  Group$groupNumber --> $coreNode")
        break
    }
}

$cacheRows = @()
$mermaidCacheLines = [System.Collections.Generic.List[string]]::new()
$mermaidCacheLines.Add('flowchart LR')
$sharedCacheIndex = 0
foreach ($cache in $snapshot.Caches) {
    $logicalProcessors = @(Get-LogicalProcessorLabels -GroupMask $cache.Mask)
    $coreIds = @($logicalProcessors | ForEach-Object { $logicalToCore[$_] } | Where-Object { $null -ne $_ } | Sort-Object -Unique)
    $cacheRows += [PSCustomObject]@{
        CacheId = $cacheRows.Count
        Level = $cache.Level
        Kind = Get-CacheKindName -Kind $cache.Kind
        Size = Format-ByteSize -Value $cache.SizeBytes
        LineSize = $cache.LineSizeBytes
        LogicalProcessors = $logicalProcessors
        CoreIds = $coreIds
    }

    if ($coreIds.Count -gt 1) {
        $cacheNode = "SharedCache$sharedCacheIndex"
        $cacheLabel = "L$($cache.Level) $(Get-CacheKindName -Kind $cache.Kind)<br/>$(Format-ByteSize -Value $cache.SizeBytes)"
        $mermaidCacheLines.Add(('  {0}["{1}"]' -f $cacheNode, $cacheLabel))
        foreach ($coreId in $coreIds) {
            $mermaidCacheLines.Add("  $cacheNode -. shared by .-> Core$coreId")
        }
        $sharedCacheIndex++
    }
}
if ($sharedCacheIndex -eq 0) {
    $mermaidCacheLines.Add('  NoSharedCache["No cache shared by more than one reported core"]')
}

$coreTableRows = $coreRows | ForEach-Object {
    "| $($_.CoreId) | $($_.EfficiencyClass) | $([string]::Join(', ', $_.LogicalProcessors)) |"
}
$cacheTableRows = $cacheRows | ForEach-Object {
    "| $($_.CacheId) | L$($_.Level) | $($_.Kind) | $($_.Size) | $($_.LineSize) | $([string]::Join(', ', $_.CoreIds)) |"
}
$numaTableRows = $snapshot.NumaNodes | ForEach-Object {
    $logicalProcessors = Get-LogicalProcessorLabels -GroupMask $_.Mask
    "| $($_.NodeNumber) | $([string]::Join(', ', $logicalProcessors)) |"
}
$groupTableRows = $snapshot.ProcessorGroups | ForEach-Object {
    "| $($_.GroupNumber) | $($_.ActiveProcessorCount) |"
}

$markdownTemplate = @'
---
kind: fieldnote
tags:
  - hardware/cpu
  - cpu-topology
  - generated
status: generated
source:
  - Local Windows topology APIs
created: "__DATE__"
updated: "__DATE__"
---

# __PROCESSOR_NAME__

> [!info]
> Generated locally at __GENERATED_AT__ by `D:\MMDLab\Tools\Generate-CpuTopology.ps1`.
> The note reads Windows core, cache, NUMA, and processor-group relationships. It does not infer vendor-specific clusters such as AMD core complexes.

## Processor Summary

| Field | Value |
| --- | --- |
| Processor | __PROCESSOR_NAME__ |
| Reported cores | __CORE_COUNT__ |
| Reported logical processors | __LOGICAL_PROCESSOR_COUNT__ |
| Processor groups | __PROCESSOR_GROUP_COUNT__ |
| NUMA nodes | __NUMA_NODE_COUNT__ |
| Installed physical memory | __MEMORY_SIZE__ |

## Core and Processor-Group Topology

```mermaid
__CORE_MERMAID__
```

| Core | Efficiency Class | Logical Processors |
| --- | --- | --- |
__CORE_ROWS__

## Shared Cache Topology

Only caches shared by more than one reported core are drawn to keep the graph readable.

```mermaid
__CACHE_MERMAID__
```

| Cache | Level | Kind | Size | Line Size | Sharing Cores |
| --- | --- | --- | --- | --- | --- |
__CACHE_ROWS__

## NUMA Nodes

| NUMA Node | Logical Processors |
| --- | --- |
__NUMA_ROWS__

## Operating-System Scheduling Groups

| Group | Active Logical Processors |
| --- | --- |
__GROUP_ROWS__

'@

$markdown = $markdownTemplate
$markdown = $markdown.Replace('__DATE__', (Get-Date -Format 'yyyy-MM-dd'))
$markdown = $markdown.Replace('__GENERATED_AT__', $generatedAt)
$markdown = $markdown.Replace('__PROCESSOR_NAME__', $processorName)
$markdown = $markdown.Replace('__CORE_COUNT__', [string]$snapshot.Cores.Count)
$markdown = $markdown.Replace('__LOGICAL_PROCESSOR_COUNT__', [string]$coreRows.LogicalProcessors.Count)
$markdown = $markdown.Replace('__PROCESSOR_GROUP_COUNT__', [string]$snapshot.ProcessorGroups.Count)
$markdown = $markdown.Replace('__NUMA_NODE_COUNT__', [string]$snapshot.NumaNodes.Count)
$markdown = $markdown.Replace('__MEMORY_SIZE__', (Format-ByteSize -Value $computerSystem.TotalPhysicalMemory))
$markdown = $markdown.Replace('__CORE_MERMAID__', ($mermaidCoreLines -join "`n"))
$markdown = $markdown.Replace('__CORE_ROWS__', ($coreTableRows -join "`n"))
$markdown = $markdown.Replace('__CACHE_MERMAID__', ($mermaidCacheLines -join "`n"))
$markdown = $markdown.Replace('__CACHE_ROWS__', ($cacheTableRows -join "`n"))
$markdown = $markdown.Replace('__NUMA_ROWS__', ($numaTableRows -join "`n"))
$markdown = $markdown.Replace('__GROUP_ROWS__', ($groupTableRows -join "`n"))

$outputDirectory = Split-Path -Parent $OutputPath
if (-not (Test-Path -LiteralPath $outputDirectory)) {
    New-Item -ItemType Directory -Path $outputDirectory -Force | Out-Null
}

Set-Content -LiteralPath $OutputPath -Value $markdown -Encoding utf8
Write-Output "Generated CPU topology note: $OutputPath"
