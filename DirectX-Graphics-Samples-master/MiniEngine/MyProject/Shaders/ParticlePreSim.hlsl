#include "ParticleComputeCommon.hlsli"

[numthreads(1, 1, 1)]
void main( uint3 DTid : SV_DispatchThreadID )
{
    uint alive = Counters.Load(COUNTER_ALIVE);
    uint groups = (alive + PARTICLE_GROUP_SIZE - 1) / PARTICLE_GROUP_SIZE;
    IndirectArgs.Store3(PARTICLE_ARGS_SIMULATE_DISPATCH, uint3(groups, 1, 1));

}