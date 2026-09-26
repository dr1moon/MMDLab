#include "Runtime/DX12/ShaderCompiler.h"

#include <d3dcompiler.h>

#include <cstring>
#include <stdexcept>
#include <string>

namespace MmdLab
{
Microsoft::WRL::ComPtr<ID3DBlob> ShaderCompiler::Compile(
    const char* source,
    const char* entryPoint,
    const char* target)
{
    Microsoft::WRL::ComPtr<ID3DBlob> bytecode;
    Microsoft::WRL::ComPtr<ID3DBlob> errors;

    const HRESULT result = D3DCompile(
        source,
        strlen(source),
        nullptr,
        nullptr,
        nullptr,
        entryPoint,
        target,
        0,
        0,
        &bytecode,
        &errors);

    if (FAILED(result))
    {
        const char* message = errors != nullptr
            ? static_cast<const char*>(errors->GetBufferPointer())
            : "unknown shader compilation error";
        throw std::runtime_error("Shader compilation failed: " + std::string(message));
    }

    return bytecode;
}
} // namespace MmdLab
