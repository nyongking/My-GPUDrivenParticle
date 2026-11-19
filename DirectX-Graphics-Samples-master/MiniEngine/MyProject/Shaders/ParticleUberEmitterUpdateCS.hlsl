// 공유 셰이더 진입점: EmitterUpdate (5주차 2단계). 본문은 ParticleUber.hlsli
#include "ParticleUber.hlsli"

[numthreads(1, 1, 1)]
void main()
{
    UberEmitterUpdate();
}
