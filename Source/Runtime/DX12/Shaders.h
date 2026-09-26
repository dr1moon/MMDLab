#pragma once

namespace MmdLab
{
// Hard-coded triangle shaders for the first render milestone. The vertex shader generates
// a full-screen triangle from the vertex id (no vertex buffer), and the pixel shader writes
// a solid color. These are compiled at runtime by D3DCompile (FXC); move to
// Source/Shaders/*.hlsl + dxc when the shader pipeline matures.
inline const char* TriangleVertexShaderSource = R"(
float4 VSMain(uint vertexId : SV_VertexID) : SV_POSITION
{
    const float2 positions[3] =
    {
        float2(0.0f, 0.5f),
        float2(0.5f, -0.5f),
        float2(-0.5f, -0.5f),
    };
    return float4(positions[vertexId], 0.0f, 1.0f);
}
)";

inline const char* TrianglePixelShaderSource = R"(
float4 PSMain() : SV_TARGET
{
    return float4(0.25f, 0.5f, 0.75f, 1.0f);
}
)";
} // namespace MmdLab
