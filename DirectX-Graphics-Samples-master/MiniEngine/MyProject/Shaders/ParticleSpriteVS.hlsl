cbuffer SpriteConstants : register(b0)
{
    float4x4 ViewProj;
    float4 CamRight;
    float4 CamUp;

    uint PositionOffset; // Particles.Position 의 offset (파티클 하나 기준 바이트)
    uint ColorOffset; // Particles.Color
    uint SizeOffset; // Particles.Size
    uint Capacity; // 최대 파티클 수 (SoA 열 길이)
    
    float4 DefaultColor; // 112
    float3 DefaultPosition; // 128
    float DefaultSize; // 140
    
    uint SortEnabled; // 1 = sort, 0 = not sorted
    uint3 _pad;
};

#define NO_ATTRIBUTE 0xFFFFFFFF

ByteAddressBuffer ParticleData : register(t0);
StructuredBuffer<uint> SortedIndices : register(t1);


struct VSOutput
{
    float4 position : SV_Position;
    float4 color : COLOR0;
    float2 uv : TEXCOORD0;
};

static const float2 kCorners[6] =
{
    float2(-1.f, 1.f), float2(1.f, 1.f), float2(1.f, -1.f),
    float2(-1.f, 1.f), float2(1.f, -1.f), float2(-1.f, -1.f)
};

// SoA 주소: 속성 열 시작 + 파티클 번호 * 타입 크기
uint ColumnAddress(uint attrOffset, uint typeSize, uint particleIndex)
{
    return attrOffset * Capacity + particleIndex * typeSize;
}


VSOutput main(uint vertexId : SV_VertexID, uint instanceId : SV_InstanceID)
{
    const uint idx = SortEnabled ? SortedIndices[instanceId] : instanceId;
    
    // 칸이 연결되어 있으면 버퍼에서, 아니면 기본값 (cbuffer 값이라 draw 전체가 같은 쪽으로 분기)
    const float3 position = (PositionOffset == NO_ATTRIBUTE) ? DefaultPosition
        : asfloat(ParticleData.Load3(ColumnAddress(PositionOffset, 12, idx)));
    const float4 color = (ColorOffset == NO_ATTRIBUTE) ? DefaultColor
        : asfloat(ParticleData.Load4(ColumnAddress(ColorOffset, 16, idx)));
    const float size = (SizeOffset == NO_ATTRIBUTE) ? DefaultSize
        : asfloat(ParticleData.Load(ColumnAddress(SizeOffset, 4, idx)));

    const float2 corner = kCorners[vertexId];
    const float3 worldPos = position + (corner.x * CamRight.xyz + corner.y * CamUp.xyz) * size;

    VSOutput output;
    output.position = mul(ViewProj, float4(worldPos, 1.f));
    output.color = color;
    output.uv = corner;
    return output;
}