// 디졸브 (ParticleSpritePS / ParticleListPS 공용)
//   마스크 텍스처(R) 가 진행도보다 작은 곳부터 사라진다. 경계에는 발광색을 더한다
//   진행도 = saturate((Dissolve 바인딩 값 - Range.x) / (Range.y - Range.x))
//     0 = 전부 보임, 1 = 전부 사라짐. 예) Dissolve <- Particles.NormalizedAge, Range (0.5, 1) = 수명 후반에 사라짐
#ifndef SPRITE_DISSOLVE_HLSLI
#define SPRITE_DISSOLVE_HLSLI

#include "SpriteAlpha.hlsli"

struct DissolveParams
{
    float2 Range;          // 바인딩 값 -> 진행도 0 ~ 1
    float EdgeWidth;       // 경계 발광 폭 (마스크 값 단위, 0 = 발광 없음)
    float Softness;        // 잘리는 곳의 부드러운 폭 (마스크 값 단위)
    float3 EdgeColor;
    float EdgeIntensity;
};

// 블렌드 모드와 상관없는 스프라이트 불투명도 (경계 발광을 스프라이트 모양 안에만 넣기 위해)
float SpriteCoverage(float4 color, float4 tex, uint alphaMode)
{
    const float a = (alphaMode == ALPHA_FROM_LUMINANCE) ? max(tex.r, max(tex.g, tex.b)) : tex.a;
    return saturate(color.a * a);
}

// premul = ResolveSpriteColor 결과 (premultiplied). 결과도 premultiplied
float4 ApplyDissolve(float4 premul, float coverage, float mask, float value, DissolveParams d)
{
    const float amount = saturate((value - d.Range.x) / max(d.Range.y - d.Range.x, 1e-4f));
    const float w = max(d.EdgeWidth, 0.f);
    const float s = max(d.Softness, 1e-4f);

    // amount 0 -> dist >= w + s (전부 남고 발광 없음), amount 1 -> dist <= 0 (전부 사라짐)
    const float dist = mask - amount * (1.f + w + s) + w + s;
    const float keep = saturate(dist / s);
    const float edge = (w > 0.f) ? 1.f - saturate((dist - s) / w) : 0.f;

    float4 o = premul * keep;
    o.rgb += d.EdgeColor * (d.EdgeIntensity * edge * keep * coverage);
    return o;
}

#endif // SPRITE_DISSOLVE_HLSLI
