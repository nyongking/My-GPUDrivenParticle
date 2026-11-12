// 공유 셰이더 진입점: ParticleUpdate (5주차 2단계). 본문은 ParticleUber.hlsli
#include "ParticleUber.hlsli"

[numthreads(GROUP_SIZE, 1, 1)]
void main(uint3 dtid : SV_DispatchThreadID)
{
    UberParticleUpdate(dtid);
}
