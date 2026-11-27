Texture2D<float4> ParticleColor : register(t0);
SamplerState LinearClamp : register(s0);

struct PSInput
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

float4 main(PSInput input) : SV_Target
{
    return ParticleColor[uint2(input.position.xy)];
}
