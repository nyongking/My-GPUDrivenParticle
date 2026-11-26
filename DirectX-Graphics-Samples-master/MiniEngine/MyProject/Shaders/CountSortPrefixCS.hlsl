#include "CountSortCommon.hlsli"

RWStructuredBuffer<uint> Buckets : register(u0);

[numthreads(1, 1, 1)]
void main( uint3 DTid : SV_DispatchThreadID )
{
    uint sum = 0;
    for (uint i = 0; i < BucketCount; ++i)
    {
        const uint val = Buckets[i];
        Buckets[i] = sum;        
        sum += Buckets[i];
    }
    
}