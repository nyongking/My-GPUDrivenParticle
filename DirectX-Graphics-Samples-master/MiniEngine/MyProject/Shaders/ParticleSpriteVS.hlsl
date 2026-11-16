#include "SpriteFacing.hlsli"

#define NO_ATTRIBUTE 0xFFFFFFFF

// C++ SpriteConstants 와 같은 배치 (320 B)
cbuffer SpriteConstants : register(b0)
{
    float4x4 ViewProj; // 0
    float4 CamRight; // 64
    float4 CamUp; // 80
    float4 CamPosition; // 96
    float4 CamForward; // 112

    uint PositionOffset; // 128 Particles.Position 의 offset (파티클 하나 기준 바이트)
    uint ColorOffset; // 132 Particles.Color
    uint SizeOffset; // 136 Particles.Size
    uint Capacity; // 140 최대 파티클 수 (SoA 열 길이)

    float4 DefaultColor; // 144
    float3 DefaultPosition; // 160
    float DefaultSize; // 172

    uint SortEnabled; // 176 1 = sort, 0 = not sorted
    uint SubImageOffset; // 180 SubImage의 Index
    float DefaultSubImage; // 184
    uint SubUVBlend; // 188 1 = 다음 프레임과 섞기

    float2 SubImageSize; // 192 (가로 칸, 세로 칸)
    uint SortStride; // 200 정렬 목록 한 칸 바이트 (CPU 4 = uint index / GPU 8 = uint2(index, key))
    uint SpriteSizeOffset; // 204 연결이 없으면 (Size * 2, Size * 2)

    uint SpriteRotationOffset; // 208
    float DefaultSpriteRotation; // 212
    uint VelocityOffset; // 216
    uint SpriteFacingOffset; // 220

    float3 DefaultVelocity; // 224
    uint SpriteAlignmentOffset; // 236
    float3 DefaultSpriteFacing; // 240
    uint FacingMode; // 252
    float3 DefaultSpriteAlignment; // 256
    uint AlignmentMode; // 268

    float2 Pivot; // 272
    float CameraOffset; // 280
    float VelocityStretch; // 284
    float BlendNear; // 288
    float BlendFar; // 292
    uint Param0Offset; // 296 MaterialParam0 바인딩 (연결이 없으면 DefaultParam0)
    float DefaultParam0; // 300
    uint Param1Offset; // 304 MaterialParam1
    float DefaultParam1; // 308
    float2 _Pad; // 312
};

ByteAddressBuffer ParticleData : register(t0);
ByteAddressBuffer SortedIndices : register(t1); // 칸의 첫 uint = 파티클 번호 (GPU 는 뒤에 키가 붙음)


struct VSOutput
{
    float4 position : SV_Position;
    float4 color : COLOR0;
    float4 uv : TEXCOORD0; // xy = 현재 프레임 uv, zw = 다음 프레임 uv
    nointerpolation float blend : TEXCOORD1; // 다음 프레임 비율 (0 = 현재 프레임만)
    float2 quadUV : TEXCOORD2; // 사각형 uv (SubUV 와 무관, 디졸브 마스크용)
    nointerpolation float2 params : TEXCOORD3; // MaterialParam0 / 1 (레이어 세기로 바꾸는 것은 PS)
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

// 칸이 연결되어 있으면 버퍼에서, 아니면 기본값 (cbuffer 값이라 draw 전체가 같은 쪽으로 분기)
float  ReadF1(uint ofs, uint idx, float  def) { return (ofs == NO_ATTRIBUTE) ? def : asfloat(ParticleData.Load(ColumnAddress(ofs, 4, idx))); }
float2 ReadF2(uint ofs, uint idx, float2 def) { return (ofs == NO_ATTRIBUTE) ? def : asfloat(ParticleData.Load2(ColumnAddress(ofs, 8, idx))); }
float3 ReadF3(uint ofs, uint idx, float3 def) { return (ofs == NO_ATTRIBUTE) ? def : asfloat(ParticleData.Load3(ColumnAddress(ofs, 12, idx))); }
float4 ReadF4(uint ofs, uint idx, float4 def) { return (ofs == NO_ATTRIBUTE) ? def : asfloat(ParticleData.Load4(ColumnAddress(ofs, 16, idx))); }

// SubUV: 프레임 번호 -> 격자 칸 안의 uv (왼쪽 위 칸부터 가로로 0, 1, 2 ...)
float2 SubImageUV(uint frame, float2 uv)
{
    const uint cols = (uint) SubImageSize.x;
    return (float2(frame % cols, frame / cols) + uv) / SubImageSize;
}

VSOutput main(uint vertexId : SV_VertexID, uint instanceId : SV_InstanceID)
{
    const uint idx = SortEnabled ? SortedIndices.Load(instanceId * SortStride) : instanceId;

    const float4 color = ReadF4(ColorOffset, idx, DefaultColor);
    const float size = ReadF1(SizeOffset, idx, DefaultSize);
    const float subImage = ReadF1(SubImageOffset, idx, DefaultSubImage);

    SpriteShape s;
    s.Position = ReadF3(PositionOffset, idx, DefaultPosition);
    s.Size = ReadF2(SpriteSizeOffset, idx, float2(size, size) * 2.f);
    s.Rotation = ReadF1(SpriteRotationOffset, idx, DefaultSpriteRotation);
    s.Velocity = ReadF3(VelocityOffset, idx, DefaultVelocity);
    s.Facing = ReadF3(SpriteFacingOffset, idx, DefaultSpriteFacing);
    s.Alignment = ReadF3(SpriteAlignmentOffset, idx, DefaultSpriteAlignment);

    SpriteOptions o;
    o.Facing = FacingMode;
    o.Alignment = AlignmentMode;
    o.Pivot = Pivot;
    o.CameraOffset = CameraOffset;
    o.VelocityStretch = VelocityStretch;
    o.BlendNear = BlendNear;
    o.BlendFar = BlendFar;

    SpriteCamera cam;
    cam.Position = CamPosition.xyz;
    cam.Right = CamRight.xyz;
    cam.Up = CamUp.xyz;
    cam.Forward = CamForward.xyz;

    float3 center, axisX, axisY;
    ComputeSpriteAxes(s, o, cam, center, axisX, axisY);

    const float2 corner = kCorners[vertexId];
    const float3 worldPos = center + corner.x * axisX + corner.y * axisY;

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
    output.quadUV = uv;
    output.params = float2(ReadF1(Param0Offset, idx, DefaultParam0), ReadF1(Param1Offset, idx, DefaultParam1));
    return output;
}
