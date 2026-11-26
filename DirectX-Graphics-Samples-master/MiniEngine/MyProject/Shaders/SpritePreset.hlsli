// Preset 모드의 효과별 픽셀 셰이더 (SpritePreset*PS.hlsl) 가 같이 쓰는 부분
//
//   Preset = 효과 하나를 고정된 셰이더로 그린다. 효과마다 파일이 따로 있고 EffectMode 분기가 없다.
//   (여러 효과를 섞거나 직접 짠 그래프는 Custom 모드: MaterialShaderGenerator 가 만든 셰이더로 그린다)
//
//   Preset 을 새로 추가하려면
//     1. ParticleMaterial::Effect 에 항목 추가 + 입력값 / 텍스처 수 (NumParams, NumEffectTextures, DefaultParam ...)
//     2. Shaders/SpritePreset<이름>PS.hlsl 작성 (아래 두 함수 사용) + vcxproj 에 Pixel 셰이더로 등록
//     3. SpriteRenderer.cpp 의 kPresetShaders 표에 한 줄 추가
//
//   입력값 / 텍스처 번호는 효과 하나만 쓸 때의 배치다: Param(0), Param(1) = Tex0 타일링 / 스크롤, Param(2) ~ = 효과별 값
#ifndef SPRITE_PRESET_HLSLI
#define SPRITE_PRESET_HLSLI

#include "SpriteMaterialCommon.hlsli"

struct PresetBase
{
    float4 baseColor; // 베이스 텍스처 x 파티클 색 (알파 방식 / 블렌드 모드 적용 후)
    float coverage; // 원래 스프라이트 모양의 알파 (효과가 색을 더할 때 모양 밖으로 새지 않게)
};

// 베이스 텍스처 (SubUV 포함) 를 읽는다. quadOffset = 사각형 uv 를 밀 양 (Distortion 이 씀, 보통 0)
PresetBase SamplePresetBase(PSInput input, float2 quadOffset)
{
    const float4 tex = SampleBase(input, quadOffset);

    PresetBase b;
    b.baseColor = ResolveSpriteColor(input.color, tex, TextureAlpha, BlendMode);
    b.coverage = BaseCoverage(input.color, tex);
    return b;
}

// 마지막 단계: 거의 안 보이는 픽셀은 버린다
float4 PresetOutput(float4 result)
{
    const float visible = (BlendMode == 1) ? max(result.r, max(result.g, result.b)) : result.a;
    if (visible < 0.01f)
        discard;
    return result;
}

#endif
