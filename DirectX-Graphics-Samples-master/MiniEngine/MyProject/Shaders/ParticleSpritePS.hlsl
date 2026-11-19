#include "SpriteAlpha.hlsli"

#define MAX_MATERIAL_TEXTURES 4
#define MAX_MATERIAL_PARAMS 16

Texture2D<float4> SpriteTexture : register(t2); // t0, t1 은 VS (파티클 / 정렬 인덱스)
Texture2D<float4> MaterialTextures[MAX_MATERIAL_TEXTURES] : register(t3);

SamplerState LinearClamp : register(s0);
SamplerState LinearWrap : register(s1);

cbuffer SpritePSConstants : register(b1)
{
    uint BlendMode; // 0 = Translucent, 1 = Additive
    uint TextureAlpha; // 0 = Straight, 1 = Premultiplied, 2 = FromLuminance
    float Time; // Global Time
};

cbuffer MaterialParamsCB : register(b2)
{
    float4 MaterialParams[MAX_MATERIAL_PARAMS];
};

float4 Param(uint k)
{
    return MaterialParams[k];
}

float4 SampleTex(uint slot, float2 uv)
{
    return MaterialTextures[slot].Sample(LinearWrap, uv);
}


struct PSInput
{
    float4 position : SV_Position;
    float4 color : COLOR0;
    float4 uv : TEXCOORD0;
    float2 quadUV : TEXCOORD1;
    nointerpolation float blend : TEXCOORD2;
};

float4 main(PSInput input) : SV_TARGET
{
    const float4 tex0 = SpriteTexture.Sample(LinearClamp, input.uv.xy);
    const float4 tex1 = SpriteTexture.Sample(LinearClamp, input.uv.zw);
    float4 tex = lerp(tex0, tex1, input.blend);
    
    float4 result = ResolveSpriteColor(input.color, tex, TextureAlpha, BlendMode);

    if (result.a < 0.01f)
        discard;
    // 0.xy = Tiling, 0.zw = Offset, 1.xy = Scroll
    const float2 layerUV = input.quadUV * Param(0).xy + Param(0).zw + Param(1).xy * Time;
    const float noise = SampleTex(0, layerUV).r; // temp
    
    result.rgb *= noise;

    return result;   
    
}
