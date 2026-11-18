#include "SpriteMaterial.hlsli"

Texture2D<float4> SpriteTexture : register(t2); // t0, t1 은 VS (파티클 / 정렬 인덱스)
Texture2D<float4> LayerTextures[MAX_SPRITE_LAYERS] : register(t3); // t3 ~ t6 = 켜진 레이어 순서 (없는 칸은 읽지 않음)
SamplerState LinearClamp : register(s0);
SamplerState LinearWrap : register(s1);    // 레이어 반복 (타일링 / 스크롤)

// C++ SpritePSConstants 와 같은 배치 (336 B)
cbuffer SpritePSConstants : register(b1)
{
    uint BlendMode; // 0 = Translucent, 1 = Additive
    uint TextureAlpha; // 0 = Straight, 1 = Premultiplied, 2 = FromLuminance
    uint LayerCount; // 켜진 레이어 수 (앞에서부터)
    float Time; // 레이어 스크롤 (초)
    SpriteLayerGpu Layers[MAX_SPRITE_LAYERS];
};

struct PSInput
{
    float4 position : SV_Position;
    float4 color : COLOR0;
    float4 uv : TEXCOORD0;
    nointerpolation float blend : TEXCOORD1;
    float2 quadUV : TEXCOORD2;
    nointerpolation float2 params : TEXCOORD3; // MaterialParam0 / 1
};

// 레이어 칸 번호는 펼친 루프의 상수라 텍스처 배열을 바로 고를 수 있다
float4 SampleLayer(uint k, float2 uv, uint flags)
{
    return (flags & LAYER_FLAG_WRAP) ? LayerTextures[k].Sample(LinearWrap, uv) : LayerTextures[k].Sample(LinearClamp, uv);
}

float4 main(PSInput input) : SV_TARGET
{
    // 1) Distort 레이어: 베이스와 뒤 레이어의 uv 를 민다
    float2 distort = 0.f;
    [unroll]
    for (uint d = 0; d < MAX_SPRITE_LAYERS; ++d)
    {
        if (d < LayerCount && Layers[d].Op == LAYER_DISTORT)
        {
            const SpriteLayerGpu L = Layers[d];
            const float4 s = SampleLayer(d, LayerUV(L, input.quadUV, input.uv.xy, Time, 0.f), L.Flags);
            distort += DistortOffset(L, s, LayerParam(L, input.params));
        }
    }

    // 2) 베이스 텍스처 (레이어가 없으면 예전과 같은 결과)
    const float4 tex0 = SpriteTexture.Sample(LinearClamp, input.uv.xy + distort);
    const float4 tex1 = SpriteTexture.Sample(LinearClamp, input.uv.zw + distort);
    const float4 tex = lerp(tex0, tex1, input.blend);

    float4 result = ResolveSpriteColor(input.color, tex, TextureAlpha, BlendMode);
    const float coverage = SpriteCoverage(input.color, tex, TextureAlpha);

    // 3) 나머지 레이어를 순서대로
    [unroll]
    for (uint k = 0; k < MAX_SPRITE_LAYERS; ++k)
    {
        if (k < LayerCount && Layers[k].Op != LAYER_DISTORT)
        {
            const SpriteLayerGpu L = Layers[k];
            const float4 s = SampleLayer(k, LayerUV(L, input.quadUV, input.uv.xy, Time, distort), L.Flags);
            result = ApplyLayer(result, coverage, L, s, LayerParam(L, input.params));
        }
    }
    return result;
}
