#pragma once

#include "VectorMath.h"
#include "GpuBuffer.h"
#include "ReadbackBuffer.h"
#include "ParticleTypes.h"

class GraphicsContext;
class ComputeContext;
struct EmitRequest;

namespace Math { class Camera; }

// GPU 파티클 풀 (AliveList + Indirect Dispatch/Draw + Wave Intrinsic)
//  Emit     : DeadList에서 슬롯을 꺼내 AliveIn에 추가
//  PreSim   : alive 수로 Simulate DispatchIndirect 인자 계산
//  Simulate : AliveIn만 순회, 살아남은 것은 AliveOut, 죽은 것은 DeadList로 (wave 단위 atomic)
//  AfterSim : 살아남은 수를 alive로 확정 + DrawIndirect 인자 갱신
class ParticlePool
{
public:
	ParticlePool() {}
	~ParticlePool() {}

public:
	void Initialize(uint32_t maxParticles);
	void Shutdown();

	void Update(float deltaT, const EmitRequest& emitRequest);
	void Render(GraphicsContext& gfx, const Math::Camera& cam);

	uint32_t					GetMaxParticles() const { return m_MaxParticles; }
	Particles::CounterSnapshot	GetCounterSnapshot() const { return m_LastCounters; }

private:
	void	CreateBuffers(uint32_t maxParticles);
#ifndef RELEASE
	void	CollectCounterReadback();
#endif
	bool	VerifyInitialState();
	void	InsertUAVBarriers(ComputeContext& ctx);

	StructuredBuffer& AliveIn() { return m_AliveListBuffer[m_AliveCurrent]; }
	StructuredBuffer& AliveOut() { return m_AliveListBuffer[1 - m_AliveCurrent]; }

	static constexpr uint32_t sThreadGroupSize = 64;
	static constexpr uint32_t sReadbackLatency = 3;

	uint32_t			m_MaxParticles = 0;
	uint32_t			m_FrameIndex = 0;

	// -----------------------
	//			buffer
	// -----------------------
	StructuredBuffer	m_ParticleBuffer;
	StructuredBuffer	m_DeadListBuffer;
	StructuredBuffer	m_AliveListBuffer[2]; // In / Out (ping-pong)
	ByteAddressBuffer	m_CounterBuffer;
	ByteAddressBuffer	m_IndirectArgsBuffer; // Simulate Dispatch Args + Draw Args

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
