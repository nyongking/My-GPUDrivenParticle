#include "ParticleDrawCommon.hlsli"

#ifndef USE_FLAG_ARRAY
#define USE_FLAG_ARRAY 0
#endif


StructuredBuffer<uint> AliveFlags : register(t1);

VSOutput main(uint vertexId : SV_VertexID, uint instanceId : SV_InstanceID)
{
#if USE_FLAG_ARRAY
    const bool alive = AliveFlags[instanceId] != 0;
#else    
    const bool alive = (Particles[instanceId].flags & PARTICLE_FLAG_ALIVE) != 0;
#endif  
    // Particle이 살아있지 않은 경우 -> Rasterizer에서 버리도록한다.
    if (!alive)
    {
        return Culled();
    }
    
    VSOutput output;
    Particle p = Particles[instanceId];
    
    float2 corner = kCorners[vertexId];
    float3 worldPos = p.position + (corner.x * CamRight.xyz + corner.y * CamUp.xyz) * p.size;
    
    output.position = mul(ViewProj, float4(worldPos, 1.f));
    output.color = UnpackColor(p.color);
    output.color.a = saturate(p.life / max(p.maxlife, 1e-4));
    output.uv = corner;
    
    return output;
}