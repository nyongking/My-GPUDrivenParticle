#include "SpriteDissolve.hlsli"

Texture2D<float4> SpriteTexture : register(t2); // t0, t1 은 VS (파티클 / 정렬 인덱스)
Texture2D<float4> DissolveMask : register(t3);  // 디졸브 마스크 (R, 선형). 꺼져 있으면 읽지 않음
SamplerState LinearClamp : register(s0);

// C++ SpritePSConstants 와 같은 배치 (루트 상수 12 개)
cbuffer SpritePSConstants : register(b1)
{
    uint BlendMode; // 0 = Translucent, 1 = Additive
    uint TextureAlpha; // 0 = Straight, 1 = Premultiplied, 2 = FromLuminance
    uint DissolveEnabled; // 마스크 텍스처가 있을 때 1
    uint _PsPad;
    float2 DissolveRange;
    float DissolveEdgeWidth;
    float DissolveSoftness;
    float3 DissolveEdgeColor;
    float DissolveEdgeIntensity;
};

struct PSInput
{
    float4 position : SV_Position;
    float4 color : COLOR0;
    float4 uv : TEXCOORD0;
    nointerpolation float blend : TEXCOORD1;
    float2 quadUV : TEXCOORD2;
    nointerpolation float dissolve : TEXCOORD3;
};

float4 main(PSInput input) : SV_TARGET
{
    const float4 tex0 = SpriteTexture.Sample(LinearClamp, input.uv.xy);
    const float4 tex1 = SpriteTexture.Sample(LinearClamp, input.uv.zw);
    float4 tex = lerp(tex0, tex1, input.blend);
    
    
    float4 result = ResolveSpriteColor(input.color, tex, TextureAlpha, BlendMode);

    // 디졸브 (draw 전체가 같은 쪽으로 분기)
    if (DissolveEnabled != 0)
    {
        DissolveParams d;
        d.Range = DissolveRange;
        d.EdgeWidth = DissolveEdgeWidth;
        d.Softness = DissolveSoftness;
        d.EdgeColor = DissolveEdgeColor;
        d.EdgeIntensity = DissolveEdgeIntensity;
        const float mask = DissolveMask.Sample(LinearClamp, input.quadUV).r;
        result = ApplyDissolve(result, SpriteCoverage(input.color, tex, TextureAlpha), mask, input.dissolve, d);
    }
    return result;
}
