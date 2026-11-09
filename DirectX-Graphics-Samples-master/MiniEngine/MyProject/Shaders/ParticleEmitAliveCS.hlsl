#include "ParticleComputeCommon.hlsli"

[numthreads(PARTICLE_GROUP_SIZE, 1, 1)]
void main(uint3 DTid : SV_DispatchThreadID)
{
    if (DTid.x >= EmitCount)
        return;
    
    uint prevDead;
    Counters.InterlockedAdd(COUNTER_DEAD, 0xFFFFFFFF, prevDead); // 1 감소
    if (prevDead == 0 || prevDead > MaxParticles) // 파티클 풀의 범위를 넘어선 경우,
    {
        uint restored;
        Counters.InterlockedAdd(COUNTER_DEAD, 1, restored);
        return;
    }
    
    uint slot = DeadList[prevDead - 1];
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
    Particles[slot] = p;
    
    uint prevAlive, prevEmit;
    Counters.InterlockedAdd(COUNTER_ALIVE, 1, prevAlive);
    AliveIn[prevAlive] = slot;
    
    Counters.InterlockedAdd(COUNTER_EMIT, 1, prevEmit);
   
}