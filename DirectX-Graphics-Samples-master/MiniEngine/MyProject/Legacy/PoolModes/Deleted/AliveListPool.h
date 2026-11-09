#pragma once
#include "ParticlePool.h"

#include "GpuBuffer.h"
#include "ReadbackBuffer.h"

class AliveListPool : public ParticlePool
{
public:
	AliveListPool(Particles::Mode mode = Particles::Mode::AliveDirect)
		: ParticlePool(mode) {}

public:
	void Initialize(uint32_t maxParticles) override;
	void CreateBuffers(uint32_t maxParticles) override;
	void Shutdown() override;

	void Update(float deltaT, const EmitRequest& emitRequest) override;
	void Render(GraphicsContext& gfx, const Math::Camera& cam) override;

	Particles::CounterSnapshot GetCounterSnapshot() const { return m_LastCounters; }

private:
#ifndef RELEASE
	void	CollectCounterReadback();
#endif
	bool	VerifyInitialState();
	void	InsertUAVBarriers(ComputeContext& ctx);

	bool	IsIndirect() const { 
		return m_Mode == Particles::Mode::AliveIndirect || m_Mode == Particles::Mode::AliveIndirectWave; 
	}
	StructuredBuffer& AliveIn() { return m_AliveListBuffer[m_AliveCurrent]; }
	StructuredBuffer& AliveOut() { return m_AliveListBuffer[1 - m_AliveCurrent]; }

	static constexpr uint32_t sThreadGroupSize = 64;

	// -----------------------
	//			buffer
	// -----------------------
	StructuredBuffer	m_ParticleBuffer;
	StructuredBuffer	m_DeadListBuffer;
	StructuredBuffer	m_AliveListBuffer[2]; // In / Out (ping-pong)
	ByteAddressBuffer	m_CounterBuffer;
	ByteAddressBuffer	m_IndirectArgsBuffer; // FOR INDIRECT

	uint32_t			m_AliveCurrent = 0;

	// -----------------------
	//		Readback buffer
	// -----------------------
	ReadbackBuffer		m_CounterReadback[sReadbackLatency];
	uint64_t			m_ReadbackFence[sReadbackLatency] = {};
	uint32_t			m_ReadbackSlot = 0;

	Particles::CounterSnapshot m_LastCounters = {};

	bool				m_initVerified = false;
};

