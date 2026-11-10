#pragma once

#include "AttributeType.h"
#include "CompiledEmitter.h"

struct EngineValues // Temp
{
	float  DeltaTime = 0.f;
	float3 OwnerPosition = { 0.f, 0.f, 0.f };
	uint32_t RandomSeed = 0; // 프레임마다 달라지는 랜덤시드
};

template<typename T>
struct StridedView 
{
	uint8_t* base; // start + offset
	uint32_t stride; // 파티클 하나의 크기
	T& operator[](uint32_t i)
	{ 
		return *reinterpret_cast<T*>(base + i * stride);
	}
};

class ModuleContext;

// 입력값 하나를 파티클별로 평가한다. ctx.Param<T>() 로 루프 밖에서 한 번 얻는다
template<typename T>
class ParamReader
{
public:
	T operator()(uint32_t particleIndex) const;

private:
	friend class ModuleContext;

	const ModuleContext*	m_Context = nullptr;
	T						m_A{};
	T						m_B{};
	ParamKind				m_Kind = ParamKind::Constant;
	bool					m_PerComponent = false;
	uint32_t				m_Salt = 0;
};

class ModuleContext
{
public:
	ModuleContext(const CompiledEmitter& compiled,
		uint8_t* particleData,   // 파티클 버퍼 시작 (Stride x 최대 개수)
		uint8_t* emitterData,    // Emitter 저장소 시작
		const EngineValues& engine,
		uint32_t			   capacity,
		uint32_t               begin,          // 이번에 처리할 파티클 범위 [begin, end)
		uint32_t               end,
		uint8_t*			   aliveaFlags);

	uint32_t Begin() const { return m_Begin; } // 시작 지점
	uint32_t End() const { return m_End; }

	bool HasParticle(const std::string& name) const { return m_Compiled.particles.Find(name) != nullptr; }


	template<typename T> StridedView<T> Particles(const std::string& name);
	template<typename T> T& Emitter(const std::string& name);

	float         DeltaTime()     const { return m_Engine.DeltaTime; }
	const float3& OwnerPosition() const { return m_Engine.OwnerPosition; }

	float			Random01(uint32_t particleIndex, uint32_t salt) const;

	void			Kill(uint32_t i);
	bool			CanKill() const {return m_AliveFlags != nullptr;}

	void			BeginModule(int compiledIndex) { m_ModuleIndex = compiledIndex; }
	template<typename T> ParamReader<T> Param(const std::string& name) const;

private:
	static const Slot* FindChecked(const Block& block, const std::string& fullName, const TypeInfo& expected);

	const CompiledEmitter& m_Compiled;
	uint8_t*				m_ParticleData;
	uint8_t*				m_EmitterData;
	const EngineValues&		m_Engine;
	uint32_t				m_Capacity;
	uint32_t               m_Begin;
	uint32_t               m_End;
	uint8_t*				m_AliveFlags;
	int						m_ModuleIndex = -1;

};

template<typename T>
inline T ParamReader<T>::operator()(uint32_t particleIndex) const
{
	if (m_Kind == ParamKind::Constant)
		return m_A;

	// RandomRange: HLSL 의 min + (max - min) * t 와 같은 식
	T out;
	const float* a = reinterpret_cast<const float*>(&m_A);
	const float* b = reinterpret_cast<const float*>(&m_B);
	float* o = reinterpret_cast<float*>(&out);
	const float shared = m_Context->Random01(particleIndex, m_Salt);
	for (uint32_t c = 0; c < sizeof(T) / sizeof(float); ++c)
	{
		const float t = m_PerComponent ? m_Context->Random01(particleIndex, m_Salt + c) : shared;
		o[c] = a[c] + (b[c] - a[c]) * t;
	}
	return out;
}

template<typename T>
inline ParamReader<T> ModuleContext::Param(const std::string& name) const
{
	assert(m_ModuleIndex >= 0 && "Param: BeginModule 이 호출되지 않음");
	const CompiledModule& module = m_Compiled.modules[m_ModuleIndex];

	const CompiledParam* info = nullptr;
	for (const CompiledParam& p : module.paramInfos)
		if (p.name == name) { info = &p; break; }
	assert(info != nullptr && "Param: 모듈의 params 에 선언되지 않은 이름");

	ParamReader<T> ret;
	ret.m_Context = this;
	ret.m_Kind = info->kind;
	ret.m_PerComponent = info->perComponent;
	ret.m_Salt = info->salt;

	const std::vector<std::string> slots = ParamSlotNames(name, info->kind);
	const std::string prefix = "Module." + module.scope + ".";
	const uint8_t* data = m_Compiled.moduleParamData.data();

	std::memcpy(&ret.m_A, data + FindChecked(m_Compiled.moduleParams, prefix + slots[0], TypeOf<T>())->offset, sizeof(T));
	ret.m_B = ret.m_A;
	if (slots.size() > 1)
		std::memcpy(&ret.m_B, data + FindChecked(m_Compiled.moduleParams, prefix + slots[1], TypeOf<T>())->offset, sizeof(T));
	return ret;
}

template<typename T>
inline StridedView<T> ModuleContext::Particles(const std::string& name)
{
	const Slot* slot = FindChecked(m_Compiled.particles, name, TypeOf<T>());

	StridedView<T> ret;
	ret.base = m_ParticleData + size_t(slot->offset) * m_Capacity;
	ret.stride = slot->type->size;
	return ret;
}

template<typename T>
inline T& ModuleContext::Emitter(const std::string& name)
{
	const Slot* slot = FindChecked(m_Compiled.emitterParams, name, TypeOf<T>());
	return *reinterpret_cast<T*>(m_EmitterData + slot->offset);

}
