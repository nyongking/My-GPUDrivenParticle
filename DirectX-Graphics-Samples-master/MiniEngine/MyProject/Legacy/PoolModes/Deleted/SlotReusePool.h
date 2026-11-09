#pragma once
#include "ParticlePool.h"

#include "GpuBuffer.h"
#include "ReadbackBuffer.h"


class SlotReusePool : public ParticlePool
{
public:
	SlotReusePool(Particles::Mode mode) : ParticlePool(mode) {  }

public:
	void Initialize(uint32_t maxParticles) override;
	void CreateBuffers(uint32_t maxParticles) override;
	void Shutdown() override;

	void Update(float deltaT, const EmitRequest& emitRequest) override;
	void Render(GraphicsContext& gfx, const Math::Camera& cam) override;

	Particles::CounterSnapshot GetCounterSnapshot() const { return m_LastCounters; }

private:
#ifndef RELEASE
	void		 CollectCounterReadback();
#endif
	bool		 VerifyInitialState(); // Initialize시에 Buffer 설정이 정확한지 확인하는 함수

	static constexpr uint32_t sThreadGroupSize = 64;
		

	// -----------------------
	//			buffer
	// -----------------------
	StructuredBuffer	m_ParticleBuffer;
	StructuredBuffer	m_DeadListBuffer;
	ByteAddressBuffer	m_CounterBuffer;
	StructuredBuffer	m_AliveFlagBuffer; // For Slot Reuse 'Flag' Version

	// -----------------------
	//		Readback buffer
	// -----------------------
	ReadbackBuffer		m_CounterReadback[sReadbackLatency];
	uint64_t			m_ReadbackFence[sReadbackLatency] = {};
	uint32_t			m_ReadbackSlot = 0;

	Particles::CounterSnapshot m_LastCounters = {};

	bool				m_initVerified = false;
};

