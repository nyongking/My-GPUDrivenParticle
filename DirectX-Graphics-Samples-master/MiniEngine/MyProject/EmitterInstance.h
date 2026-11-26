#pragma once
#include "ModuleDesc.h"
#include "ModuleContext.h"
#include "CompiledEmitter.h"
#include "OwnerTransform.h"
#include "RenderParticle.h"

#include <vector>

class EmitterInstance
{
public:
	bool Initialize(const std::vector<ModuleDesc>& modules,
		const std::vector<RendererDesc>& renders,
		uint32_t capacity);

	void Tick(float deltaTime, const float3& ownerPosition);
	void Tick(float deltaTime, const OwnerTransform& t);
	void Shutdown();

	// 입력값 바이트만 교체 (레이아웃 동일)
	void SetModuleParamData(const std::vector<uint8_t>& data);
	// 파티클 레이아웃이 같은 컴파일 결과로 교체 (살아 있는 파티클 유지)
	void ReplaceCompiled(const CompiledEmitter& compiled);

	uint32_t				Count() const { return m_Count; }
	uint32_t				Capacity() const { return m_Capacity; }
	const CompiledEmitter&	Compiled() const { return m_CompiledEmitter; }
	const uint8_t*			ParticleData() const { return m_ParticleData.data(); }   // 가변 속성 (SoA)

	size_t	 ParticleDataBytes() const { return m_ParticleData.size(); }

	// 고정 렌더 구조체 배열 (RenderParticleData x capacity). 렌더러가 그대로 올린다
	const RenderParticleData* RenderData() const { return m_RenderData.data(); }
	// 아무 모듈도 쓰지 않는 필드에 들어갈 값. 다음에 태어나는 파티클부터 적용
	void SetRenderDefaults(const RenderParticleData& defaults) { m_RenderDefaults = defaults; }

	uint32_t NewBegin() const { return m_NewBegin; }
	uint32_t NewEnd() const { return m_NewEnd; }

	uint32_t KilledLastTick() const { return m_KilledLastTick; }


#ifndef RELEASE
	const std::string& Log() const { return m_Log; }
#endif
private:
	void RunModules(std::vector<ModuleDesc>& modules, uint32_t begin, uint32_t end, uint8_t* aliveFlags = nullptr);
	void ZeroRange(uint32_t begin, uint32_t end); // 시작지점부터 끝지점까지 clear
	void ApplyCopyOps(const std::vector<CopyOp>& ops, uint32_t begin, uint32_t end);

	void CopyParticles(uint32_t from, uint32_t to);
	uint32_t CompactAlive();

	static constexpr float kMaxDeltaTime = 0.1f;
	
	CompiledEmitter				m_CompiledEmitter;
	std::vector<ModuleDesc>		m_EmitterUpdates;
	std::vector<ModuleDesc>		m_ParticleSpawns;
	std::vector<ModuleDesc>		m_ParticleUpdates;

	std::vector<uint8_t>    m_ParticleData;		// 가변 속성 (SoA)
	std::vector<RenderParticleData> m_RenderData;	// 고정 렌더 구조체 (AoS)
	RenderParticleData		m_RenderDefaults = RenderParticle::Defaults();
	std::vector<uint8_t>    m_EmitterData;
	std::vector<uint8_t>	m_AliveFlags;
	EngineValues			m_EngineValues;
	float3					m_PrevOwnerPosition = { 0.f, 0.f, 0.f };
	bool					m_HasPrevOwner = false;
	float					m_Age = 0.f;			// Engine.Emitter.Age (sum of clamped dt since Initialize)

	uint32_t				m_Capacity = 0;
	uint32_t				m_Count = 0;
	uint32_t				m_Frame = 0;
	uint32_t				m_NewBegin = 0;
	uint32_t				m_NewEnd = 0;
	uint32_t				m_KilledLastTick = 0;

#ifndef RELEASE
	std::string				m_Log;
#endif
};

