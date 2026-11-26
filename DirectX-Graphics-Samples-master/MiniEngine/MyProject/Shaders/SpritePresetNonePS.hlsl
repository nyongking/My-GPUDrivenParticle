// Preset: None
// 효과 없음: 베이스 텍스처 x 파티클 색
#include "SpritePreset.hlsli"

float4 main(PSInput input) : SV_TARGET
{
    const PresetBase b = SamplePresetBase(input, float2(0.f, 0.f));
    return PresetOutput(b.baseColor);
}
