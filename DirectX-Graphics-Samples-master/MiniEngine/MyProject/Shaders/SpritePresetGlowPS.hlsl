// Preset: Glow
// Tex0.r x Tex1.r 만큼 색을 더한다 (두 텍스처를 다른 속도로 흘려서 일렁이게)
//   Param(0), Param(1) = Tex0 타일링 / 스크롤, Param(2), Param(3) = Tex1 타일링 / 스크롤, Param(4).rgb = 더할 색
#include "SpritePreset.hlsli"

float4 main(PSInput input) : SV_TARGET
{
    const PresetBase b = SamplePresetBase(input, float2(0.f, 0.f));

    const float a = SampleTex(0, UVTransform(input.quadUV, 0)).r;
    const float c = SampleTex(1, UVTransform(input.quadUV, 2)).r;
    const float3 rgb = b.baseColor.rgb + Param(4).rgb * (a * c * b.coverage);

    return PresetOutput(float4(rgb, b.baseColor.a));
}
