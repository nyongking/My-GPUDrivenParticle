// ParticleSpritePS.hlsl 과 생성된 머티리얼 PS (MaterialShaderGenerator) 가 같이 쓰는 선언
#ifndef SPRITE_MATERIAL_COMMON_HLSLI
#define SPRITE_MATERIAL_COMMON_HLSLI

#include "SpriteAlpha.hlsli"

#define MAX_MATERIAL_TEXTURES 4
#define MAX_MATERIAL_PARAMS 16

#define EFFECT_NONE         0
#define EFFECT_PATTERN      1
#define EFFECT_TINT         2
#define EFFECT_DISSOLVE     3
#define EFFECT_DISTORTION   4
#define EFFECT_GLOW         5

Texture2D<float4> SpriteTexture : register(t2); // t0, t1 은 VS (파티클 / 정렬 인덱스)
Texture2D<float4> MaterialTextures[MAX_MATERIAL_TEXTURES] : register(t3);

SamplerState LinearClamp : register(s0);
SamplerState LinearWrap : register(s1);

cbuffer SpritePSConstants : register(b1)
{
    uint BlendMode; // 0 = Translucent, 1 = Additive
    uint TextureAlpha; // 0 = Straight, 1 = Premultiplied, 2 = FromLuminance
    float Time; // Global Time
    uint EffectMode; // Temp
    float2 CellSize; // SubUV 한칸 크기
};

cbuffer MaterialParamsCB : register(b2)
{
    float4 MaterialParams[MAX_MATERIAL_PARAMS];
};




struct PSInput
{
    float4 position : SV_Position;
    float4 color : COLOR0;
    float4 uv : TEXCOORD0;
    float2 quadUV : TEXCOORD1;
    nointerpolation float blend : TEXCOORD2;
    nointerpolation float2 params : TEXCOORD3;
};

float4 Param(uint k)
{
    return MaterialParams[k];
}

float4 SampleTex(uint slot, float2 uv)
{
    return MaterialTextures[slot].Sample(LinearWrap, uv);
}

struct MaterialInput
{
    float2 quadUV;
    float4 baseColor;
    float  coverage;
    float2 particleParams;
    float4 subUV;
    float  blend;
    float4 vertexColor;
};

float4 Param(MaterialInput i, uint k)
{
    return MaterialParams[k];
}

float4 SampleTex(MaterialInput i, uint slot, float2 uv)
{
    return MaterialTextures[slot].Sample(LinearWrap, uv);
}

float4 SampleBaseUV(float4 subUV, float2 quadUV, float blend, float2 quadOffset)
{
    const float2 offset = quadOffset * CellSize;
    const float2 origin0 = subUV.xy - quadUV * CellSize;
    const float2 origin1 = subUV.zw - quadUV * CellSize;
    const float2 uv0 = clamp(subUV.xy + offset, origin0, origin0 + CellSize);
    const float2 uv1 = clamp(subUV.zw + offset, origin1, origin1 + CellSize);
    const float4 tex0 = SpriteTexture.Sample(LinearClamp, uv0);
    const float4 tex1 = SpriteTexture.Sample(LinearClamp, uv1);
    return lerp(tex0, tex1, blend);
}

float4 SampleBaseOffset(MaterialInput i, float2 quadOffset)
{
    const float4 tex = SampleBaseUV(i.subUV, i.quadUV, i.blend, quadOffset);
    return ResolveSpriteColor(i.vertexColor, tex, TextureAlpha, BlendMode);
}


float2 UVTransform(float2 uv, uint k)
{
    return uv * Param(k).xy + Param(k).zw + Param(k + 1).xy * Time; // 타일링 + 오프셋
}

float4 SampleBase(PSInput input, float2 quadOffset)
{
    return SampleBaseUV(input.uv, input.quadUV, input.blend, quadOffset);
}

float BaseCoverage(float4 color, float4 tex)
{
    const float texAlpha = (TextureAlpha == ALPHA_FROM_LUMINANCE) ? max(tex.r, max(tex.g, tex.b)) : tex.a;
    return saturate(color.a * texAlpha);
}

// PS 입력 -> 머티리얼 입력 (VM / Generated 공용)
MaterialInput MakeMaterialInput(PSInput input)
{
    const float4 tex = SampleBase(input, float2(0.f, 0.f));

    MaterialInput mi;
    mi.quadUV = input.quadUV;
    mi.baseColor = ResolveSpriteColor(input.color, tex, TextureAlpha, BlendMode);
    mi.coverage = BaseCoverage(input.color, tex);
    mi.particleParams = input.params;
    mi.subUV = input.uv;
    mi.blend = input.blend;
    mi.vertexColor = input.color;
    return mi;
}

#endif
