Texture2D<float4>   QuadTexture : register(t0);
SamplerState        LinearSampler : register(s0);

cbuffer TintConstants : register(b2)
{
    float4 Tint;
}

struct PSInput
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

float4 main(PSInput input) : SV_TARGET
{
    return QuadTexture.Sample(LinearSampler, input.uv) * Tint;
}