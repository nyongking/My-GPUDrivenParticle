#include "ParticleComputeCommon.hlsli"

[numthreads(PARTICLE_GROUP_SIZE, 1, 1)]
void main(uint3 DTid : SV_DispatchThreadID)
{
    uint i = DTid.x;
    bool survived = false;

    if (i < MaxParticles)
    {
        if ((Particles[i].flags & PARTICLE_FLAG_ALIVE) != 0)
        {
            Particle p = Particles[i];
            p.life -= DeltaTime;

            if (p.life <= 0.0f)
            {
                Particles[i].flags = 0;
            }
            else
            {
                p.velocity += Gravity * DeltaTime;
                p.position += p.velocity * DeltaTime;
                Particles[i] = p;
                survived = true;
                
                uint origin;
                Counters.InterlockedAdd(COUNTER_ALIVE, 1, origin);
            }
        }
    }
}
