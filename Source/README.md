# Source Architecture

The first MMDLab milestone is intentionally small: convert a MikuMikuDance (MMD) model into a compact native binary file and render static triangles through DirectX 12. The source tree borrows Unreal Engine's useful separation between application startup, rendering policy, and graphics execution without reproducing its module count or its render hardware interface (RHI) abstraction.

## Terminology

- DirectX 12 (DX12) is the only graphics backend in this milestone.
- DirectX Graphics Infrastructure (DXGI) and Direct3D 12 (D3D12) are the native graphics interfaces used by `Runtime/DX12`.
- A graphics processing unit (GPU) executes submitted graphics work; a central processing unit (CPU) owns host-side runtime state.
- Single-producer, single-consumer (SPSC) describes a queue with exactly one writing thread and one reading thread.
- High-Level Shader Language (HLSL) is used for shader source files.
- A pipeline state object (PSO) combines fixed graphics pipeline state and compiled shaders for one draw path.
- The Windows application programming interface (Win32) owns the application window and message loop.
- An application programming interface (API) is a boundary exposed by one module for another module to use.
- An application binary interface (ABI) is the binary layout and calling convention observed by compiled code.
- Input/output (I/O) covers file and device operations; asynchronous I/O completion is distinct from CPU decoding or validation work.
- An operating system (OS) provides platform scheduling and asynchronous I/O services.
- A personal computer (PC) is the current deployment platform. PlayStation 5 (PS5) is a future topology reference only, not a DX12 deployment target.
- Simultaneous multithreading (SMT) exposes multiple logical processors that share one physical core's execution resources.
- A central processing unit (CPU) cluster is an optional Engine-level grouping of cores with a platform-reported or platform-profiled affinity relationship. A core complex (CCX) is one Zen-specific source for such a cluster.
- Advanced RISC Machines (ARM) systems commonly expose cluster-oriented CPU topology.
- Cache topology describes actual cache resources and their sharing relationships. It is separate from CPU clusters.
- A non-uniform memory access (NUMA) node describes a memory-affinity relationship. An operating-system scheduling group describes a platform scheduling constraint.
- PMX and VMD are canonical MikuMikuDance file-format identifiers; this project does not invent expansions for them.

## Initial Layout

```text
Source/
├─ App/
│  └─ MmdViewer/                # Win32 entry point and GameThread ownership
├─ Tools/
│  └─ MmdCooker/                # Offline PMX static-mesh importer and binary cooker
├─ Runtime/
│  ├─ Core/                     # Types, errors, frame slots, queues, threading, diagnostics
│  ├─ Asset/                    # .mmdl reading, CPU asset metadata, startup upload descriptions
│  ├─ Render/                   # RenderFrame building and RenderThread work compilation
│  └─ DX12/                     # DXGI/D3D12 resources, RhiThread, fences, presentation
├─ Shaders/                     # HLSL source files
└─ Tests/                       # Parser, format, queue, and runtime regression tests
```

`Runtime/Core`, `Runtime/Asset`, `Runtime/Render`, and `Runtime/DX12` are source boundaries, not separate build targets yet. Start with one `MmdRuntime` static library, one `MmdViewer` executable, and one `MmdCooker` executable. Split libraries only when build time, ownership, or reuse requires it.

There is no generic RHI module in the first milestone. `Runtime/DX12` is the only backend. Introduce an RHI abstraction only after a second backend or a real test seam justifies it.

## Responsibility Boundaries

| Module | Owns | Must Not Own |
| --- | --- | --- |
| `App/MmdViewer` | window, input, GameThread, frame pacing policy | PMX parsing, D3D12 resource lifetime |
| `Tools/MmdCooker` | PMX validation, convention conversion, `.mmdl` writing | window, render loop, D3D12 calls |
| `Runtime/Core` | frame slots, thread roles, queues, common types, diagnostics | PMX details or D3D12 calls |
| `Runtime/Asset` | `.mmdl` reading, CPU metadata, asset handles, upload descriptions | command-list submission or GPU resource lifetime |
| `Runtime/Render` | sealed render data and API-independent work compilation | direct D3D12 object ownership |
| `Runtime/DX12` | device, queues, fences, descriptors, GPU resources, command lists, present | PMX/VMD knowledge or scene ownership |

## Frame Ownership and Back-Pressure

The initial runtime uses exactly three `FrameSlot` objects. A slot owns every transient allocation associated with one presented frame. The queues transfer a `FrameLease`, never a naked `std::span` or pointer to temporary storage.

```text
Free
  -> GameWriting
  -> GameToRenderQueue
  -> RenderBuilding
  -> RenderToRhiQueue
  -> GpuInFlight
  -> Retiring
  -> Free
```

```cpp
struct FrameLease {
    FrameSlotId slotId;
    FrameId frameId;
};

struct FrameSlot {
    FrameId frameId;
    FrameArena arena;
    RenderFrame renderFrame;
    RenderWorkBatch renderWork;
    uint64_t submittedFenceValue;
};
```

- `GameThread` acquires only a `Free` slot. If no slot is free, it waits for one returned by `RhiThread`; it does not allocate another slot, overwrite a queued slot, or silently drop a frame.
- `RenderThread` receives a lease, reads the sealed `RenderFrame`, writes `RenderWorkBatch` into the same slot, then forwards the lease.
- `RhiThread` receives a lease, records and submits D3D12 work, stores the submitted fence value, and owns all GPU-facing retirement.
- After the fence completes, `RhiThread` retires descriptors and transient GPU allocations, clears the slot, and returns it to `GameThread`.
- The first milestone uses three slots for deterministic bounded latency. Frame-slot count becomes configurable only after profiling.

This is the initial Runtime Data Graph: explicit data ownership, explicit version (`FrameId`), and explicit back-pressure. It is deliberately not a global event bus or a generic scheduler.

## Evolution Rule

The Runtime Data Graph is the architectural model. SPSC queues, frame slots, and fences are transport and lifetime mechanisms used to realize one edge of that model. Do not evolve this design into `DataBus::Publish` and `DataBus::Subscribe`.

`FrameSlot` owns frame-local, transient data only. Persistent authoritative state remains owned by the module that defines it:

```text
GameThread-owned persistent state
        |
        | project only required immutable inputs
        v
FrameSlot(N)
  - GameFrameInput
  - AnimationInput            # Later
  - PoseBuffer                # Later
  - RenderFrame
  - RenderWorkBatch
        |
        v
RhiThread and GPU retirement
```

For example, a future Animation System consumes `AnimationInput` and produces `PoseBuffer`; it does not own or tick a character object. The authoritative character state still belongs to the GameThread. This keeps the frame graph immutable and prevents `FrameSlot` from becoming another global world container.

Add general `Read / Write / Version / Phase` declarations only when a real branch appears, such as Animation producing a pose that is independently consumed by rendering and another system. Until then, the fixed lease transfer is the scheduler.

## Thread Model

Thread names are logical roles. The bootstrap path may temporarily run roles on fewer operating-system threads, but queue ownership and frame-slot transitions must remain identical.

```text
GameThread                         RenderThread                      RhiThread
    |                                    |                                |
    |-- GameToRenderFrameQueue --------->|                                |
    |                                    |-- RenderToRhiWorkQueue -------->|
    |                                    |                                |-- D3D12 driver calls
    |<-- RhiToGameReturnedSlotQueue --------------------------------------|
```

```cpp
using GameToRenderFrameQueue = SpscQueue<FrameLease>;
using RenderToRhiWorkQueue = SpscQueue<FrameLease>;
using RhiToGameReturnedSlotQueue = SpscQueue<FrameSlotId>;

GameToRenderFrameQueue::Producer gameToRenderFrame;
GameToRenderFrameQueue::Consumer renderFromGameFrame;

RenderToRhiWorkQueue::Producer renderToRhiWork;
RenderToRhiWorkQueue::Consumer rhiFromRenderWork;

RhiToGameReturnedSlotQueue::Producer rhiToGameReturnedSlot;
RhiToGameReturnedSlotQueue::Consumer gameFromRhiReturnedSlot;
```

The queue name answers "from whom, to whom, and what kind of data." The endpoint name answers "which role owns this handle." `RhiThread` is the only owner of swap-chain presentation, D3D12 queue submission, fence polling, descriptor retirement, and native GPU resource destruction. This isolates driver-facing synchronization from `RenderThread`.

## Execution Resources and CPU Budget

The three ownership contexts are not the complete execution model, and they do not imply three permanently reserved physical CPU cores. They exist because they own serial state and lifetime boundaries:

```text
Ownership Contexts                 Threads Groups and I/O
------------------                 ---------------------
GameThread                         Compute Threads Group (later)
RenderThread                         - animation evaluation
RhiThread                            - inverse kinematics
                                     - physics work
                                     - asset decoding and validation

                                   I/O Threads Group (later, optional)
                                     - blocking or fallback file work
                                     - I/O completion dispatch

                                   OS asynchronous I/O service (later)
                                     - asynchronous reads
                                     - completion notification
                                     - decode work sent to Compute Threads Group
```

An Animation System must not own an `AnimationThread`, and a Physics System must not own a `PhysicsThread`. They eventually submit independent CPU-bound jobs to `ComputeThreadsGroup` after their input contracts are ready. The Runtime Data Graph determines *when* work is eligible; the execution class and CPU budget determine *where* it runs.

The static-mesh milestone does not create either threads group. It uses synchronous startup loading and the three ownership contexts only. Add `ComputeThreadsGroup` only when animation, decoding, or another measured CPU-bound workload has enough parallel work to justify it. Add `IoThreadsGroup` only if OS asynchronous I/O and completion dispatch cannot meet the required behavior by themselves.

### Future Execution Classes

When real graph branches exist, systems may declare one of these execution classes in addition to their data contracts:

| Execution Class | Runs On | Intended Use |
| --- | --- | --- |
| `GameOwner` | `GameThread` | mutable authoritative state and lifecycle transitions |
| `RenderOwner` | `RenderThread` | render work compilation and render-frame ownership |
| `RhiOwner` | `RhiThread` | native DX12 resource lifetime and command submission |
| `Compute` | `ComputeThreadsGroup` | animation, inverse kinematics, physics, decoding, validation |
| `AsyncIo` | OS asynchronous I/O plus optional `IoThreadsGroup` completion handling | file/device reads; not CPU decode work |

This table is a design boundary, not a request to implement a generic scheduler now.

### CPU Budget Policy

Future worker count must come from a platform-specific CPU budget, not from a direct expression such as `logicalProcessorCount - 3`.

- The runtime retains available physical-core, logical-processor, simultaneous-multithreading sibling, performance-class, and cache-locality-domain information when the platform exposes it.
- Dedicated ownership contexts are scheduling constraints, not automatic core reservations or affinity assignments.
- Compute worker count is configurable, capped by platform policy, and tuned through profiling.
- I/O waiting does not consume a permanent compute worker. CPU-heavy decompression, parsing, validation, and asset conversion are ordinary compute jobs after I/O completion.
- The platform layer provides the game-visible execution budget, including any system-reserved capacity. Hardware topology never directly determines worker count.
- A console-class topology, including PS5, can be used as a future budget reference, but no PS5 core, SMT, CCX, or system-reservation number is hardcoded. The current DirectX 12 application remains a Windows PC project.
- Cache locality may influence future job affinity only after profiling. Cache sharing, clusters, NUMA nodes, and operating-system scheduling groups remain separate topology relations.

Do not add processor affinity, heterogeneous-core policy, platform-specific console code, or automatic worker-count heuristics before a compute pool exists and profiling demonstrates a need.

### Future CPU Topology Model

CPU topology is a relation graph, not a single `LogicalProcessor -> Core -> Cluster` tree. The portable baseline is:

```cpp
struct CpuTopology {
    std::vector<LogicalProcessor> logicalProcessors;
    std::vector<CpuCore> cores;
    std::vector<CpuPackage> packages;
    std::vector<NumaNode> numaNodes;
    std::vector<OsSchedulingGroup> schedulingGroups;
    std::vector<CpuCache> caches;
    std::vector<CpuCluster> clusters; // Optional platform-provided grouping.
};

struct CpuCore {
    CoreId id;
    std::vector<LogicalProcessorId> logicalProcessors;
    std::optional<CpuClusterId> clusterId;
    std::optional<NumaNodeId> numaNodeId;
    std::optional<OsSchedulingGroupId> schedulingGroupId;
    PerformanceClass performanceClass;
};

struct CpuCache {
    CacheId id;
    CacheLevel level;
    CacheKind kind;
    uint64_t sizeBytes;
    uint32_t lineSizeBytes;
    std::vector<LogicalProcessorId> sharedByLogicalProcessors;
};

struct CpuCluster {
    CpuClusterId id;
    CpuClusterKind kind;
    std::vector<CoreId> cores;
};
```

`CpuCluster` is optional because not every platform exposes a meaningful portable cluster boundary. It may represent a Zen core complex, an ARM cluster, a performance-core group, an efficiency-core group, or another platform-defined affinity group. It must carry a kind and source, and it must never be guessed solely from cache size.

`CpuCache` is independent of `CpuCluster`. Cache sharing is represented by logical-processor membership because operating systems commonly report cache affinity at that granularity; the set of sharing cores is derived from those members. A cache may be private, shared by simultaneous-multithreading siblings, shared by several cores, or described differently by each platform.

The scheduler must not impose one universal locality ranking such as "same cluster is always better." Simultaneous-multithreading siblings may have excellent data locality but compete for execution capacity; shared cache, cluster membership, NUMA placement, and operating-system scheduling groups are separate signals. Future affinity policy consumes these signals only after workload-specific profiling.

## Back-Pressure Diagnosis

`GameThread` waiting for a `Free` slot does not necessarily mean that GameThread work is slow or incorrect. It means that the GameThread has produced the maximum number of frames permitted to be in flight and the oldest slot has not yet become safe to reuse.

```text
GameThread produces frames faster than the complete downstream pipeline consumes them.

Game -> Render -> RHI -> GPU -> Present -> returned Free slot
```

With three frame slots, GameThread may work ahead on frames `N`, `N + 1`, and `N + 2`. It waits only if all three slots are queued, being built, or remain GPU-in-flight. This is bounded back-pressure: it prevents unbounded memory growth and excessive input-to-presentation latency.

Use queue state and slot-state duration to identify the limiting stage:

| Observation | Likely Limiting Stage |
| --- | --- |
| `GameToRenderFrameQueue` remains full | `RenderThread` is behind frame production |
| `RenderToRhiWorkQueue` remains full | `RhiThread`, driver-facing work, or presentation is behind |
| Both queues are mostly empty but no slot is free | GPU execution, fence completion, or presentation timing is behind |
| A slot remains in `GameWriting` or `RenderBuilding` for too long | GameThread or RenderThread CPU work is behind |

For a static-triangle prototype, RenderThread and RhiThread CPU work should normally be cheap. The common reason for all slots remaining in flight is GPU fence latency or presentation pacing. That is expected behavior under vertical synchronization and is not automatically a bug.

Record the time spent in every slot state: `GameWriting`, `GameToRenderQueue`, `RenderBuilding`, `RenderToRhiQueue`, `GpuInFlight`, and `Retiring`. GameThread must wait on a free-slot event or condition, never spin while polling for one.

## Initial Data Contracts

```text
GameThread
  Write RenderFrame(N) into FrameSlot(N)
        |
        v
RenderThread
  Read  RenderFrame(N)
  Write RenderWorkBatch(N) into FrameSlot(N)
        |
        v
RhiThread
  Read  RenderWorkBatch(N)
  Write submitted fence value
  Return FrameSlot(N) only after fence completion
```

| Producer | Contract | Consumer | Access Rule |
| --- | --- | --- | --- |
| `GameThread` | `RenderFrame` | `RenderThread` | one writer, sealed immutable read |
| `RenderThread` | `RenderWorkBatch` | `RhiThread` | one writer, ordered lease transfer |
| `RhiThread` | returned `FrameSlotId` | `GameThread` | one writer, only after fence retirement |

`RenderThread` never fetches mutable state from application or asset modules. `RenderFrame` contains only dense render-facing data and stable asset handles. The fixed contracts become general `Read / Write / Version / Phase` declarations only after Animation or Physics creates a real branch in the graph.

## Minimal Native Asset Pipeline

```text
PMX static-mesh source file
        |
        v
MmdCooker
  - parse and validate
  - normalize conventions
  - pack runtime data
        |
        v
.mmdl native binary
        |
        v
Runtime/Asset synchronous startup load
        |
        v
RhiThread startup upload
        |
        v
ready mesh asset handle
        |
        v
RenderFrame -> RenderWorkBatch -> draw
```

`MmdCooker` version zero accepts PMX static geometry only. It explicitly rejects VMD, skeleton, skin weights, morphs, textures, and physics data until those runtime features have a tested consumer.

`.mmdl` is the provisional native MMDLab asset extension. It is an implementation detail, not a public interchange format.

### Format Rules

- The file is little-endian and starts with a fixed-width versioned header, total file size, chunk-table offset, and chunk count.
- Each chunk descriptor contains a type, chunk version, file-relative offset, byte size, and required alignment.
- The on-disk format uses fixed-width integer and floating-point fields; it contains no native pointers, raw C++ containers, compiler-dependent enums, or unversioned raw C++ structs.
- CPU metadata and GPU payloads are separate chunks. The loader can inspect metadata without touching large vertex/index payloads.
- GPU payload chunks use final packed vertex and index layouts. Their exact field order, scalar type, stride, and input-layout interpretation are part of the chunk version.
- Chunks are independently range-checked before use. The loader rejects invalid offsets, sizes, overlap, alignment, unknown required chunks, and unsupported chunk versions.
- Large immutable chunks may be memory mapped later; the initial loader may use ordinary file reads.
- The cooker owns conversion from PMX conventions to MMDLab conventions. Runtime code must not contain PMX-specific branches.

### First `.mmdl` Chunks

```text
FileHeader
ChunkTable
StringTable                 # Optional names for diagnostics
MeshMetadata                # Bounds, vertex layout, material range metadata
GpuVertexBuffer             # Packed position, normal, UV, vertex color if present
GpuIndexBuffer              # Final index type and draw ranges
MaterialTable               # Minimal material constants
```

## Startup Upload Path

The first milestone does not implement streaming. Before frame production begins, `MmdViewer` synchronously loads one `.mmdl` file through `Runtime/Asset`, creates an immutable `StartupUploadRequest`, and hands it to `RhiThread` during startup. `RhiThread` creates the GPU buffers, submits the copy work, waits for its startup fence, and publishes a ready mesh asset handle.

This one-time bootstrap path is intentionally synchronous. Introduce `AssetToRhiUploadQueue` only when asynchronous loading or runtime asset replacement becomes a measured requirement.

## Minimal Render Path

The first renderer supports only:

- one Win32 window and one DXGI swap chain;
- one D3D12 direct queue, one graphics command list per frame, and fence-based frame-slot reuse;
- one root signature, one graphics PSO, one vertex shader, and one pixel shader;
- static indexed triangle meshes, a fixed camera, depth testing, and a solid-color or vertex-color material;
- `RenderFrame` instances that reference ready native mesh asset handles.

It does not support a render graph, texture streaming, material graphs, shadows, skinning, animation, transparency, GPU-driven rendering, a second graphics backend, or runtime asset streaming.

## First Runtime Types

```cpp
struct RenderFrame {
    FrameId frameId;
    std::span<const RenderInstance> instances;
    CameraConstants camera;
};

struct RenderWorkBatch {
    FrameId frameId;
    std::span<const DrawPacket> draws;
};
```

`RenderFrame` and `RenderWorkBatch` are stored inside `FrameSlot::arena`; they are never copied through a queue. Their spans remain valid because the lease transfers ownership of the slot through all consumer stages, and the slot returns to `Free` only after D3D12 fence retirement.

## Implementation Order

1. Premake targets: `MmdRuntime`, `MmdViewer`, and `MmdCooker`.
2. `Runtime/Core`: errors, logging, `FrameId`, three-slot state machine, and tested SPSC queues.
3. `Runtime/DX12`: debug layer, device, direct queue, swap chain, fence, descriptor heap, and a hard-coded triangle.
4. `Runtime/Render`: `GameToRenderFrameQueue` and `RenderToRhiWorkQueue` with one triangle instance.
5. `Runtime/DX12`: `RhiToGameReturnedSlotQueue`, fence retirement, and bounded back-pressure.
6. `Runtime/Asset`: `.mmdl` header, chunk table, static mesh payload, and strict range validation.
7. `Tools/MmdCooker`: PMX static-mesh import into `.mmdl`.
8. Replace the hard-coded triangle with a synchronously uploaded cooked PMX static mesh.

Animation, skinning, VMD playback, a general scheduler, physics, and asynchronous asset loading begin only after this path is correct, profiled, and testable.
