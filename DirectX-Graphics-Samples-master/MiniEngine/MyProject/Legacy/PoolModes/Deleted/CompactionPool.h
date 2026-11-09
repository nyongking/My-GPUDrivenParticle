#pragma once
#include "ParticlePool.h"

#include "GpuBuffer.h"
#include "ReadbackBuffer.h"

class CompactionPool : public ParticlePool
{
public:
	CompactionPool(Particles::Mode mode = Particles::Mode::Compaction)
		: ParticlePool(mode) {
	}

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

	StructuredBuffer& Current() { return m_ParticleBuffer[m_Current]; }
	StructuredBuffer& Next() { return m_ParticleBuffer[1 - m_Current]; }

	static constexpr uint32_t sThreadGroupSize = 64;

	StructuredBuffer	m_ParticleBuffer[2];	// ÇÎÆþ (48 B x MAX x 2)
	ByteAddressBuffer	m_CounterBuffer;		// alive / (dead ¾È ¾¸) / emit / aliveAfterSim
	ByteAddressBuffer	m_IndirectArgsBuffer;	// 0~12: Simulate dispatch, 16~32: draw

	uint32_t			m_Current = 0;

	ReadbackBuffer		m_CounterReadback[sReadbackLatency];
	uint64_t			m_ReadbackFence[sReadbackLatency] = {};
	uint32_t			m_ReadbackSlot = 0;

	Particles::CounterSnapshot m_LastCounters = {};
	bool				m_initVerified = false;
};