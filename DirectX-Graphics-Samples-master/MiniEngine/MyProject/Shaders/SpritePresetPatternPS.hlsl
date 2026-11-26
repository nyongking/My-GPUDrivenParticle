// Preset: Pattern
// 색 x Tex0.r (무늬). Param(0), Param(1) = Tex0 타일링 / 스크롤
#include "SpritePreset.hlsli"

float4 main(PSInput input) : SV_TARGET
{
    const PresetBase b = SamplePresetBase(input, float2(0.f, 0.f));

    const float4 pattern = SampleTex(0, UVTransform(input.quadUV, 0));
    return PresetOutput(float4(b.baseColor.rgb * pattern.r, b.baseColor.a));
}
