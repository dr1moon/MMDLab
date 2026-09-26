# RenderDoc Frame Verification

This document records the automated workflow for capturing a GPU frame from MMDLab and
verifying its contents with [rdc-cli](https://github.com/BANANASJIM/rdc-cli), a
command-line interface over [RenderDoc](https://renderdoc.org/) captures.

The workflow has three stages: **capture** (RenderDoc in-app API produces a `.rdc`),
**inspect** (rdc-cli opens the `.rdc` and lists its contents), and **verify** (assert on
pixels, state, or validation errors). Stages two and three compose with `grep`/`jq` and
with CI, because every rdc-cli assertion exits `0` (pass), `1` (fail), or `2` (error).

## Prerequisites

- RenderDoc installed (its `renderdoccmd.exe`, `renderdoc.dll`, and the `renderdoc_app.h`
  in-app API header).
- rdc-cli installed: `pip install rdc-cli`.
- The RenderDoc Python bindings (`renderdoc.pyd`) built and on `RENDERDOC_PYTHON_PATH`.

### Building the RenderDoc Python bindings

`rdc setup-renderdoc` builds `renderdoc.pyd`, `renderdoc.dll`, and `renderdoccmd.exe` from
source. Two machine-specific notes from the current setup (Visual Studio 2026 / toolset
v145):

1. `rdc/_build_renderdoc.py` hardcodes `/p:PlatformToolset=v143` (Visual Studio 2022). On a
   v145 machine, patch that line to `v145` first.
2. The full `renderdoc.sln` build fails in qrenderdoc (the GUI) with
   `stdext::make_checked_array_iterator` (removed in v145). Ignore it: the artifacts we
   need are built before that point. Copy them manually, since the install step never runs:

```powershell
# build output: <build-dir>/renderdoc/x64/Release/pymodules/renderdoc.pyd
#                <build-dir>/renderdoc/x64/Release/renderdoc.dll
$install = "C:\Users\42960\AppData\Local\rdc\renderdoc-python"
Copy-Item renderdoc.pyd $install
Copy-Item renderdoc.dll $install      # the .pyd needs the .dll next to it
```

Then point rdc-cli at it and verify:

```powershell
$env:RENDERDOC_PYTHON_PATH = $install
rdc doctor   # renderdoc-module: version=1.41, replay-support ok
```

## Capture

The in-app API is wrapped by `Runtime/DX12/RenderDocCapture`, which loads `renderdoc.dll`
at runtime and calls `StartFrameCapture` / `EndFrameCapture`. The test
`Tests/DX12/Dx12RenderDocCaptureTests.cpp` runs one clear inside a capture:

```text
load RenderDoc  ->  set capture path  ->  create device  ->  StartFrameCapture
    ->  Reset / ClearRenderTargetView / Close  ->  ExecuteCommandLists + fence
    ->  EndFrameCapture
```

`renderdoc.dll` must be next to the capturing executable (or on its DLL search path), and
it must be the **same version** as the replay module (both v1.41 here; the system-installed
RenderDoc is v1.43, so do not use it for capture).

```powershell
Copy-Item renderdoc.dll Build\Bin\Debug\x64\renderdoc.dll
Build\Bin\Debug\x64\MmdTests.exe DX12.Dx12RenderDocCapture
# -> Build\Bin\Debug\x64\captures\clear_capture.rdc
```

## Capturing the Viewer (triangle)

`MmdViewer` captures its first frame when the `MMDLAB_CAPTURE` environment variable is set.
The capture lives on `RhiThread` (`Runtime/DX12/RhiThread.cpp`): `Init()` loads RenderDoc
before creating the device so it hooks device creation, and `Run()` wraps the first frame in
`StartFrameCapture` / `EndFrameCapture`:

```powershell
$env:MMDLAB_CAPTURE = "1"
Build\Bin\Debug\x64\MmdViewer.exe
# -> captures\triangle_threaded_capture.rdc  (relative to the process working directory)
```

`SetCapturePath("captures/triangle_threaded")` names the file and RenderDoc appends a `_capture`
suffix. `renderdoc.dll` (v1.41) must sit next to `MmdViewer.exe`. Force-killing the viewer
(`Stop-Process -Force`) can leave the GPU mid-transition and make the next capture silently
produce no file; add a short delay before re-running.

## Inspect

```bash
export RENDERDOC_PYTHON_PATH="/c/Users/42960/AppData/Local/rdc/renderdoc-python"
RDC=rdc
$RDC open captures/clear_capture.rdc
$RDC info      # API, event/draw/clear counts
$RDC events    # EID TYPE NAME  -- find the clear/draw action
$RDC resources # ID TYPE NAME   -- find the render target (Texture)
$RDC close
```

For the current clear capture, `rdc info` reports `Clears: 1`, and `rdc events` shows the
clear at EID 3; the render target is the `Texture` resource (ID 276).

## Verify

For a **draw call**, assert a pixel against its color target:

```bash
$RDC assert-pixel <EID> <X> <Y> --expect "R G B A" --tolerance 0.02
$RDC assert-clean                    # no D3D12 validation errors
$RDC assert-count draws --expect N   # draw count
```

For a **raw clear** (which has no bound color target), `assert-pixel` reports
"no color targets"; export the render target and read the pixel instead:

```bash
$RDC texture <ID> -o out.png
python -c "from PIL import Image; print(Image.open('out.png').getpixel((128,128)))"
```

The verified clear color is `(0.25, 0.5, 0.75, 1.0)` in float, which lands on
`(64, 128, 191, 255)` in `R8G8B8A8_UNORM`.

For the triangle capture, the draw is the `DrawInstanced` action; assert its center pixel:

```bash
$RDC assert-pixel 11 792 430 --expect "0.25 0.5 0.75 1.0" --tolerance 0.02
# pass: pixel (792, 430) = 0.2510 0.5020 0.7490 1.0000
```

The render target is 1584x861 (the window client area), so (792, 430) is its center, which
lies inside the triangle. The triangle uses the same color as the clear test.

## Gotchas

- `assert-pixel` requires a draw with a bound color target; a bare `ClearRenderTargetView`
  has none. Use the texture export for clears, `assert-pixel` for draws.
- Capture `renderdoc.dll` and replay `renderdoc.pyd` must be the same RenderDoc version.
- `rdc setup-renderdoc` needs the v143 -> v145 patch on Visual Studio 2026, and its GUI
  (qrenderdoc) fails; build and copy the three needed artifacts manually.
- A stale rdc-cli session blocks `rdc open`; clear it with `rdc close` first.
- D3D12 has no default viewport: call `RSSetViewports` + `RSSetScissorRects` before drawing,
  or the triangle rasterizes to nothing. `assert-pixel` reading the background color caught
  this, where "the program did not crash" would not have.
