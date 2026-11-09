#include "ParticleComputeCommon.hlsli"

RWStructuredBuffer<Particle> ParticlesOut : register(u7);

[numthreads(PARTICLE_GROUP_SIZE, 1, 1)]
void main( uint3 DTid : SV_DispatchThreadID )
{
    uint i = DTid.x;
    bool survived = false;
    Particle p = (Particle) 0;
 
    if (i < Counters.Load(COUNTER_ALIVE))
    {
        p = Particles[i];
        p.life -= DeltaTime;
        if (p.life > 0.f)
        {
            p.velocity += Gravity * DeltaTime;
            p.position += p.velocity * DeltaTime;
            survived = true;
        }
    }
    
    uint waveAlive = WaveActiveCountBits(survived);
    uint aliveRank = WavePrefixCountBits(survived);
    uint aliveBase = 0;
    if (waveAlive > 0 && WaveIsFirstLane())
        Counters.InterlockedAdd(COUNTER_ALIVE_AFTER_SIM, waveAlive, aliveBase);
    aliveBase = WaveReadLaneFirst(aliveBase);
    
    if (survived)
        ParticlesOut[aliveBase + aliveRank] = p;
}