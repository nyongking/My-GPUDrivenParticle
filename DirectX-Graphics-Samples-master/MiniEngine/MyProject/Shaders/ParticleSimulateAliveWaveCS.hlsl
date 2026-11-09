#include "ParticleComputeCommon.hlsli"

[numthreads(PARTICLE_GROUP_SIZE, 1, 1)]
void main( uint3 DTid : SV_DispatchThreadID )
{
    uint i = DTid.x;
    bool valid = i < Counters.Load(COUNTER_ALIVE);
    
    uint slot = 0;
    bool survived = false;
    bool died = false;

    if (valid)
    {
        slot = AliveIn[i];
        Particle p = Particles[slot];
        p.life -= DeltaTime;
        
        if (p.life <= 0.0f)
        {
            p.life = 0.f;
            p.flags = 0;
            died = true;
        }
        else
        {
            p.velocity += Gravity * DeltaTime;
            p.position += p.velocity * DeltaTime;
            survived = true;
        }
        
        Particles[slot] = p;
    }
    
    uint waveAlive = WaveActiveCountBits(survived);
    uint aliveRank = WavePrefixCountBits(survived);
    uint aliveBase = 0;
    if (waveAlive > 0 && WaveIsFirstLane())
        Counters.InterlockedAdd(COUNTER_ALIVE_AFTER_SIM, waveAlive, aliveBase);
    aliveBase = WaveReadLaneFirst(aliveBase);
    if (survived)
        AliveOut[aliveBase + aliveRank] = slot;
    
    uint waveDead = WaveActiveCountBits(died);
    uint deadRank = WavePrefixCountBits(died);
    uint deadBase = 0;
    if (waveDead > 0 && WaveIsFirstLane())
        Counters.InterlockedAdd(COUNTER_DEAD, waveDead, deadBase);
    deadBase = WaveReadLaneFirst(deadBase);
    if (died)
        DeadList[deadBase + deadRank] = slot;

}