#pragma once

#include <d3d12.h>
#include <wrl/client.h>

#include <cstddef>
#include <cstdint>

namespace MmdLab
{
// Compiles a single HLSL entry point to shader bytecode. The minimal renderer uses FXC
// (D3DCompile), which produces deprecated DXBC; a later milestone will switch to dxc/DXIL.
class ShaderCompiler final
{
public:
    ShaderCompiler() = delete;

    // Compiles `source` with the given entry point and target (e.g. "VSMain", "vs_5_1").
    // Returns the compiled bytecode, or throws std::runtime_error with the compiler output.
    [[nodiscard]] static Microsoft::WRL::ComPtr<ID3DBlob> Compile(
        const char* source,
        const char* entryPoint,
        const char* target);
};
} // namespace MmdLab
