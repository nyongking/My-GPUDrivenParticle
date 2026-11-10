Texture2D<float4> SpriteTexture : register(t2); // t0, t1 은 VS (파티클 / 정렬 인덱스)
SamplerState LinearClamp : register(s0);

cbuffer SpritePSConstants : register(b1)
{
    uint BlendMode; // 0 = Translucent, 1 = Additive
};

struct PSInput
{
    float4 position : SV_Position;
    float4 color : COLOR0;
    float4 uv : TEXCOORD0;
    nointerpolation float blend : TEXCOORD1;
};

float4 main(PSInput input) : SV_TARGET
{
    const float4 tex0 = SpriteTexture.Sample(LinearClamp, input.uv.xy);
    const float4 tex1 = SpriteTexture.Sample(LinearClamp, input.uv.zw);
    const float4 tex = lerp(tex0, tex1, input.blend);

    const float a = input.color.a * tex.a;
    const float3 rgb = input.color.rgb * tex.rgb * a;

    return float4(rgb, (BlendMode == 1) ? 0.f : a);
}
