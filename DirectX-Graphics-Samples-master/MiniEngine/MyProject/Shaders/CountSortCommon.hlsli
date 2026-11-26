#include "RenderParticle.hlsli"

cbuffer CountSortConstants : register(b0)
{
    uint Capacity;
    uint CountOffset;
    uint BucketCount;
    uint _Pad;
    float3 CamPos;
    float DepthMin;
    float3 CamForward;
    float DepthScale;
}

uint BucketOf(float3 position)
{
    float depth = dot(position - CamPos, normalize(CamForward));
    
    float f = clamp((depth - DepthMin) * DepthScale, 0.f, float(BucketCount - 1));
    return (BucketCount - 1) - uint(f); // 먼것부터 0번   

}