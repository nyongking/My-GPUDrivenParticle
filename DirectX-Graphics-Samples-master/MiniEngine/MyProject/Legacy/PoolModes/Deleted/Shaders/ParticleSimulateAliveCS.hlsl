#include "ParticleComputeCommon.hlsli"

[numthreads(PARTICLE_GROUP_SIZE, 1, 1)]
void main( uint3 DTid : SV_DispatchThreadID )
{
    uint i = DTid.x;
    if (i >= Counters.Load(COUNTER_ALIVE))
        return;
    
    uint slot = AliveIn[i];
    Particle p = Particles[slot];
    p.life -= DeltaTime;
    
    if (p.life <= 0.0f)
    {
        p.life = 0.f;
        p.flags = 0;
        Particles[slot] = p;

        uint prevDead;
        Counters.InterlockedAdd(COUNTER_DEAD, 1, prevDead);
        DeadList[prevDead] = slot;
        return;
    }
            
    p.velocity += Gravity * DeltaTime;
    p.position += p.velocity * DeltaTime;
    Particles[slot] = p;

    uint outIndex;
    Counters.InterlockedAdd(COUNTER_ALIVE_AFTER_SIM, 1, outIndex);
    AliveOut[outIndex] = slot;
}