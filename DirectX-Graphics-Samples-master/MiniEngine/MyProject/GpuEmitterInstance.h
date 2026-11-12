#pragma once
#include "CompiledEmitter.h"
#include "EmitterGpuProgram.h"

#include "GpuBuffer.h"
#include "ReadbackBuffer.h"
#include "OwnerTransform.h"
#include "UberEmitterData.h"     // [W5-4 추가]

#include <cstdint>
#include <string>

class ComputeContext;

struct GpuSimFrameStats
{
	uint32_t psoChanges = 0;
	uint32_t dispatches = 0;
	uint32_t emitters = 0;
	uint32_t uberEmitters = 0;
	uint32_t uberFallbacks = 0;
};

class GpuEmitterInstance
{
public:
	GpuEmitterInstance() = default;
	GpuEmitterInstance(const GpuEmitterInstance&) = delete;
	GpuEmitterInstance& operator=(const GpuEmitterInstance&) = delete;

	static void BeginFrameStats();
	static const GpuSimFrameStats& FrameStats();

	bool Initialize(const CompiledEmitter& compiled, 
		const EmitterGpuProgram& program,
		uint32_t capacity);
	void Shutdown();

	void Tick(ComputeContext& ctx, float deltaTime, const OwnerTransform& t);
	void Tick(ComputeContext& ctx, float deltaTime, const float3& ownerPosition);
	void OnSubmitted(uint64_t fence);

	// 입력값 바이트만 교체 (레이아웃 동일). 다음 Tick 부터 반영
	void SetModuleParamData(const std::vector<uint8_t>& data);
	// 파티클 레이아웃이 같은 새 프로그램으로 교체 (버퍼 유지). 호출 전 GPU idle 필요
	void Rebind(const CompiledEmitter& compiled, const EmitterGpuProgram& program);
	void SetUberData(const UberEmitterData& data);
	bool UberReady() const { return m_UberOk; }

	bool		Ready() const { return m_Ready; }
	uint32_t	Capacity() const { return m_Capacity; }
	const CompiledEmitter& Compiled() const { return m_Compiled; }

	ByteAddressBuffer& ParticleBuffer() { return m_Particles[m_Current]; }
	IndirectArgsBuffer& DrawArgs() { return m_FrameArgs; }

	// readback으로 받은 값
	uint32_t	ReadbackAlive() const { return m_ReadbackAlive; }
	uint32_t	ReadbackRequest() const { return m_ReadbackRequest; }

#ifndef RELEASE
	const std::string& Log() const { return m_Log; }

#endif


private:
	void ReadCountersIfReady();
	void LoadModuleParams();

	static const uint32_t kReadbackSlots = 3;
	
	CompiledEmitter          m_Compiled;
	const EmitterGpuProgram* m_Program = nullptr;

	ByteAddressBuffer        m_Particles[2];      // Particle 버퍼, In/Out 형태의 버퍼 구성
	ByteAddressBuffer        m_Counters;       // ALIVE_IN, WRITE_CURSOR, SPAWN_REQUEST, (pad)의 정보 저장
	ByteAddressBuffer        m_EmitterData;    // Emitter.* 의 데이터 저장
	IndirectArgsBuffer       m_SpawnArgs;      // DispatchIndirect (x, y, z)
	IndirectArgsBuffer       m_FrameArgs;      // [0 - 15] : DrawIndirect 인자 (6, AliveCount, 0, 0) / [16 - 27] : DispatchIndirect 인자 (다음 프레임의 ParticleUpdate 그룹 수, 1, 1)


	// b1 ModuleParams: ConstantBufferSize 만큼, 16 바이트 정렬 (SetDynamicConstantBufferView 조건)
	struct alignas(16) Chunk16 { uint8_t bytes[16]; };
	std::vector<Chunk16>     m_ModuleParams;

	std::vector<Chunk16>     m_UberParams;
	alignas(16) EmitterLayoutCB m_UberLayout;
	bool                     m_UberOk = false;

	ReadbackBuffer           m_Readback[kReadbackSlots];
	uint64_t                 m_ReadbackFence[kReadbackSlots] = {};
	uint32_t                 m_ReadbackAlive = 0;
	uint32_t                 m_ReadbackRequest = 0;

	uint32_t				 m_SubmitSlot = 0;
	bool					 m_WaitingSubmit = false;

	uint32_t                 m_Capacity = 0;
	uint32_t                 m_Frame = 0;
	float3                   m_PrevOwnerPosition = { 0.f, 0.f, 0.f };
	bool                     m_HasPrevOwner = false;
	bool                     m_Ready = false;

	uint32_t				 m_Current = 0;

#ifndef RELEASE
	std::string              m_Log;
#endif
};

