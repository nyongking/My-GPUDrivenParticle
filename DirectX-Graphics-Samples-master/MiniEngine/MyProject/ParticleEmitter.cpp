#include "pch.h"
#include "ParticleEmitter.h"

uint32_t ParticleEmitter::ComputeEmitCount(float deltaT)
{
	const float want = emitParams.emitRate * deltaT + m_EmitCarry;
	const uint32_t count = (uint32_t)want;
	m_EmitCarry = want - float(count);
	return max(count, 0.f);
}

EmitRequest ParticleEmitter::RequestEmit(float deltaT)
{
	EmitRequest ret = {};
	ret.emitPosition = position;
	ret.emitCount = ComputeEmitCount(deltaT) + ComputeBurstCount(deltaT);
	ret.emitParams = emitParams;

	return ret;
}

uint32_t ParticleEmitter::ComputeBurstCount(float deltaT)
{
	if (emitParams.burstCount == 0 || emitParams.burstInterval <= 0.f)
		return 0;

	uint32_t count = 0;

	if (m_BurstTimer + 1e-4 >= emitParams.burstInterval)
	{
		m_BurstTimer -= emitParams.burstInterval;
		count = emitParams.burstCount;
	}
	m_BurstTimer += deltaT;

	return count;
}

void ParticleEmitter::ResetBurst()
{
	m_BurstTimer = 0;
	m_EmitCarry = 0.f;
}
