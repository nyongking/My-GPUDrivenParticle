#include "CountSortCommon.hlsli"

StructuredBuffer<RenderParticle> Particles : register(t0);
ByteAddressBuffer CountBuffer : register(t1);
RWStructuredBuffer<uint> Buckets : register(u0);
RWByteAddressBuffer SortIndices : register(u1);

[numthreads(256, 1, 1)]
void main( uint3 DTid : SV_DispatchThreadID )
{
    const uint count = min(CountBuffer.Load(CountOffset), Capacity);
    const uint i = DTid.x;
    if (i >= count)
        return;
    
    uint slot;
    InterlockedAdd(Buckets[BucketOf(Particles[i].Position)], 1, slot);
    SortIndices.Store(slot * 4, i);    
}