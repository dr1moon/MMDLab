#include "Runtime/DX12/Dx12Device.h"

#include <cstdio>
#include <stdexcept>

namespace MmdLab
{
namespace
{
void ThrowOnFailedHresult(const HRESULT result, const char* message)
{
    if (FAILED(result))
    {
        char buffer[192];
        std::snprintf(buffer, sizeof(buffer), "%s (HRESULT 0x%08X)", message, static_cast<unsigned int>(result));
        throw std::runtime_error(buffer);
    }
}

const wchar_t* FeatureLevelToString(const D3D_FEATURE_LEVEL featureLevel)
{
    switch (featureLevel)
    {
    case D3D_FEATURE_LEVEL_12_2: return L"12_2";
    case D3D_FEATURE_LEVEL_12_1: return L"12_1";
    case D3D_FEATURE_LEVEL_12_0: return L"12_0";
    case D3D_FEATURE_LEVEL_11_1: return L"11_1";
    case D3D_FEATURE_LEVEL_11_0: return L"11_0";
    default: return L"unknown";
    }
}

D3D_FEATURE_LEVEL FindHighestFeatureLevel(ID3D12Device* device)
{
    const D3D_FEATURE_LEVEL featureLevels[] =
    {
        D3D_FEATURE_LEVEL_12_2,
        D3D_FEATURE_LEVEL_12_1,
        D3D_FEATURE_LEVEL_12_0,
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
    };

    D3D12_FEATURE_DATA_FEATURE_LEVELS caps{};
    caps.pFeatureLevelsRequested = featureLevels;
    caps.NumFeatureLevels = static_cast<UINT>(sizeof(featureLevels) / sizeof(featureLevels[0]));

    if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_FEATURE_LEVELS, &caps, sizeof(caps))))
    {
        return caps.MaxSupportedFeatureLevel;
    }
    return D3D_FEATURE_LEVEL_11_0;
}

bool IsHardware(const DXGI_ADAPTER_DESC1& description)
{
    return (description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) == 0;
}
} // namespace

Dx12Device::Dx12Device()
{
#ifdef MMDLAB_DEBUG
    // Enable the debug layer in debug builds so D3D12 validation reports misuse. Ignored if
    // the Graphics Tools feature is not installed.
    Microsoft::WRL::ComPtr<ID3D12Debug> debugController;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debugController))))
    {
        debugController->EnableDebugLayer();
    }
#endif

    ThrowOnFailedHresult(
        CreateDXGIFactory2(0, IID_PPV_ARGS(&factory_)),
        "Failed to create the DXGI factory.");

    // Enumerate adapters and pick the best hardware one (most dedicated video memory),
    // falling back to a software adapter when no hardware adapter is present.
    Microsoft::WRL::ComPtr<IDXGIAdapter1> bestAdapter;
    DXGI_ADAPTER_DESC1 bestDescription{};
    bool found = false;

    for (UINT index = 0;; ++index)
    {
        Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
        if (factory_->EnumAdapters1(index, &adapter) == DXGI_ERROR_NOT_FOUND)
        {
            break;
        }

        DXGI_ADAPTER_DESC1 description{};
        if (FAILED(adapter->GetDesc1(&description)))
        {
            continue;
        }

        if (!found)
        {
            bestAdapter = adapter;
            bestDescription = description;
            found = true;
            continue;
        }

        if (IsHardware(description)
            && (!IsHardware(bestDescription) || description.DedicatedVideoMemory > bestDescription.DedicatedVideoMemory))
        {
            bestAdapter = adapter;
            bestDescription = description;
        }
    }

    if (!found)
    {
        throw std::runtime_error("No DXGI adapter was found.");
    }

    adapter_ = std::move(bestAdapter);

    ThrowOnFailedHresult(
        D3D12CreateDevice(adapter_.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device_)),
        "Failed to create the Direct3D 12 device.");

    const D3D_FEATURE_LEVEL featureLevel = FindHighestFeatureLevel(device_.Get());

    info_.adapterName = bestDescription.Description;
    info_.vendorId = bestDescription.VendorId;
    info_.deviceId = bestDescription.DeviceId;
    info_.dedicatedVideoMemoryBytes = bestDescription.DedicatedVideoMemory;
    info_.dedicatedSystemMemoryBytes = bestDescription.DedicatedSystemMemory;
    info_.sharedSystemMemoryBytes = bestDescription.SharedSystemMemory;
    info_.featureLevel = FeatureLevelToString(featureLevel);
    info_.isHardware = IsHardware(bestDescription);
}
} // namespace MmdLab
