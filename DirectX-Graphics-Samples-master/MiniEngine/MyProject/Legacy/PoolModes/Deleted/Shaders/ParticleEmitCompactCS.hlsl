#include "ParticleComputeCommon.hlsli"

[numthreads(PARTICLE_GROUP_SIZE, 1, 1)]
void main( uint3 DTid : SV_DispatchThreadID )
{
    if (DTid.x >= EmitCount)
        return;
    
    uint index;
    Counters.InterlockedAdd(COUNTER_ALIVE, 1, index);
    if (index >= MaxParticles)
    {
        uint outNum;
        Counters.InterlockedAdd(COUNTER_ALIVE, 0xFFFFFFFF, outNum);
        return;
    }
    
    uint rng = PcgHash(RandomSeed ^ (DTid.x * 9781u + 1u));

    Particle p;
    p.position = EmitterPos;
    p.velocity = RandDirection(rng) * lerp(MinSpeed, MaxSpeed, Rand01(rng));
    p.life = lerp(MinLife, MaxLife, Rand01(rng));
    p.maxlife = p.life;
    p.color = BaseColor;
    p.size = ParticleSize;
    p.flags = PARTICLE_FLAG_ALIVE;
    p.pad = 0;
    Particles[index] = p;

    uint outNum;
    Counters.InterlockedAdd(COUNTER_EMIT, 1, outNum);
}