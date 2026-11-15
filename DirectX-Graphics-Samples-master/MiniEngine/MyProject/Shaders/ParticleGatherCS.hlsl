// 이미터 하나의 살아 있는 파티클 -> 공용 렌더 목록 (ParticleRenderList::Add)
//   SoA (이미터마다 다른 레이아웃) 을 RenderParticle 하나의 형식으로 풀고,
//   정렬 목록에 uint2(목록 번호, 깊이 키) 를 쓴다. 자리는 공용 카운터에 웨이브 단위로 원자적으로 더해서 정한다
//   빌보드 방향(Facing / Alignment / 회전 / 피벗)도 여기서 계산해서 축 벡터로 넘긴다
#include "ParticleListCommon.hlsli"
#include "SpriteFacing.hlsli"

// C++ GatherConstants 와 같은 배치 (224 B)
cbuffer GatherConstants : register(b0)
{
    uint PositionOffset; // 0   속성 열 offset (파티클 하나 기준 바이트), NO_ATTRIBUTE = 기본값
    uint ColorOffset; // 4
    uint SizeOffset; // 8
    uint SubImageOffset; // 12
    uint Capacity; // 16  이미터 SoA 열 길이
    uint CountOffset; // 20  CountBuffer 안의 개수 위치
    uint ConstCount; // 24  CountBuffer 가 없을 때 (CPU 이미터)
    uint UseCountBuffer; // 28
    uint EmitterIndex; // 32
    uint ListCapacity; // 36  공용 목록 크기
    uint SpriteSizeOffset; // 40  연결이 없으면 (Size * 2, Size * 2)
    uint SpriteRotationOffset; // 44
    float4 DefaultColor; // 48
    float3 DefaultPosition; // 64
    float DefaultSize; // 76
    float3 CamPos; // 80
    float DefaultSubImage; // 92
    float3 CamForward; // 96
    float DefaultSpriteRotation; // 108
    float3 CamRight; // 112
    uint VelocityOffset; // 124
    float3 CamUp; // 128
    uint SpriteFacingOffset; // 140
    float3 DefaultVelocity; // 144
    uint SpriteAlignmentOffset; // 156
    float3 DefaultSpriteFacing; // 160
    uint FacingMode; // 172
    float3 DefaultSpriteAlignment; // 176
    uint AlignmentMode; // 188
    float2 Pivot; // 192
    float CameraOffset; // 200
    float VelocityStretch; // 204
    float BlendNear; // 208
    float BlendFar; // 212
    float2 _Pad0; // 216
};

ByteAddressBuffer ParticleData : register(t0);
ByteAddressBuffer CountBuffer : register(t1);
RWByteAddressBuffer RenderParticles : register(u0);
RWByteAddressBuffer SortList : register(u1);
RWByteAddressBuffer Counter : register(u2);

// float -> 크기 순서가 같은 uint (음수 깊이 = 카메라 뒤도 순서가 맞게)
uint SortableKey(float f)
{
    const uint u = asuint(f);
    return (u & 0x80000000u) ? ~u : (u | 0x80000000u);
}

uint ColumnAddress(uint attrOffset, uint typeSize, uint index)
{
    return attrOffset * Capacity + index * typeSize;
}

float  ReadF1(uint ofs, uint i, float  def) { return (ofs == NO_ATTRIBUTE) ? def : asfloat(ParticleData.Load(ColumnAddress(ofs, 4, i))); }
float2 ReadF2(uint ofs, uint i, float2 def) { return (ofs == NO_ATTRIBUTE) ? def : asfloat(ParticleData.Load2(ColumnAddress(ofs, 8, i))); }
float3 ReadF3(uint ofs, uint i, float3 def) { return (ofs == NO_ATTRIBUTE) ? def : asfloat(ParticleData.Load3(ColumnAddress(ofs, 12, i))); }
float4 ReadF4(uint ofs, uint i, float4 def) { return (ofs == NO_ATTRIBUTE) ? def : asfloat(ParticleData.Load4(ColumnAddress(ofs, 16, i))); }

[numthreads(256, 1, 1)]
void main(uint3 dtid : SV_DispatchThreadID)
{
    const uint count = min(UseCountBuffer ? CountBuffer.Load(CountOffset) : ConstCount, Capacity);
    const uint i = dtid.x;
    const bool valid = i < count;

    // 웨이브에서 살아 있는 파티클 수만큼 한 번에 자리를 받는다 (return 전에 웨이브 전체가 같이 실행)
    const uint waveCount = WaveActiveCountBits(valid);
    uint waveBase = 0;
    if (WaveIsFirstLane() && waveCount > 0)
        Counter.InterlockedAdd(0, waveCount, waveBase);
    waveBase = WaveReadLaneFirst(waveBase);

    if (!valid)
        return;

    const uint slot = waveBase + WavePrefixCountBits(valid);
    if (slot >= ListCapacity)
        return; // 목록이 꽉 참 (Reserve 크기 = capacity 합이라 정상이면 오지 않음)

    const float size = ReadF1(SizeOffset, i, DefaultSize);

    SpriteShape s;
    s.Position = ReadF3(PositionOffset, i, DefaultPosition);
    s.Size = ReadF2(SpriteSizeOffset, i, float2(size, size) * 2.f);
    s.Rotation = ReadF1(SpriteRotationOffset, i, DefaultSpriteRotation);
    s.Velocity = ReadF3(VelocityOffset, i, DefaultVelocity);
    s.Facing = ReadF3(SpriteFacingOffset, i, DefaultSpriteFacing);
    s.Alignment = ReadF3(SpriteAlignmentOffset, i, DefaultSpriteAlignment);

    SpriteOptions o;
    o.Facing = FacingMode;
    o.Alignment = AlignmentMode;
    o.Pivot = Pivot;
    o.CameraOffset = CameraOffset;
    o.VelocityStretch = VelocityStretch;
    o.BlendNear = BlendNear;
    o.BlendFar = BlendFar;

    SpriteCamera cam;
    cam.Position = CamPos;
    cam.Right = CamRight;
    cam.Up = CamUp;
    cam.Forward = CamForward;

    RenderParticle p;
    ComputeSpriteAxes(s, o, cam, p.Center, p.AxisX, p.AxisY);
    p.Color = ReadF4(ColorOffset, i, DefaultColor);
    p.SubImage = ReadF1(SubImageOffset, i, DefaultSubImage);
    p.EmitterIndex = EmitterIndex;

    StoreRenderParticle(RenderParticles, slot, p);

    // 깊이는 파티클 위치 기준 (피벗 / 카메라 오프셋과 상관없이 순서가 같게)
    const float depth = dot(s.Position - CamPos, CamForward);
    SortList.Store2(slot * 8, uint2(slot, SortableKey(depth)));
}
