#include "Runtime/DX12/ShaderCompiler.h"
#include "Runtime/DX12/Shaders.h"
#include "Runtime/Core/TestFramework.h"

// Compiles the runtime mesh shaders so HLSL syntax errors surface in the test suite rather than
// at renderer startup. The renderer compiles these lazily; this test runs without a full renderer.
MMDLAB_TEST(DX12.Shader, CompilesMeshShaders)
{
    const auto vertex = MmdLab::ShaderCompiler::Compile(MmdLab::MeshVertexShaderSource, "VSMain", "vs_5_1");
    MMDLAB_CHECK(vertex != nullptr);

    const auto staticVertex = MmdLab::ShaderCompiler::Compile(MmdLab::StaticVertexShaderSource, "VSMain", "vs_5_1");
    MMDLAB_CHECK(staticVertex != nullptr);

    const auto pixel = MmdLab::ShaderCompiler::Compile(MmdLab::MeshPixelShaderSource, "PSMain", "ps_5_1");
    MMDLAB_CHECK(pixel != nullptr);
}
