---
kind: technique
tags:
  - runtime/threading
  - frame-model
  - mermaid
  - obsidian
status: draft
source:
  - "D:\\MMDLab\\Source\\Runtime\\Core\\Thread.h"
  - "D:\\MMDLab\\Source\\Runtime\\Core\\Channel.h"
  - "D:\\MMDLab\\Source\\Runtime\\Core\\FrameResourcePool.h"
  - "D:\\MMDLab\\Source\\Runtime\\Core\\RenderThread.h"
  - "D:\\MMDLab\\Source\\Runtime\\Core\\RhiThread.h"
created: 2026-09-25
updated: 2026-09-25
---

# Thread Model

> [!info]
> This graph reflects the actual implementation: two channels plus a `FrameResourcePool` return
> path (no third queue). Mermaid output is an inspection aid, not a scheduler design verdict.

## Ownership and Data-Flow Loop

```mermaid
flowchart LR
    Pool["FrameResourcePool<br/>3 × FrameResource"]
    GT["GameThread<br/>(main thread)"]
    RT["RenderThread<br/>Runnable on Thread"]
    RHI["RhiThread<br/>Runnable on Thread"]
    C1["gameToRender<br/>Channel&lt;FrameIndex,3&gt;"]
    C2["renderToRhi<br/>Channel&lt;FrameIndex,3&gt;"]

    Pool -->|"Acquire()<br/>block while none free"| GT
    GT -->|"write frameId + gameToRender<br/>Push(index)"| C1
    C1 -->|"Pop(index)<br/>block while empty"| RT
    RT -->|"write renderToRhi<br/>Push(index)"| C2
    C2 -->|"Pop(index)<br/>block while empty"| RHI
    RHI -->|"set gpuFenceValue<br/>Release(index)"| Pool
```

A `FrameIndex` travels the loop once per frame. The channel carries only the index; the frame's
data (`RenderFrame gameToRender`, `RenderWorkBatch renderToRhi`, `frameId`, `gpuFenceValue`) lives in the `FrameResource`
that the index points into.

## Who Owns a FrameResource at Each Moment

```mermaid
flowchart LR
    Free["Free<br/>(in pool)"] -->|"Acquire"| Game["Owned by GameThread"]
    Game -->|"Push to gameToRender"| In1["In gameToRender channel"]
    In1 -->|"Pop"| Render["Owned by RenderThread"]
    Render -->|"Push to renderToRhi"| In2["In renderToRhi channel"]
    In2 -->|"Pop"| Rhi["Owned by RhiThread"]
    Rhi -->|"Release"| Free
```

Exactly one owner exists at any moment: either the pool (free) or one of the three stages. This
single-owner invariant is what makes each stage's write to the `FrameResource` safe without locks.

## Scheduling Facts

| Thread | Runs on | Idle behavior | Stop path |
| --- | --- | --- | --- |
| GameThread | the process main thread | blocks in `FrameResourcePool::Acquire()` | stops producing, then requests stops downstream |
| RenderThread | a `Thread` running the `RenderThread` runnable | blocks in `gameToRender.Pop()` | `Stop()` calls `gameToRender.Close()` |
| RhiThread | a `Thread` running the `RhiThread` runnable | blocks in `renderToRhi.Pop()` | `Stop()` calls `renderToRhi.Close()` |

- Scheduling is **fixed threads + blocking channels**, not a task graph. No thread polls or spins.
- Back-pressure comes from the pool: `GameThread` produces at most three frames ahead of the GPU.
- Shutdown is explicit and upstream-first: stop `RenderThread`, then `RhiThread`. Each `Close()`
  wakes the blocked `Pop()`, which drains remaining frames, then returns `std::nullopt` and exits.
