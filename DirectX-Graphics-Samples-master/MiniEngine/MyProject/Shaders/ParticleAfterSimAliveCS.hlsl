#include "ParticleComputeCommon.hlsli"

[numthreads(1, 1, 1)]
void main( uint3 DTid : SV_DispatchThreadID )
{
    uint survived = Counters.Load(COUNTER_ALIVE_AFTER_SIM);
    Counters.Store(COUNTER_ALIVE, survived);
    IndirectArgs.Store4(PARTICLE_ARGS_DRAW, uint4(6, survived, 0, 0));

}