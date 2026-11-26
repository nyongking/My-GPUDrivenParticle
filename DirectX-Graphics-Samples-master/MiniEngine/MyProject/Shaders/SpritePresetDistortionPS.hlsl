// Preset: Distortion
// Tex0.rg = 밀 방향 (0.5 가 중립). 베이스 텍스처를 읽는 uv 를 민다
//   Param(2).xy = 세기
#include "SpritePreset.hlsli"

float4 main(PSInput input) : SV_TARGET
{
    const float4 noise = SampleTex(0, UVTransform(input.quadUV, 0));
    const float2 offset = (noise.rg * 2.f - 1.f) * Param(2).xy;

    const PresetBase b = SamplePresetBase(input, offset);
    return PresetOutput(b.baseColor);
}
