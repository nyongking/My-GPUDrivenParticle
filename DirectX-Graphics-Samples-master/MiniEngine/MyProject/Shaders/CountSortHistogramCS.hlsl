#include "CountSortCommon.hlsli"

StructuredBuffer<RenderParticle> Particles : register(t0);
ByteAddressBuffer CountBuffer : register(t1);
RWStructuredBuffer<uint> Buckets : register(u0);


[numthreads(256, 1, 1)]
void main( uint3 DTid : SV_DispatchThreadID )
{
    const uint count = min(CountBuffer.Load(CountOffset), Capacity);
    const int id = DTid.x;
        
    if (count <= id)
        return;
    
    InterlockedAdd(Buckets[BucketOf(Particles[id].Position)], 1);
        
    
}