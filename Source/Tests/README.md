# MMDLab Tests

`MmdTests` is a native C++ console executable for deterministic unit and integration tests that do not require a window, DXGI, or Direct3D 12 device.

## Design

- Tests register through static initialization and run in one process without test discovery subprocesses.
- Passing checks allocate no memory. Failure records allocate only when a test fails.
- Each test is timed with `std::chrono::steady_clock` and reports its own result immediately.
- A name substring filter keeps focused test runs fast.
- Exceptions are reported as test failures. Native crashes remain process failures and must be diagnosed by the caller or continuous integration system.

## Writing a Test

```cpp
#include "Runtime/Core/TestFramework.h"

MMDLAB_TEST(Core.FrameSlot, AcquiresFreeSlot)
{
    MMDLAB_CHECK(slot.IsFree());
    MMDLAB_CHECK_EQUAL(expectedSlotId, actualSlotId);
}
```

## Running Tests

```text
GenerateProjects.bat --build Debug
Build\Bin\Debug\x64\MmdTests.exe
Build\Bin\Debug\x64\MmdTests.exe FrameSlot
```
