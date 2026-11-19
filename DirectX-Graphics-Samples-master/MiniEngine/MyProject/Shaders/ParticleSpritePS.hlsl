#include "SpriteAlpha.hlsli"

Texture2D<float4> SpriteTexture : register(t2); // t0, t1 은 VS (파티클 / 정렬 인덱스)
Texture2D<float4> LayerTexture : register(t3); // Temp: Layer을 둔다.

SamplerState LinearClamp : register(s0);
SamplerState LinearWrap : register(s1);

cbuffer SpritePSConstants : register(b1)
{
    uint BlendMode; // 0 = Translucent, 1 = Additive
    uint TextureAlpha; // 0 = Straight, 1 = Premultiplied, 2 = FromLuminance
    float Time; // Global Time
};


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
    const float2 layerUV = input.quadUV * float2(1.f, 1.f) + float2(0.3f, 0.3f) * Time;
    const float noise = LayerTexture.Sample(LinearWrap, layerUV).r;
    
    result.rgb *= noise;

    return result;   
    
}
