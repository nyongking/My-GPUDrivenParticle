struct PSInput
{
    float4 position : SV_Position;
    float4 color : COLOR0;
    float2 uv : TEXCOORD0;
};

float4 main(PSInput input) : SV_TARGET
{
    float falloff = saturate(1.0f - dot(input.uv, input.uv)); // 가장자리 = 0, 중심부 = 1
    float a = input.color.a * falloff * falloff;
    return float4(input.color.rgb * a, a);
}