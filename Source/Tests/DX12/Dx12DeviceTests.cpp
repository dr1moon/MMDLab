#include "Runtime/Core/TestFramework.h"
#include "Runtime/DX12/Dx12Device.h"

#include <cstdio>

namespace
{
uint64_t BytesToMiB(const uint64_t bytes)
{
    return bytes / (1024ull * 1024ull);
}
} // namespace

MMDLAB_TEST(DX12.Dx12Device, CreatesDeviceAndExtractsGpuInfo)
{
    MmdLab::Dx12Device device;
    const MmdLab::GpuInfo& info = device.Info();

    std::printf("GPU adapter: %ls\n", info.adapterName.c_str());
    std::printf("  vendor 0x%04X, device 0x%04X\n", info.vendorId, info.deviceId);
    std::printf("  dedicated video memory: %llu MiB\n", static_cast<unsigned long long>(BytesToMiB(info.dedicatedVideoMemoryBytes)));
    std::printf("  dedicated system memory: %llu MiB\n", static_cast<unsigned long long>(BytesToMiB(info.dedicatedSystemMemoryBytes)));
    std::printf("  shared system memory: %llu MiB\n", static_cast<unsigned long long>(BytesToMiB(info.sharedSystemMemoryBytes)));
    std::printf("  feature level: %ls\n", info.featureLevel.c_str());
    std::printf("  hardware: %s\n", info.isHardware ? "yes" : "no");

    MMDLAB_CHECK(device.Get() != nullptr);
    MMDLAB_CHECK(!info.adapterName.empty());
    MMDLAB_CHECK(info.featureLevel != L"unknown");
}
