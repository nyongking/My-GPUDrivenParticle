#include "SpriteMaterial.hlsli"
#include "ParticleListCommon.hlsli"
#define MAX_TEXTURES 32 // C++ ParticleRenderList::kMaxTextures (베이스 + 레이어 텍스처)

Texture2D<float4> SpriteTextures[MAX_TEXTURES] : register(t0, space1);
StructuredBuffer<EmitterInfo> Emitters : register(t2); // VS 와 같은 루트 SRV (PS 에서도 보이게 함)
StructuredBuffer<SpriteLayerGpu> Layers : register(t3); // 이미터마다 MAX_SPRITE_LAYERS 칸
SamplerState LinearClamp : register(s0);
SamplerState LinearWrap : register(s1);

cbuffer ListPSConstants : register(b1)
{
    float Time; // 레이어 스크롤 (초)
};

struct PSInput
{
    float4 position : SV_Position;
    float4 color : COLOR0;
    float4 uv : TEXCOORD0;
    nointerpolation float blend : TEXCOORD1;
    nointerpolation uint textureIndex : TEXCOORD2;
    nointerpolation uint blendMode : TEXCOORD3;
    nointerpolation uint textureAlpha : TEXCOORD4;
    float2 quadUV : TEXCOORD5;
    nointerpolation uint emitterIndex : TEXCOORD6;
    nointerpolation float2 params : TEXCOORD7;
};

// 한 draw 안에서 이미터마다 텍스처가 달라서 non-uniform 인덱스
float4 SampleLayer(SpriteLayerGpu L, float2 uv)
{
    const uint t = NonUniformResourceIndex(L.TextureIndex);
    return (L.Flags & LAYER_FLAG_WRAP) ? SpriteTextures[t].Sample(LinearWrap, uv) : SpriteTextures[t].Sample(LinearClamp, uv);
}

float4 main(PSInput input) : SV_TARGET
{
    const EmitterInfo e = Emitters[input.emitterIndex];
    const uint layerBase = input.emitterIndex * MAX_SPRITE_LAYERS;

    // 1) Distort 레이어: 베이스와 뒤 레이어의 uv 를 민다
    float2 distort = 0.f;
    for (uint d = 0; d < e.LayerCount; ++d)
    {
        const SpriteLayerGpu L = Layers[layerBase + d];
        if (L.Op == LAYER_DISTORT)
        {
            const float4 s = SampleLayer(L, LayerUV(L, input.quadUV, input.uv.xy, Time, 0.f));
            distort += DistortOffset(L, s, LayerParam(L, input.params));
        }
    }

    // 2) 베이스 텍스처 (레이어가 없으면 예전과 같은 결과)
    const uint t = NonUniformResourceIndex(input.textureIndex);
    const float4 tex0 = SpriteTextures[t].Sample(LinearClamp, input.uv.xy + distort);
    const float4 tex1 = SpriteTextures[t].Sample(LinearClamp, input.uv.zw + distort);
    const float4 tex = lerp(tex0, tex1, input.blend);

    // premultiplied 블렌드: Additive 는 alpha 0 (뒤를 지우지 않고 더하기만)
    float4 result = ResolveSpriteColor(input.color, tex, input.textureAlpha, input.blendMode);
    const float coverage = SpriteCoverage(input.color, tex, input.textureAlpha);

    // 3) 나머지 레이어를 순서대로
    for (uint k = 0; k < e.LayerCount; ++k)
    {
        const SpriteLayerGpu L = Layers[layerBase + k];
        if (L.Op != LAYER_DISTORT)
        {
            const float4 s = SampleLayer(L, LayerUV(L, input.quadUV, input.uv.xy, Time, distort));
            result = ApplyLayer(result, coverage, L, s, LayerParam(L, input.params));
        }
    }
    return result;
}
