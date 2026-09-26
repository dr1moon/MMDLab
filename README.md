# MMDLab

[![Build and Test](https://github.com/dr1moon/MMDLab/actions/workflows/build-and-test.yml/badge.svg)](https://github.com/dr1moon/MMDLab/actions/workflows/build-and-test.yml)

MMDLab is a native Windows experiment for building a minimal MikuMikuDance (MMD) runtime. The project incrementally develops PMX inspection and cooking, animation evaluation, and explicit DirectX 12 rendering with modern C++ and bare Win32 APIs.

## Current Scope

- Build a static PMX mesh pipeline before animation, morphs, inverse kinematics, or physics.
- Keep CPU asset processing, render policy, and DirectX 12 execution behind explicit ownership boundaries.
- Use deterministic frame ownership, bounded back-pressure, and explicit GPU synchronization.

## Build

Requirements: Visual Studio 2026 with C++ desktop development tools. Premake is included in `Tools`.

```text
GenerateProjects.bat
GenerateProjects.bat --build Debug
GenerateProjects.bat --build Release
```

The script uses `vswhere` to find MSBuild, generates the Visual Studio 2026 solution, and optionally builds it.

## Tests

```text
Build\Bin\Debug\x64\MmdTests.exe
Build\Bin\Debug\x64\MmdTests.exe FrameResourcePool
```

`MmdTests` is a lightweight native test runner for Core, asset, and runtime tests. Passing assertions avoid failure-record allocations; failures report their expression and source location.

## Continuous Integration

GitHub Actions runs the Windows Debug build and `MmdTests` on pushes, pull requests, and manual dispatch. The workflow is defined in `.github/workflows/build-and-test.yml`.

## Repository Layout

- `Source/App/MmdViewer`: Win32 viewer entry point and application ownership.
- `Source/Runtime`: Core runtime, asset, render, and DirectX 12 source boundaries.
- `Source/Tests`: Native unit and integration tests.
- `Source/Tools/MmdCooker`: Offline asset cooker.
- `Tools`: Premake and local development utilities.
