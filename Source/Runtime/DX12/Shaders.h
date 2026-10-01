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

// Skinning bone matrices (world * inverse-bind), bound per model (t3). A structured buffer (not
// a constant buffer) keeps the per-vertex dynamic indexing fast.
StructuredBuffer<float4x4> Bones : register(t3);

// Concatenated per-submesh skin-reference-bone lists, bound per model (t4): each entry maps a
// submesh-local u8 index to a global bone index into `Bones`, sliced by the per-draw
// `refBoneOffset` root constant.
StructuredBuffer<uint> RefBones : register(t4);

// Morph deltas (model-space position offset per vertex), bound per model (t5). Indexed by the
// vertex id and added to the position before skinning; zero for models without vertex morphs.
StructuredBuffer<float3> MorphDeltas : register(t5);

// Per-submesh skin-reference-bone slice offset, set as a root constant before each draw.
cbuffer RefBoneConstants : register(b3)
{
    uint refBoneOffset;
};

struct VSInput
{
    float4 position : POSITION;
    float4 normal : NORMAL;
    float2 uv : TEXCOORD;
    float2 uv1 : TEXCOORD1; // Additional UV for the sphere subtexture.
    // Submesh-local skin indices, read directly in the vertex shader and never output to the
    // pixel shader, so they are flat (not interpolated) by construction. The R8G8B8A8_UINT
    // vertex format also prevents any interpolation.
    uint4 blendIndices : BLENDINDICES;
    float4 blendWeights : BLENDWEIGHT;
    uint vertexId : SV_VertexID;
};

struct VSOutput
{
    float4 position : SV_POSITION;
    float3 normal : NORMAL;
    float2 uv : TEXCOORD0;
    float2 uv1 : TEXCOORD1;
};

VSOutput VSMain(VSInput input)
{
    VSOutput output;
    // Linear-blend skinning: resolve each submesh-local index through the skin-reference-bone
    // table to its global bone, then weight that bone's palette matrix. Unused slots carry index
    // 0 and weight 0, so the zero-weight term resolves to a valid bone and never reads out of
    // bounds.
    float4x4 skinMatrix =
        input.blendWeights.x * Bones[RefBones[refBoneOffset + input.blendIndices.x]] +
        input.blendWeights.y * Bones[RefBones[refBoneOffset + input.blendIndices.y]] +
        input.blendWeights.z * Bones[RefBones[refBoneOffset + input.blendIndices.z]] +
        input.blendWeights.w * Bones[RefBones[refBoneOffset + input.blendIndices.w]];

    // Morph the model-space position before skinning: the delta is a model-space offset, so it
    // must land inside the skin transform, not after it.
    float3 morphedPosition = input.position.xyz + MorphDeltas[input.vertexId];
    float4 skinnedPosition = mul(skinMatrix, float4(morphedPosition, 1.0));
    output.position = mul(viewProjection, mul(world, skinnedPosition));

    float3 skinnedNormal = mul((float3x3)skinMatrix, input.normal.xyz);
    // Rotate the normal into view space: first by the instance world's upper 3x3, then by the
    // view matrix's upper 3x3, so the matcap samples a camera-relative direction. The pixel
    // shader re-normalizes after interpolation.
    output.normal = mul((float3x3)view, mul((float3x3)world, skinnedNormal));
    output.uv = input.uv;
    output.uv1 = input.uv1;
    return output;
}
)";

// Static-mesh vertex shader: no skeleton, so no Bones/RefBones/refBoneOffset/blendIndices/
// blendWeights. It still applies per-vertex morph deltas (t5) when the model has vertex morphs.
inline const char* StaticVertexShaderSource = R"(
cbuffer CameraConstants : register(b0)
{
    float4x4 viewProjection;
    float4x4 view;
    float4 lightDirection;
    float4 cameraDirection;
};

// Per-instance world transform, supplied as root constants (register b2).
cbuffer InstanceConstants : register(b2)
{
    float4x4 world;
};

// Morph deltas (model-space position offset per vertex), bound per model (t5). Indexed by the
// vertex id and added to the position; zero for models without vertex morphs.
StructuredBuffer<float3> MorphDeltas : register(t5);

struct VSInput
{
    float4 position : POSITION;
    float4 normal : NORMAL;
    float2 uv : TEXCOORD;
    float2 uv1 : TEXCOORD1;
    uint vertexId : SV_VertexID;
};

struct VSOutput
{
    float4 position : SV_POSITION;
    float3 normal : NORMAL;
    float2 uv : TEXCOORD0;
    float2 uv1 : TEXCOORD1;
};

VSOutput VSMain(VSInput input)
{
    VSOutput output;
    float3 morphedPosition = input.position.xyz + MorphDeltas[input.vertexId];
    output.position = mul(viewProjection, mul(world, float4(morphedPosition, 1.0)));
    output.normal = mul((float3x3)view, mul((float3x3)world, input.normal.xyz));
    output.uv = input.uv;
    output.uv1 = input.uv1;
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
    float2 uv1 : TEXCOORD1;
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

    // Sphere map: multiply/add (mode 1/2) sample the matcap with the view-space normal; the
    // subtexture (mode 3) is a second texture layer sampled with the additional UV instead.
    float2 sphereUv = (params.x == 3.0) ? input.uv1 : (normal.xy * 0.5 + 0.5);
    float4 sphere = sphereTex.Sample(linearSampler, sphereUv);

    if (params.x == 1.0 || params.x == 3.0)
    {
        color *= sphere.rgb; // multiply / subtexture.
    }
    else if (params.x == 2.0)
    {
        color += sphere.rgb; // add.
    }
    // 0 = off.

    // Blinn specular.
    float ndoth = saturate(dot(normal, halfVec));
    color += specular.rgb * pow(ndoth, max(specular.a, 1.0));

    float alpha = baseColor.a * base.a * toon.a * sphere.a;
    return float4(color, alpha);
}
)";

// Pixel shader for the reflective floor: samples the offscreen reflection target (bound as a raw
// root SRV at t6) at the fragment's screen position and tints it with the material's base color.
// Opaque (mirror) in this pass; a glass variant adds blend and fresnel later.
inline const char* ReflectPixelShaderSource = R"(
cbuffer MaterialParams : register(b1)
{
    float4 baseColor; // rgb = diffuse tint.
    float4 ambient;
    float4 specular;
    float4 params;
};

Texture2D reflectionTex : register(t6);
SamplerState linearSampler : register(s0);

struct PSInput
{
    float4 position : SV_POSITION;
    float3 normal : NORMAL;
    float2 uv : TEXCOORD0;
    float2 uv1 : TEXCOORD1;
};

float4 ReflectPSMain(PSInput input) : SV_TARGET
{
    // The offscreen target matches the back buffer, so its own dimensions normalize the
    // screen-space sample. The reflected view already mirrors the geometry below the floor, so the
    // reflection is sampled directly at the fragment's screen position (no vertical flip).
    uint2 dimensions;
    reflectionTex.GetDimensions(dimensions.x, dimensions.y);
    float2 screenUv = (input.position.xy + 0.5) / float2(dimensions);
    float4 reflection = reflectionTex.Sample(linearSampler, screenUv);
    return float4(reflection.rgb * baseColor.rgb, 1.0);
}
)";

} // namespace MmdLab
