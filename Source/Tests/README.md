# MMDLab Tests

`MmdTests` is a native C++ console executable for deterministic unit and integration tests. Most tests are pure CPU; one Direct3D 12 test creates a device and falls back to WARP when no hardware adapter is present.

## Design

- Tests register through static initialization and run in one process without test discovery subprocesses.
- Passing checks allocate no memory. Failure records allocate only when a test fails.
- Each test is timed with `std::chrono::steady_clock` and reports its own result immediately.
- A name substring filter keeps focused test runs fast.
- Exceptions are reported as test failures. Native crashes remain process failures and must be diagnosed by the caller or continuous integration system.

## Writing a Test

```cpp
#include "Runtime/Core/TestFramework.h"
#include "Runtime/Core/FrameResourcePool.h"

MMDLAB_TEST(Core.FrameResourcePool, AcquiresAFreeFrame)
{
    MmdLab::FrameResourcePool pool;
    const MmdLab::FrameIndex index = pool.Acquire();
    MMDLAB_CHECK(index < MmdLab::FrameResourcePool::kFrameCount);
}
```

## Running Tests

```text
GenerateProjects.bat --build Debug
Build\Bin\Debug\x64\MmdTests.exe
Build\Bin\Debug\x64\MmdTests.exe FrameResourcePool
```
