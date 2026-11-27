struct VSOutput
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

VSOutput main(uint vertexId : SV_VertexID)
{
    VSOutput output;
    output.uv = float2(uint2(vertexId, vertexId << 1) & 2); // (0,0), (0,2), (2,0) 
    output.position = float4(lerp(float2(-1.f, 1.f), float2(1.f, -1.f), output.uv), 0.f, 1.f); // (-1, 1), (-1, -3), (3, 1)
    return output;
}
