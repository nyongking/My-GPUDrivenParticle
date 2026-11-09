#include "ParticleComputeCommon.hlsli"

#ifndef USE_FLAG_ARRAY
#define USE_FLAG_ARRAY 0
#endif

[numthreads(PARTICLE_GROUP_SIZE, 1, 1)]
void main(uint3 DTid : SV_DispatchThreadID)
{
    uint i = DTid.x;
    if (i >= MaxParticles)
        return;
    
#if USE_FLAG_ARRAY
    if (AliveFlags[i] == 0)
        return;
#else  
    if ((Particles[i].flags & PARTICLE_FLAG_ALIVE) == 0)
        return;
#endif
    
    Particle p = Particles[i];
    p.life -= DeltaTime;
    
    // ¼ö¸í ³¡
    if (p.life <= 0.0f)
    {
        Particles[i].flags = 0;
        AliveFlags[i] = 0;
        
        uint prevDead, prevAlive;
        Counters.InterlockedAdd(COUNTER_DEAD, 1, prevDead);
        DeadList[prevDead] = i;
        Counters.InterlockedAdd(COUNTER_ALIVE, 0xFFFFFFFF, prevAlive);
        return;
    }
    
    p.velocity += Gravity * DeltaTime;
    p.position += p.velocity * DeltaTime;
    Particles[i] = p;
}