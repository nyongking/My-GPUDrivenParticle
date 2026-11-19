#include "ParticleUber.hlsli"

[numthreads(GROUP_SIZE, 1, 1)]
void main(uint3 dtid : SV_DispatchThreadID)
{
    UberParticleSpawn(dtid);
}
