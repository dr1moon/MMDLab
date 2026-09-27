#pragma once

namespace MmdLab
{
// CPU-side mirror of the pixel shader's per-material constant buffer (register b1). The
// layout must match the HLSL cbuffer MaterialParams below exactly (four float4s).
struct MaterialShaderParams
{
    float baseColor[4]; // rgb = diffuse, a = alpha.
    float ambient[3];   // rgb = ambient tint.
    float ambientPad;
    float specular[3]; // rgb = specular tint.
    float shininess;   // Blinn exponent (larger = sharper highlight).
    float sphereMode;  // 0 = off, 1 = multiply, 2 = add, 3 = subtexture (as multiply).
    float pad0;
    float pad1;
    float pad2;
};
static_assert(sizeof(MaterialShaderParams) == 64);

// CPU-side mirror of the camera constant buffer (register b0). The layout must match the HLSL
// cbuffer CameraConstants below exactly: two float4x4s followed by two float4s. The matrices are
// stored column-major (the XMStoreFloat4x4 layout HLSL reads directly, no transpose).
struct CameraConstants
{
    float viewProjection[16];
    float view[16];
    float lightDirection[4];  // xyz = direction toward the light, w = pad.
    float cameraDirection[4]; // xyz = view forward, w = pad.
};
static_assert(sizeof(CameraConstants) == 160);

// Shaders for the static-mesh render milestone. The vertex shader transforms model-space
// positions by the per-instance world matrix and then the view-projection matrix, and forwards
// the normal and UV; the pixel shader implements the canonical MMD toon model: N.L diffused
// through the toon ramp, ambient, Blinn specular, and a multiply/add sphere matcap. The world
// matrix is identity until per-instance placement (manifest or editor) is added.
inline const char* MeshVertexShaderSource = R"(
cbuffer CameraConstants : register(b0)
{
    float4x4 viewProjection;
    float4x4 view;           // model -> view; its upper 3x3 rotates normals into view space.
    float4 lightDirection;   // xyz = normalized direction toward the light.
    float4 cameraDirection;  // xyz = normalized view forward.
};

// Per-instance world transform, supplied as root constants (register b2).
cbuffer InstanceConstants : register(b2)
{
    float4x4 world;
};

struct VSInput
{
    float4 position : POSITION;
    float4 normal : NORMAL;
    float2 uv : TEXCOORD;
};

struct VSOutput
{
    float4 position : SV_POSITION;
    float3 normal : NORMAL;
    float2 uv : TEXCOORD0;
};

VSOutput VSMain(VSInput input)
{
    VSOutput output;
    output.position = mul(viewProjection, mul(world, float4(input.position.xyz, 1.0)));
    // Rotate the normal into view space: first by the instance world's upper 3x3, then by the
    // view matrix's upper 3x3, so the matcap samples a camera-relative direction. The pixel
    // shader re-normalizes after interpolation.
    output.normal = mul((float3x3)view, mul((float3x3)world, input.normal.xyz));
    output.uv = input.uv;
    return output;
}
)";

inline const char* MeshPixelShaderSource = R"(
cbuffer CameraConstants : register(b0)
{
    float4x4 viewProjection;
    float4x4 view;           // layout alignment only; the pixel shader uses lightDirection below.
    float4 lightDirection;
    float4 cameraDirection;
};

cbuffer MaterialParams : register(b1)
{
    float4 baseColor;  // rgb = diffuse, a = alpha.
    float4 ambient;    // rgb = ambient tint.
    float4 specular;   // rgb = specular tint, a = shininess.
    float4 params;     // x = sphere mode.
};

Texture2D baseTex : register(t0);
Texture2D toonTex : register(t1);
Texture2D sphereTex : register(t2);
SamplerState linearSampler : register(s0);

struct PSInput
{
    float4 position : SV_POSITION;
    float3 normal : NORMAL;
    float2 uv : TEXCOORD0;
};

float4 PSMain(PSInput input) : SV_TARGET
{
    float3 normal = normalize(input.normal);
    float3 lightDir = normalize(lightDirection.xyz);
    float3 viewDir = normalize(cameraDirection.xyz);
    float3 halfVec = normalize(lightDir + viewDir);

    float4 base = baseTex.Sample(linearSampler, input.uv);

    // Toon ramp: N.L mapped through a half-lambert coordinate into the 1D ramp (stored as a
    // 2D texture; sample the middle row). Front faces fall in [0.5, 1.0], back faces [0, 0.5].
    float ndotl = dot(normal, lightDir);
    float rampCoord = saturate(ndotl * 0.5 + 0.5);
    float4 toon = toonTex.Sample(linearSampler, float2(rampCoord, 0.5));

    // Ambient is the unlit floor; the toon-lit diffuse fills the range up to full. Blending
    // as ambient + (1 - ambient) * diffuse*toon keeps the lit result from exceeding the
    // diffuse color, so the ramp's gradient stays visible instead of clamping to white.
    float3 lighting = ambient.rgb + (1.0 - ambient.rgb) * (baseColor.rgb * toon.rgb);
    float3 color = base.rgb * lighting;

    // Sphere map (matcap): sample the view-space normal's xy, mapped to [0,1], so the matcap
    // stays fixed to the camera while it orbits.
    float2 sphereUv = normal.xy * 0.5 + 0.5;
    float4 sphere = sphereTex.Sample(linearSampler, sphereUv);

    if (params.x == 1.0)
    {
        color *= sphere.rgb; // multiply.
    }
    else if (params.x == 2.0)
    {
        color += sphere.rgb; // add.
    }
    // 0 = off; 3 = subtexture (sampled with the base UV for now, treated as multiply).

    // Blinn specular.
    float ndoth = saturate(dot(normal, halfVec));
    color += specular.rgb * pow(ndoth, max(specular.a, 1.0));

    float alpha = baseColor.a * base.a * toon.a * sphere.a;
    return float4(color, alpha);
}
)";

// Flat, unlit color shaders for the debug overlays: the skeleton line overlay and the
// skinning-color view. They reuse the same root signature as the toon mesh but consume only the
// camera constant buffer (b0) and the per-instance world matrix (b2); the material constants
// and texture table are simply unused by them.
inline const char* FlatColorVertexShaderSource = R"(
cbuffer CameraConstants : register(b0)
{
    float4x4 viewProjection;
};

cbuffer InstanceConstants : register(b2)
{
    float4x4 world;
};

struct VSInput
{
    float4 position : POSITION;
    float4 color : COLOR;
};

struct VSOutput
{
    float4 position : SV_POSITION;
    float4 color : COLOR;
};

VSOutput VSMain(VSInput input)
{
    VSOutput output;
    output.position = mul(viewProjection, mul(world, float4(input.position.xyz, 1.0)));
    output.color = input.color;
    return output;
}
)";

inline const char* FlatColorPixelShaderSource = R"(
struct PSInput
{
    float4 position : SV_POSITION;
    float4 color : COLOR;
};

float4 PSMain(PSInput input) : SV_TARGET
{
    return input.color;
}
)";
} // namespace MmdLab
