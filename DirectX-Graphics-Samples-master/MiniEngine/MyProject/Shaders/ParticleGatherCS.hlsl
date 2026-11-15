// 이미터 하나의 살아 있는 파티클 -> 공용 렌더 목록 (ParticleRenderList::Add)
//   SoA (이미터마다 다른 레이아웃) 을 RenderParticle 하나의 형식으로 풀고,
//   정렬 목록에 uint2(목록 번호, 깊이 키) 를 쓴다. 자리는 공용 카운터에 웨이브 단위로 원자적으로 더해서 정한다
#include "ParticleListCommon.hlsli"

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
    uint2 _Pad0; // 40
    float4 DefaultColor; // 48
    float3 DefaultPosition; // 64
    float DefaultSize; // 76
    float3 CamPos; // 80
    float DefaultSubImage; // 92
    float3 CamForward; // 96
    float _Pad1; // 108
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

    RenderParticle p;
    p.Position = (PositionOffset == NO_ATTRIBUTE) ? DefaultPosition
        : asfloat(ParticleData.Load3(ColumnAddress(PositionOffset, 12, i)));
    p.Color = (ColorOffset == NO_ATTRIBUTE) ? DefaultColor
        : asfloat(ParticleData.Load4(ColumnAddress(ColorOffset, 16, i)));
    p.Size = (SizeOffset == NO_ATTRIBUTE) ? DefaultSize
        : asfloat(ParticleData.Load(ColumnAddress(SizeOffset, 4, i)));
    p.SubImage = (SubImageOffset == NO_ATTRIBUTE) ? DefaultSubImage
        : asfloat(ParticleData.Load(ColumnAddress(SubImageOffset, 4, i)));
    p.EmitterIndex = EmitterIndex;

    StoreRenderParticle(RenderParticles, slot, p);

    const float depth = dot(p.Position - CamPos, CamForward);
    SortList.Store2(slot * 8, uint2(slot, SortableKey(depth)));
}
