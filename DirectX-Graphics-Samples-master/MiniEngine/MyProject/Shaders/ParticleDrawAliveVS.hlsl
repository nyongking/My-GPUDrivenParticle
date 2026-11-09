#include "ParticleDrawCommon.hlsli"

StructuredBuffer<uint> AliveList : register(t1);
ByteAddressBuffer Counters : register(t2);

VSOutput main(uint vertexId : SV_VertexID, uint instanceId : SV_InstanceID)
{	    
    // instanceID가 Alive 개수를 넘어선다 -> 뭔가 이상한 상황
    if (instanceId >= Counters.Load(COUNTER_ALIVE))
        return Culled();
    
    uint slot = AliveList[instanceId];      
    
    VSOutput output;
    Particle p = Particles[slot];
    
    float2 corner = kCorners[vertexId];
    float3 worldPos = p.position + (corner.x * CamRight.xyz + corner.y * CamUp.xyz) * p.size;
    
    output.position = mul(ViewProj, float4(worldPos, 1.f));
    output.color = UnpackColor(p.color);
    output.color.a = saturate(p.life / max(p.maxlife, 1e-4));
    output.uv = corner;
    
    return output;
}