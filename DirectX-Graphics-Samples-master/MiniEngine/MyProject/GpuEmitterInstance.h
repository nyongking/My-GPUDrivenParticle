#pragma once
#include "CompiledEmitter.h"
#include "EmitterGpuProgram.h"

#include "GpuBuffer.h"
#include "ReadbackBuffer.h"
#include "OwnerTransform.h"

#include <cstdint>
#include <string>

class GpuEmitterInstance
{
public:
	GpuEmitterInstance() = default;
	GpuEmitterInstance(const GpuEmitterInstance&) = delete;
	GpuEmitterInstance& operator=(const GpuEmitterInstance&) = delete;

	bool Initialize(const CompiledEmitter& compiled, 
		const EmitterGpuProgram& program,
		uint32_t capacity);
	void Shutdown();

	void Tick(float deltaTime, const OwnerTransform& t);
	void Tick(float deltaTime, const float3& ownerPosition);

	// 입력값 바이트만 교체 (레이아웃 동일). 다음 Tick 부터 반영
	void SetModuleParamData(const std::vector<uint8_t>& data);
	// 파티클 레이아웃이 같은 새 프로그램으로 교체 (버퍼 유지). 호출 전 GPU idle 필요
	void Rebind(const CompiledEmitter& compiled, const EmitterGpuProgram& program);

	bool		Ready() const { return m_Ready; }
	uint32_t	Capacity() const { return m_Capacity; }
	const CompiledEmitter& Compiled() const { return m_Compiled; }

	ByteAddressBuffer& ParticleBuffer() { return m_Particles; }
	IndirectArgsBuffer& DrawArgs() { return m_DrawArgs; }

	// readback으로 받은 값
	uint32_t	ReadbackAlive() const { return m_ReadbackAlive; }
	uint32_t	ReadbackRequest() const { return m_ReadbackRequest; }

#ifndef RELEASE
	const std::string& Log() const { return m_Log; }

#endif


private:
	static const uint32_t kReadbackSlots = 3;

	void ReadCountersIfReady();
	void LoadModuleParams();

	CompiledEmitter          m_Compiled;
	const EmitterGpuProgram* m_Program = nullptr;

	ByteAddressBuffer        m_Particles;      // Particle 버퍼, 추후 In/Out 형태로 바꿀 예정
	ByteAddressBuffer        m_Counters;       // ALIVE_IN, WRITE_CURSOR, SPAWN_REQUEST, (pad)의 정보 저장
	ByteAddressBuffer        m_EmitterData;    // Emitter.* 의 데이터 저장
	IndirectArgsBuffer       m_SpawnArgs;      // DispatchIndirect (x, y, z)
	IndirectArgsBuffer       m_DrawArgs;       // DrawIndirect (vertex, instance, 0, 0)

	// b1 ModuleParams: ConstantBufferSize 만큼, 16 바이트 정렬 (SetDynamicConstantBufferView 조건)
	struct alignas(16) Chunk16 { uint8_t bytes[16]; };
	std::vector<Chunk16>     m_ModuleParams;

	ReadbackBuffer           m_Readback[kReadbackSlots];
	uint64_t                 m_ReadbackFence[kReadbackSlots] = {};
	uint32_t                 m_ReadbackAlive = 0;
	uint32_t                 m_ReadbackRequest = 0;

	uint32_t                 m_Capacity = 0;
	uint32_t                 m_Frame = 0;
	float3                   m_PrevOwnerPosition = { 0.f, 0.f, 0.f };
	bool                     m_HasPrevOwner = false;
	bool                     m_Ready = false;


#ifndef RELEASE
	std::string              m_Log;
#endif
};

