// Preset: Dissolve
// Tex0.r = 마스크. 진행도 (ParticleParam.x = NormalizedAge) 보다 마스크가 낮은 곳부터 사라진다
//   Param(2).x = 경계 부드러움, Param(2).y = 테두리 폭, Param(3).rgb = 테두리 색
#include "SpritePreset.hlsli"

float4 main(PSInput input) : SV_TARGET
{
    const PresetBase b = SamplePresetBase(input, float2(0.f, 0.f));

    const float mask = SampleTex(0, UVTransform(input.quadUV, 0)).r;
    const float progress = input.params.x;
    const float edge = progress + Param(2).x;

    // 남는 부분 (0 = 사라짐, 1 = 그대로)
    const float keep = smoothstep(progress, edge, mask);
    const float4 color = b.baseColor * keep;

    // 사라지는 경계 바로 안쪽에 테두리 색을 더한다
    const float rimEnd = edge + Param(2).y;
    const float rim = 1.f - smoothstep(edge, rimEnd, mask);
    const float3 rgb = color.rgb + Param(3).rgb * (rim * keep * b.coverage);

    return PresetOutput(float4(rgb, color.a));
}
