# MMDLab

[![Build and Test](https://github.com/dr1moon/MMDLab/actions/workflows/build-and-test.yml/badge.svg)](https://github.com/dr1moon/MMDLab/actions/workflows/build-and-test.yml)

![MMDLab viewer](Docs/hutao_idle.gif)

A minimal MikuMikuDance runtime in C++, Win32, and DirectX 12.

## Build

Requires Visual Studio 2026 with C++ desktop tools.

```text
GenerateProjects.bat --build Release
```

## Test

```text
Build\Bin\Debug\x64\MmdTests.exe
```

## Profile

Tracy is on by default; build with `--no-tracy` to compile it out. See `Docs/TRACY.md`.
