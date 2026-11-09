#pragma once
#include "ParticlePool.h"

#include "GpuBuffer.h"
#include "ReadbackBuffer.h"

class FullScanPool : public ParticlePool
{
public:
	FullScanPool() : ParticlePool(Particles::Mode::Default) {}


public:
	void Initialize(uint32_t maxParticles) override;
	void CreateBuffers(uint32_t maxParticles) override;
	void Shutdown() override;

	void Update(float deltaT, const EmitRequest& emitRequest) override;
	void Render(GraphicsContext& gfx, const Math::Camera& cam) override;

	Particles::CounterSnapshot GetCounterSnapshot()
	{
		Particles::CounterSnapshot s = m_LastCounters;
		s.deadCount = m_MaxParticles - s.aliveCount;
		return s;
	}

private:
#ifndef RELEASE
	void	CollectCounterReadback();
#endif

	static constexpr uint32_t sThreadGroupSize = 64;

	// -----------------------
	//			buffer
	// -----------------------
	StructuredBuffer	m_ParticleBuffer;
	ByteAddressBuffer	m_CounterBuffer;

	// -----------------------
	//		Readback buffer
	// -----------------------
	ReadbackBuffer		m_CounterReadback[sReadbackLatency];
	uint64_t			m_ReadbackFence[sReadbackLatency] = {};
	uint32_t			m_ReadbackSlot = 0;

	Particles::CounterSnapshot m_LastCounters = {};
};

