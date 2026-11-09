#include "ParticleDrawCommon.hlsli"
VSOutput main(uint vertexId : SV_VertexID, uint instanceId : SV_InstanceID)
{
    Particle p = Particles[instanceId];
    
    VSOutput output;
    float2 corner = kCorners[vertexId];
    float3 worldPos = p.position + (corner.x * CamRight.xyz + corner.y * CamUp.xyz) * p.size;
    
    output.position = mul(ViewProj, float4(worldPos, 1.f));
    output.color = UnpackColor(p.color);
    output.color.a = saturate(p.life / max(p.maxlife, 1e-4));
    output.uv = corner;
    
    return output;
}