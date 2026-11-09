#define GROUP_SIZE 64

cbuffer EngineConstants : register(b0)
{
    float3  OwnerPosition;
    float   DeltaTime;
    uint    Capacity;
    uint    RandomSeed;
    uint2   _pad;
};

RWByteAddressBuffer ParticlesIn : register(u0);
RWByteAddressBuffer ParticlesOut : register(u1);
RWByteAddressBuffer Counters : register(u2);
RWByteAddressBuffer EmitterData : register(u3);
RWByteAddressBuffer IndirectArgs : register(u4);

// Counters에서 읽는 주소 공간
#define COUNTER_ALIVE_IN      0   // ParticlesIn 에 들어 있는 개수
#define COUNTER_WRITE_CURSOR  4   // ParticlesOut 에 다음으로 쓸 자리 (InterlockedAdd)
#define COUNTER_SPAWN_REQUEST 8   // 이번 프레임 생성 요청 수 (EmitterUpdate 가 씀)

uint ColumnAddress(uint attrOffset, uint typeSize, uint idx)
{
    return attrOffset * Capacity + idx * typeSize;
}

float LoadF1(RWByteAddressBuffer buf, uint off, uint idx)
{
    return asfloat(buf.Load(ColumnAddress(off, 4, idx)));
}

float2 LoadF2(RWByteAddressBuffer b, uint off, uint idx)
{
    return asfloat(b.Load2(ColumnAddress(off, 8, idx)));
}

float3 LoadF3(RWByteAddressBuffer b, uint off, uint idx)
{
    return asfloat(b.Load3(ColumnAddress(off, 12, idx)));
}

float4 LoadF4(RWByteAddressBuffer b, uint off, uint idx)
{
    return asfloat(b.Load4(ColumnAddress(off, 16, idx)));
}

uint LoadU1(RWByteAddressBuffer b, uint off, uint idx)
{
    return asuint(b.Load(ColumnAddress(off, 4, idx)));
}

int LoadI1(RWByteAddressBuffer b, uint off, uint idx)
{
    return asint(b.Load(ColumnAddress(off, 4, idx)));
}

void StoreF1(RWByteAddressBuffer b, uint off, uint idx, float v)
{
    b.Store(ColumnAddress(off, 4, idx), asuint(v));
}

void StoreF2(RWByteAddressBuffer b, uint off, uint idx, float2 v)
{
    b.Store2(ColumnAddress(off, 8, idx), asuint(v));
}

void StoreF3(RWByteAddressBuffer b, uint off, uint idx, float3 v)
{
    b.Store3(ColumnAddress(off, 12, idx), asuint(v));
}

void StoreF4(RWByteAddressBuffer b, uint off, uint idx, float4 v)
{
    b.Store4(ColumnAddress(off, 16, idx), asuint(v));
}

void StoreU1(RWByteAddressBuffer b, uint off, uint idx, uint v)
{
    b.Store(ColumnAddress(off, 4, idx), v);
}

void StoreI1(RWByteAddressBuffer b, uint off, uint idx, int v)
{
    b.Store(ColumnAddress(off, 4, idx), asuint(v));
}

uint Hash(uint x)
{
    x ^= x >> 16;
    x *= 0x7feb352dU;
    x ^= x >> 15;
    x *= 0x846ca68bU;
    x ^= x >> 16;
    return x;
}

float Random01(uint x, uint salt)
{
    const uint h = Hash(RandomSeed ^ Hash(x * 0x9E3779B9U + salt));
    return float(h >> 8) * (1.0f / 16777216.0f); // 상위 24비트 -> [0, 1)
}

#define KILL() (alive = false)
