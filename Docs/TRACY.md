# Tracy CPU Profiling

This document records the workflow for capturing a bounded CPU profile from MMDLab with
[Tracy](https://github.com/wolfpld/tracy), a real-time frame profiler. The client is vendored
under `Source/ThirdParty/tracy` and instrumented across the three-thread render pipeline, the
animation hot path, and the asset I/O workers.

The workflow has three stages: **build** (Tracy is enabled by default), **capture** (run the
viewer for a fixed frame count while a headless capture daemon records the trace), and
**inspect** (open the `.tracy` file in the Tracy profiler GUI).

## Enablement

`TRACY_ENABLE` is defined for every configuration by default, so both Debug and Release emit
trace events. To build without it (every marker compiles to a no-op), pass `--no-tracy`:

```text
GenerateProjects.bat --build Release           # Tracy on (default)
GenerateProjects.bat --build Release --no-tracy
```

The switch lives in `premake5.lua` as a `newoption` (`--no-tracy`) and is forwarded by
`GenerateProjects.bat`.

## Instrumentation

- **Threads** are named for the timeline: `GameThread`, `RenderThread`, `RhiThread`,
  `ComputeThreadsGroup-N`, and `IoThreadsGroup-N` (via `tracy::SetThreadName` in
  `Runtime/Core/Thread.cpp` and `Main.cpp`).
- **Frame boundary**: `FrameMark` once per game-loop iteration in `Main.cpp`.
- **Pipeline zones**: `RenderThread::Compile`, `RhiThread::Frame`, `Dx12Renderer::Render`.
- **Per-model frame zones**: `GameThread.EvaluateModels` spans the fork-join on the GameThread;
  each model's `EvaluateModelFrame` runs on the GameThread or a `ComputeThreadsGroup-N` worker and
  nests `VmdAnimator::SamplePose`, `VmdAnimator::SampleMorphWeights`, `ResolveMorphWeights`,
  `ApplyBoneMorphs`, `Animation.EvaluateBoneWorld` (with `Animation.IK`, `Animation.IK.Chain`,
  `Animation.FixedAxis`, `Animation.InheritRotation`, `Animation.InheritTranslation`),
  `Physics.Simulate`/`Physics.Step`, `Animation.EvaluateBoneWorldAfterPhysics`,
  `Animation.BuildSkinningPalette`, and `AccumulateVertexMorphDeltas`.
- **Asset I/O zones** (workers): `ParsePmxStaticMesh`, `DecodeImage`, `IoWorker::ParseModel`,
  `IoWorker::DecodeTexture`.

GPU-side zones (`TracyD3D12.hpp`) are intentionally not instrumented; this captures CPU time only.

## Prerequisites

- The **Tracy profiler GUI**, to inspect `.tracy` files. Build it from `D:\tracy\profiler` (a
  standalone CMake project), or use an official Tracy release binary:

```powershell
cmake -B D:\tracy\build-profiler -S D:\tracy\profiler -A x64 -DCMAKE_CXX_FLAGS="/utf-8"
cmake --build D:\tracy\build-profiler --config Release
# -> D:\tracy\build-profiler\Release\tracy-profiler.exe
```

- **`tracy-capture-daemon`**, a headless capture server, for unattended capture. Build it from
  `D:\tracy\capture` (a standalone CMake project):

```powershell
cmake -B D:\tracy\build-capture -S D:\tracy\capture
cmake --build D:\tracy\build-capture --config Release
# -> D:\tracy\build-capture\Release\tracy-capture-daemon.exe  (path varies by generator)
```

Both listen on port 8086 — the GUI for a live client, the daemon for the client's UDP broadcast,
which it answers by connecting back and writing one `.tracy` file per captured client.

On a non-UTF-8 system locale (e.g. Simplified Chinese, codepage 936), the profiler GUI needs
`/utf-8` in `CMAKE_CXX_FLAGS` as above or its UTF-8 sources fail to compile, and its `embed`
asset step needs `D:\tracy\build-profiler` on `PATH` during the build.

## Automated capture (~60 seconds)

The viewer accepts `--frames N`, which runs exactly N frames and then shuts down cleanly. The
log prints `Running 3600 frame(s), then exiting` to confirm the limit is active. A sixty-second
capture is the recommended default: the viewer bounds the run by frame count, so a minute is
3600 frames at a vsynced 60 fps, or proportionally more on an uncapped or high-refresh display.
Confirm the true duration from the log's elapsed milliseconds and scale the count up if a full
minute is required. A minute is long enough for the Tracy connection to establish and the trace
to stream, and for the animation hot path to reach steady state (models finish loading in a few
hundred milliseconds on the cooked path, so the motion zones dominate the trace).

1. Start the headless capture daemon first so it is listening before the client broadcasts:

```powershell
D:\tracy\build-capture\Release\tracy-capture-daemon.exe -o captures\
```

2. Launch the viewer bounded to 3600 frames, from the project directory that holds `Models/` and
   `Motions/` (the viewer scans its working directory, not the repository root):

```powershell
Set-Location <project directory>
<repository>\Build\Bin\Release\x64\MmdViewer.exe --frames 3600
```

   `--compute-threads N` sets the compute worker count (`0` evaluates every model on the
   GameThread, for a serial baseline), and `--io-threads N` sets the I/O worker count.

3. The daemon writes `captures\MmdViewerexe_<address>_<port>.tracy` — the daemon's name sanitizer
   drops the `.exe` dot, and `<address>` is the address the client announced (often the LAN IP
   such as `10.61.112.6`, not `127.0.0.1`). Stop the daemon with Ctrl+C, then open the file in
   the Tracy GUI.

The daemon runs until Ctrl+C, so an unattended run starts it in the background (a `;` one-liner
blocks on the daemon and never launches the viewer). For a single capture, `tracy-capture`
connects directly and exits when the client disconnects:

```powershell
$capture = Start-Process -PassThru -NoNewWindow D:\tracy\build-capture\Release\tracy-capture.exe `
    -ArgumentList '-a', '127.0.0.1', '-o', 'captures\viewer.tracy', '-f'
<repository>\Build\Bin\Release\x64\MmdViewer.exe --frames 3600
$capture.WaitForExit()
```

`D:\tracy\build-csvexport\Release\tracy-csvexport.exe` turns a trace into per-zone statistics
(`-u` lists every zone event with its thread, `-e` reports self time).

## Interactive capture

For an interactive session, run the Tracy GUI (it also listens on port 8086), launch the viewer
without a frame limit, and capture from the GUI:

```powershell
D:\tracy\build-profiler\Release\tracy-profiler.exe
Build\Bin\Release\x64\MmdViewer.exe
```

## Gotchas

- Start the daemon (or GUI) **before** the client; the client advertises itself at startup and
  a daemon started afterward can miss it.
- The `.tracy` output filename embeds the client's program name and data port, so it changes
  per run; use a wildcard (`captures\MmdViewerexe_*.tracy`) or pass `--filter-name MmdViewer` to
  the daemon.
- Without `--frames`, the viewer runs until its window closes; force-killing it
  (`Stop-Process -Force`) can truncate the trace, so prefer the bounded `--frames N` exit for
  automation.
- Only the client is vendored into MMDLab (`Source/ThirdParty/tracy`); the GUI, server, and
  capture tools live in the separate `D:\tracy` checkout and are not part of this repository.
