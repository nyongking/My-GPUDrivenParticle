// GPU 파티클 정렬 1단계: 깊이 키 목록 만들기 (2단계 = MiniEngine BitonicSort, 64 비트 키/인덱스)
//   출력 = uint2(파티클 번호, 정렬 키). BitonicSort 64 비트 형식 (키 = .y)
//   키를 내림차순으로 정렬하면 카메라에서 먼 것부터 = CPU SortBackToFront 와 같은 순서

cbuffer SortKeyConstants : register(b0)
{
    uint PositionOffset; // 0   Particles.Position 열의 offset (파티클 하나 기준 바이트)
    uint Capacity; // 4   SoA 열 길이
    uint CountOffset; // 8   CountBuffer 안의 alive 위치 (DrawArgs.InstanceCount = 4)
    uint _Pad0; // 12
    float3 CamPos; // 16
    float _Pad1; // 28
    float3 CamForward; // 32
    float _Pad2; // 44
};

ByteAddressBuffer ParticleData : register(t0);
ByteAddressBuffer CountBuffer : register(t1);
RWByteAddressBuffer KeyIndexList : register(u0);

// float -> 크기 순서가 같은 uint (음수 깊이 = 카메라 뒤도 순서가 맞게)
uint SortableKey(float f)
{
    const uint u = asuint(f);
    return (u & 0x80000000u) ? ~u : (u | 0x80000000u);
}

[numthreads(256, 1, 1)]
void main(uint3 dtid : SV_DispatchThreadID)
{
    const uint count = min(CountBuffer.Load(CountOffset), Capacity);
    const uint i = dtid.x;
    if (i >= count)
        return;

    const float3 position = asfloat(ParticleData.Load3(PositionOffset * Capacity + i * 12));
    const float depth = dot(position - CamPos, CamForward);
    KeyIndexList.Store2(i * 8, uint2(i, SortableKey(depth)));
}
