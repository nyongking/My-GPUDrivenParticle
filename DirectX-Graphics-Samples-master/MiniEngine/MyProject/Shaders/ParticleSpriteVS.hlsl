cbuffer SpriteConstants : register(b0)
{
    float4x4 ViewProj;
    float4 CamRight;
    float4 CamUp;

    uint PositionOffset; // Particles.Position 의 offset (파티클 하나 기준 바이트)
    uint ColorOffset; // Particles.Color
    uint SizeOffset; // Particles.Size
    uint Capacity; // 최대 파티클 수 (SoA 열 길이)
    
    float4 DefaultColor;
    float3 DefaultPosition;
    float DefaultSize;
    
    uint SortEnabled; // 1 = sort, 0 = not sorted
    uint SubImageOffset; // SubImage의 Index
    float DefaultSubImage; // 152
    uint SubUVBlend; // 156 1 = 다음 프레임과 섞기
    
    float2 SubImageSize; // 160 (가로 칸, 세로 칸)
    float2 _pad; // 168
};

#define NO_ATTRIBUTE 0xFFFFFFFF

ByteAddressBuffer ParticleData : register(t0);
StructuredBuffer<uint> SortedIndices : register(t1);


struct VSOutput
{
    float4 position : SV_Position;
    float4 color : COLOR0;
    float4 uv : TEXCOORD0; // xy = 현재 프레임 uv, zw = 다음 프레임 uv
    nointerpolation float blend : TEXCOORD1; // 다음 프레임 비율 (0 = 현재 프레임만)
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


// SubUV: 프레임 번호 -> 격자 칸 안의 uv (왼쪽 위 칸부터 가로로 0, 1, 2 ...)
float2 SubImageUV(uint frame, float2 uv)
{
    const uint cols = (uint) SubImageSize.x;
    return (float2(frame % cols, frame / cols) + uv) / SubImageSize;
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
    const float subImage = (SubImageOffset == NO_ATTRIBUTE) ? DefaultSubImage
        : asfloat(ParticleData.Load(ColumnAddress(SubImageOffset, 4, idx)));

    const float2 corner = kCorners[vertexId];
    const float3 worldPos = position + (corner.x * CamRight.xyz + corner.y * CamUp.xyz) * size;

    VSOutput output;
    output.position = mul(ViewProj, float4(worldPos, 1.f));
    output.color = color;
    const float2 uv = corner * float2(0.5f, -0.5f) + 0.5f; // 왼쪽 위 (0,0) ~ 오른쪽 아래 (1,1)

    // SubUV: 정수부 = 프레임, 소수부 = 다음 프레임으로 넘어가는 비율. 칸 수를 넘으면 처음으로 돈다
    const uint total = (uint) SubImageSize.x * (uint) SubImageSize.y;
    const float index = max(subImage, 0.f);
    const uint frame0 = (uint) index % total;
    const uint frame1 = (frame0 + 1) % total;
    output.uv = float4(SubImageUV(frame0, uv), SubImageUV(frame1, uv));
    output.blend = SubUVBlend ? frac(index) : 0.f;
    return output;
}