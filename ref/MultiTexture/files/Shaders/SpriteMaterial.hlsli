// 스프라이트 텍스처 레이어 (ParticleSpritePS / ParticleListPS 공용). C++ ParticleMaterial.h 와 같은 정의
//   PS 순서: 1) Distort 레이어로 uv 이동량 계산 -> 2) 베이스 텍스처 (이동된 uv) -> 3) 나머지 레이어를 순서대로 적용
//   모든 값은 premultiplied 기준 (BlendPreMultiplied)
#ifndef SPRITE_MATERIAL_HLSLI
#define SPRITE_MATERIAL_HLSLI

#include "SpriteAlpha.hlsli"

#define MAX_SPRITE_LAYERS 4

#define LAYER_OFF        0
#define LAYER_MULTIPLY   1
#define LAYER_ADD        2
#define LAYER_ALPHA_MASK 3
#define LAYER_ERODE      4
#define LAYER_DISTORT    5

#define LAYER_UV_QUAD    0
#define LAYER_UV_SUBUV   1

#define CHANNEL_RGBA      0
#define CHANNEL_R         1
#define CHANNEL_G         2
#define CHANNEL_B         3
#define CHANNEL_A         4
#define CHANNEL_LUMINANCE 5

#define PARAM_CONSTANT 0
#define PARAM_0        1
#define PARAM_1        2

#define LAYER_FLAG_WRAP           1
#define LAYER_FLAG_MASK_BY_SPRITE 2

// C++ SpriteLayerGpu 와 같은 배치 (80 B, 16 B 줄 5개)
struct SpriteLayerGpu
{
    uint Op;
    uint UV;
    uint Channel;
    uint Flags;
    float2 Tiling;
    float2 Offset;
    float2 Scroll;
    float2 ParamRange;
    float3 Tint;
    float Intensity;
    uint Param;
    uint TextureIndex; // 이미터별 경로 = 레이어 칸, 전체 정렬 경로 = 텍스처 표 번호
    float EdgeWidth;
    float Softness;
};

// 블렌드 모드와 상관없는 스프라이트 불투명도 (발광을 스프라이트 모양 안에만 넣을 때)
float SpriteCoverage(float4 color, float4 tex, uint alphaMode)
{
    const float a = (alphaMode == ALPHA_FROM_LUMINANCE) ? max(tex.r, max(tex.g, tex.b)) : tex.a;
    return saturate(color.a * a);
}

float4 SelectChannel(float4 s, uint channel)
{
    if (channel == CHANNEL_R) return s.rrrr;
    if (channel == CHANNEL_G) return s.gggg;
    if (channel == CHANNEL_B) return s.bbbb;
    if (channel == CHANNEL_A) return s.aaaa;
    if (channel == CHANNEL_LUMINANCE) return dot(s.rgb, float3(0.299f, 0.587f, 0.114f)).xxxx;
    return s;
}

// 레이어 세기 0~1 (Constant 는 ParamRange.x 그대로). params = (MaterialParam0, MaterialParam1)
float LayerParam(SpriteLayerGpu L, float2 params)
{
    if (L.Param == PARAM_CONSTANT)
        return L.ParamRange.x;
    const float v = (L.Param == PARAM_1) ? params.y : params.x;
    const float span = L.ParamRange.y - L.ParamRange.x;
    if (abs(span) < 1e-4f)
        return (v >= L.ParamRange.x) ? 1.f : 0.f;
    return saturate((v - L.ParamRange.x) / span);
}

float2 LayerUV(SpriteLayerGpu L, float2 quadUV, float2 frameUV, float time, float2 distort)
{
    const float2 base = (L.UV == LAYER_UV_SUBUV) ? frameUV : quadUV;
    return base * L.Tiling + L.Offset + L.Scroll * time + distort;
}

// Distort: rg (0~1) -> 방향 (-1~1) x 세기
float2 DistortOffset(SpriteLayerGpu L, float4 s, float p)
{
    const float2 v = SelectChannel(s, L.Channel).rg;
    return (v * 2.f - 1.f) * (L.Intensity * p);
}

// Erode: 마스크 < 진행도 인 곳부터 사라지고 경계에 Tint x Intensity 를 더한다 (진행도 0 = 그대로, 1 = 전부 사라짐)
float4 ErodeLayer(float4 premul, float coverage, float mask, float progress, SpriteLayerGpu L)
{
    const float w = max(L.EdgeWidth, 0.f);
    const float s = max(L.Softness, 1e-4f);
    const float dist = mask - progress * (1.f + w + s) + w + s;
    const float keep = saturate(dist / s);
    const float edge = (w > 0.f) ? 1.f - saturate((dist - s) / w) : 0.f;

    float4 o = premul * keep;
    o.rgb += L.Tint * (L.Intensity * edge * keep * coverage);
    return o;
}

// Distort 를 뺀 연산. s = 레이어 텍스처 샘플, p = LayerParam
float4 ApplyLayer(float4 premul, float coverage, SpriteLayerGpu L, float4 s, float p)
{
    const float4 v = SelectChannel(s, L.Channel);
    const float scalar = (L.Channel == CHANNEL_RGBA) ? v.r : v.x;

    if (L.Op == LAYER_MULTIPLY)
    {
        premul.rgb *= lerp(float3(1.f, 1.f, 1.f), v.rgb * L.Tint, saturate(L.Intensity * p));
    }
    else if (L.Op == LAYER_ADD)
    {
        const float a = (L.Channel == CHANNEL_RGBA) ? v.a : 1.f;
        const float m = (L.Flags & LAYER_FLAG_MASK_BY_SPRITE) ? coverage : 1.f;
        premul.rgb += v.rgb * L.Tint * (L.Intensity * p * a * m);
    }
    else if (L.Op == LAYER_ALPHA_MASK)
    {
        const float m = (L.Channel == CHANNEL_RGBA) ? v.a : v.x;
        premul *= lerp(1.f, m, saturate(L.Intensity * p));
    }
    else if (L.Op == LAYER_ERODE)
    {
        premul = ErodeLayer(premul, coverage, scalar, p, L);
    }
    return premul;
}

#endif // SPRITE_MATERIAL_HLSLI
