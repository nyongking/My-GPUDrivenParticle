cbuffer FrameConstants : register(b0)
{
    float4x4 ViewProj;
};

cbuffer ObjectConstants : register(b1)
{
    float4x4 World;
}

struct VSInput
{
    float3 position : POSITION;
    float2 uv : TEXCOORD0;
};

struct VSOutput
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

VSOutput main(VSInput input)
{
    VSOutput output;
    
    float4 worldPos = mul(World, float4(input.position, 1.f));
    output.position = mul(ViewProj, worldPos);
    output.uv = input.uv;
      
    
    return output;
}