#pragma once

namespace Particles
{
	inline uint32_t PackColor(uint8_t r, uint8_t g, uint8_t b, uint8_t a)
	{
		return uint32_t(r) | (uint32_t(g) << 8) | (uint32_t(b) << 16) | (uint32_t(a) << 24);
	}

	uint32_t HashU32(uint32_t x);

	float ComputeEmitRateByAliveRatio(float aliveRatio, float minLife, float maxLife, uint32_t maxParticles);
}
