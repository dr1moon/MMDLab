#pragma once

#include <cstdint>
#include <string>

namespace MmdLab
{
// Extracted information about one graphics adapter, captured at device creation time. It is
// a plain data record with no DirectX object lifetimes, so it can be logged or inspected
// freely.
struct GpuInfo
{
    std::wstring adapterName;                 // e.g. "AMD Radeon RX 7900 XTX".
    uint32_t vendorId = 0;                    // PCI vendor id, e.g. 0x1002 for AMD.
    uint32_t deviceId = 0;                    // PCI device id.
    uint64_t dedicatedVideoMemoryBytes = 0;   // Dedicated graphics memory.
    uint64_t dedicatedSystemMemoryBytes = 0;  // Dedicated system memory.
    uint64_t sharedSystemMemoryBytes = 0;     // Shared system memory.
    std::wstring featureLevel;                // e.g. "12_1".
    bool isHardware = false;                  // false for software (WARP / basic) adapters.
};
} // namespace MmdLab
