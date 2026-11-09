#include "ParticleComputeCommon.hlsli"

#define COUNTER_EMIT_TICKET COUNTER_DEAD

[numthreads(PARTICLE_GROUP_SIZE, 1, 1)]
void main( uint3 DTid : SV_DispatchThreadID )
{
    uint slot = DTid.x;
    if (slot >= MaxParticles)
        return;

    if ((Particles[slot].flags & PARTICLE_FLAG_ALIVE) != 0)
        return;
    
    if (Counters.Load(COUNTER_EMIT_TICKET) >= EmitCount)
        return;

    uint ticket;
    Counters.InterlockedAdd(COUNTER_EMIT_TICKET, 1, ticket);
    if (ticket >= EmitCount)
        return;

    uint rng = PcgHash(RandomSeed ^ (ticket * 9781u + 1u));

    Particle p;
    p.position = EmitterPos;
    p.velocity = RandDirection(rng) * lerp(MinSpeed, MaxSpeed, Rand01(rng));
    p.life = lerp(MinLife, MaxLife, Rand01(rng));
    p.maxlife = p.life;
    p.color = BaseColor;
    p.size = ParticleSize;
    p.flags = PARTICLE_FLAG_ALIVE;
    p.pad = 0;
    Particles[slot] = p;

    uint prevEmit;
    Counters.InterlockedAdd(COUNTER_EMIT, 1, prevEmit);
}