#include "SpriteAlpha.hlsli"

#define MAX_MATERIAL_TEXTURES 4
#define MAX_MATERIAL_PARAMS 16

#define EFFECT_PATTERN 0
#define EFFECT_TINT    1
#define EFFECT_DISSOLVE 2
#define EFFECT_DISTORTION 3
#define EFFECT_GLOW 4

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
};

float4 Param(uint k)
{
    return MaterialParams[k];
}

float4 SampleTex(uint slot, float2 uv)
{
    return MaterialTextures[slot].Sample(LinearWrap, uv);
}

float2 UVTransform(float2 uv, uint k)
{
    return uv * Param(k).xy + Param(k).zw + Param(k + 1).xy * Time; // 타일링 + 오프셋
}

float4 SampleBase(PSInput input, float2 quadOffset)
{
    const float2 offset = quadOffset * CellSize;
    const float2 origin0 = input.uv.xy - input.quadUV * CellSize;
    const float2 origin1 = input.uv.zw - input.quadUV * CellSize;
    const float2 uv0 = clamp(input.uv.xy + offset, origin0, origin0 + CellSize);
    const float2 uv1 = clamp(input.uv.zw + offset, origin1, origin1 + CellSize);
    const float4 tex0 = SpriteTexture.Sample(LinearClamp, uv0);
    const float4 tex1 = SpriteTexture.Sample(LinearClamp, uv1);
    return lerp(tex0, tex1, input.blend);
}

float BaseCoverage(float4 color, float4 tex)
{
    const float texAlpha = (TextureAlpha == ALPHA_FROM_LUMINANCE) ? max(tex.r, max(tex.g, tex.b)) : tex.a;
    return saturate(color.a * texAlpha);
}

float4 main(PSInput input) : SV_TARGET
{
    float2 distortion = float2(0.f, 0.f);
    if (EffectMode == EFFECT_DISTORTION)
    {
        const float2 n0 = input.quadUV;
        const float2 n1 = UVTransform(n0, 0);
        const float4 n2 = SampleTex(0, n1);
        distortion = (n2.rg * 2.f - 1.f) * Param(2).xy;
    }
    
    // distortion 적용
    const float4 tex = SampleBase(input, distortion);
    const float4 baseColor = ResolveSpriteColor(input.color, tex, TextureAlpha, BlendMode);
    const float coverage = BaseCoverage(input.color, tex);
    
    float4 result = baseColor;
    
    if (EffectMode == EFFECT_PATTERN)
    {
        const float2 n0 = input.quadUV;
        const float2 n1 = UVTransform(n0, 0);
        const float4 n2 = SampleTex(0, n1);
        result = float4(baseColor.rgb * n2.r, baseColor.a);
    }
    else if (EffectMode == EFFECT_TINT)
    {
        const float2 n0 = input.quadUV;
        const float2 n1 = UVTransform(n0, 0);
        const float4 n2 = SampleTex(0, n1);
        result = float4(baseColor.rgb * n2.rgb, baseColor.a); 
    }
    else if (EffectMode == EFFECT_DISSOLVE)
    {
        const float2 n0 = input.quadUV;
        const float2 n1 = UVTransform(n0, 0);
        const float4 n2 = SampleTex(0, n1);     // mask
        const float n3 = Param(2).z;            
        const float n4 = n3 + Param(2).x;
        const float n5 = smoothstep(n3, n4, n2.r);
        const float4 n6 = baseColor * n5;
        const float n7 = n4 + Param(2).y;       // 
        const float n8 = 1.f - smoothstep(n4, n7, n2.r);
        const float3 n9 = n6.rgb + Param(3).rgb * (n8 * n5 * coverage);
        result = float4(n9, n6.a);
    }
    else if (EffectMode == EFFECT_DISTORTION)
    {
        const float2 n3 = input.quadUV + distortion;
        const float4 n4 = SampleTex(1, n3);
        result = float4(baseColor.rgb * n4.r, baseColor.a);
    }
    else if (EffectMode == EFFECT_GLOW)
    {
        const float2 n0 = input.quadUV;
        const float2 n1 = UVTransform(n0, 0);
        const float4 n2 = SampleTex(0, n1);
        const float2 m1 = UVTransform(n0, 2);
        const float4 m2 = SampleTex(1, m1);
        const float n3 = n2.r * m2.r;
        const float3 n4 = baseColor.rgb + Param(4).rgb * (n3 * coverage);
        result = float4(n4, baseColor.a);
    }
         
    return result;
}
