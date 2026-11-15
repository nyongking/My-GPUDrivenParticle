#include "pch.h"
#include "UberEmitterData.h"

#include "EmitterCompiler.h"     // ModuleScope (생성 방식과 같은 이름 규칙)
#include "ModuleLibrary.h"
#include "ShaderCompiler.h"
#include "ShaderGenerator.h"     // ShaderNames::Param / HlslType
#include "TagLibrary.h"

#include "CompiledShaders/ParticleUberEmitterUpdateCS.h"
#include "CompiledShaders/ParticleUberParticleUpdateCS.h"
#include "CompiledShaders/ParticleUberParticleSpawnCS.h"

#include <cstddef>
#include <cstring>

namespace
{
	constexpr uint32_t kModuleCount = uint32_t(ModuleId::Count);

	bool ValidId(ModuleId id) { return uint32_t(id) < kModuleCount; }

	// 기준 모듈들 (모듈 번호별 단계 / 입력값 dword 수 확인 기준). ModuleId 순서 그대로 (값은 쓰지 않으므로 아무거나)
	std::vector<ModuleDesc> ReferenceModules()
	{
		std::vector<ModuleDesc> m =
		{
			SpawnRate(0.f),
			InitializeParticle(0.f, 0.f, 0.f, float3{ 0.f, 0.f, 0.f }, float4{ 0.f, 0.f, 0.f, 0.f }),
			SphereLocation(0.f, false),
			AddVelocityInCone(0.f, 0.f, float3{ 0.f, 1.f, 0.f }),
			ParticleState(false),
			GravityForce(float3{ 0.f, 0.f, 0.f }),
			SolveForceAndVelocity(0.f, 0.f),
			SubUVAnimation(0.f, 0.f),
			InitializeSprite(float2{ 0.f, 0.f }, 0.f),
			SpriteRotationRate(0.f)
		};
		for (uint32_t i = 0; i < uint32_t(m.size()); ++i)
			ASSERT(uint32_t(m[i].id) == i, "ReferenceModules 순서가 ModuleId 와 다름");
		return m;
	}

	// ParticleUber.hlsli 모듈 함수가 읽는 입력값 dword 수 (ModuleId 순서). 셰이더 주석의 표와 같다
	constexpr uint32_t kExpectedRecordDwords[] = { 1, 20, 3, 17, 1, 3, 2, 4, 6, 1 };
	static_assert(sizeof(kExpectedRecordDwords) / sizeof(kExpectedRecordDwords[0]) == size_t(ModuleId::Count), "kExpectedRecordDwords 개수");

	// 공유 셰이더가 단계 함수에서 도는 단계 (b2 의 범위 순서)
	const Stage kUberStages[] = { Stage::EmitterUpdate, Stage::ParticleSpawn, Stage::ParticleUpdate };

	const std::vector<ModuleDesc>& References()
	{
		static const std::vector<ModuleDesc> s_Ref = ReferenceModules();
		return s_Ref;
	}

	// 모듈 스택 -> b1 (인스턴스 목록 + 입력값 풀). L 이 있으면 단계별 범위도 채운다
	void BuildModuleData(const std::vector<ModuleDesc>& modules, std::vector<uint8_t>& out,
		EmitterLayoutCB* L, std::vector<std::string>* errors)
	{
		auto fail = [&](const std::string& msg) { if (errors) errors->push_back(msg); };

		// ---- 1. 실행할 인스턴스 고르기: 단계별로, 단계 안은 스택 순서 ----
		std::vector<uint32_t> order;               // modules 의 순번
		uint32_t begin[3] = {}, count[3] = {};
		for (int g = 0; g < 3; ++g)
		{
			begin[g] = uint32_t(order.size());
			for (uint32_t mi = 0; mi < uint32_t(modules.size()); ++mi)
			{
				const ModuleDesc& m = modules[mi];
				if (!ValidId(m.id) || m.stage != kUberStages[g])
					continue;
				if (References()[uint32_t(m.id)].stage != m.stage)
					continue;   // 아래 검사에서 오류로 남김
				order.push_back(mi);
			}
			count[g] = uint32_t(order.size()) - begin[g];
		}

		for (const ModuleDesc& m : modules)
		{
			if (!ValidId(m.id))
				fail(m.name + ": 공유 셰이더가 모르는 모듈 (ModuleDesc::id 가 없음)");
			else if (References()[uint32_t(m.id)].stage != m.stage)
				fail(m.name + ": 공유 셰이더가 이 모듈을 실행하는 단계와 다름");
		}

		// ---- 2. 크기 계산 ----
		const uint32_t instCount = uint32_t(order.size());
		uint32_t total = instCount * 4;             // dword
		std::vector<UberParamRecord> records(instCount);
		for (uint32_t i = 0; i < instCount; ++i)
		{
			records[i] = MakeUberParamRecord(modules[order[i]]);
			total += records[i].dwords;
		}
		if (total > Uber::kMaxDataDwords)
		{
			fail("모듈 데이터가 " + std::to_string(total * 4) + " 바이트라 상한(" + std::to_string(Uber::kMaxDataDwords * 4) + ") 을 넘음");
			out.assign(16, 0);
			return;
		}

		out.assign((std::max)(size_t(16), size_t((total * 4 + 15) & ~15u)), 0);   // 16 바이트 단위 (Chunk16)

		uint32_t* dw = reinterpret_cast<uint32_t*>(out.data());

		// ---- 3. 인스턴스 + 입력값 쓰기 ----
		uint32_t base = instCount * 4;
		for (uint32_t i = 0; i < instCount; ++i)
		{
			const uint32_t mi = order[i];
			const ModuleDesc& m = modules[mi];
			const UberParamRecord& rec = records[i];

			Uber::ModuleInstGpu inst = { uint32_t(m.id), mi, base, 0 };

			for (uint32_t pi = 0; pi < uint32_t(m.params.size()); ++pi)
			{
				const ParamDesc& p = m.params[pi];
				if (p.type == nullptr || (p.type->size % 4) != 0)
					continue;   // EmitterCompiler 가 이미 오류로 남김
				const size_t typeSize = p.type->size;
				const size_t srcCount = ParamSlotNames(p.name, p.kind).size();     // Constant 1개 / RandomRange 2개
				if (p.value.size() != typeSize * srcCount)
					continue;   // EmitterCompiler 가 이미 오류로 남김

				if (p.kind == ParamKind::RandomRange && p.perComponent && pi < 32)
					inst.flags |= 1u << pi;

				uint8_t* dst = out.data() + size_t(base + rec.paramDword[pi]) * 4;
				std::memcpy(dst, p.value.data(), typeSize);                                       // Min (또는 상수)
				if (IsUberRangeParam(m.stage, p.type))
					std::memcpy(dst + typeSize, p.value.data() + (srcCount > 1 ? typeSize : 0), typeSize);   // Max (상수면 Min 과 같게)
			}

			std::memcpy(dw + i * 4, &inst, sizeof(inst));
			base += rec.dwords;
		}

		if (L)
		{
			L->emitterInstBegin = begin[0]; L->emitterInstCount = count[0];
			L->spawnInstBegin = begin[1];   L->spawnInstCount = count[1];
			L->updateInstBegin = begin[2];  L->updateInstCount = count[2];
		}
	}

	uint32_t FindOffset(const Block& block, const char* fullName)
	{
		const Slot* s = block.Find(fullName);
		return s ? s->offset : Uber::kNoAttribute;
	}

	// 공유 셰이더가 아는 파티클 속성 (EmitterLayout 의 Ofs_*)
	const char* const kKnownParticleAttributes[] =
	{
		Particles::Position, Particles::Color, Particles::Size, Particles::Lifetime, Particles::Age,
		Particles::Mass, Particles::NormalizedAge, Particles::Velocity, Particles::Acceleration, Particles::SubImageIndex,
		Particles::SpriteSize, Particles::SpriteRotation
	};

	std::string OfsText(uint32_t ofs)
	{
		return ofs == Uber::kNoAttribute ? std::string("-") : std::to_string(ofs);
	}
}

const char* ModuleIdName(ModuleId id)
{
	static const char* const kNames[] =
	{
		"SpawnRate", "InitializeParticle", "SphereLocation", "AddVelocityInCone",
		"ParticleState", "GravityForce", "SolveForceAndVelocity", "SubUVAnimation",
		"InitializeSprite", "SpriteRotationRate"
	};
	static_assert(sizeof(kNames) / sizeof(kNames[0]) == size_t(ModuleId::Count), "ModuleIdName 개수");
	return ValidId(id) ? kNames[uint32_t(id)] : "Unknown";
}

bool IsUberRangeParam(Stage stage, const TypeInfo* type)
{
	return stage == Stage::ParticleSpawn && IsFloatFamily(type);
}

UberParamRecord MakeUberParamRecord(const ModuleDesc& m)
{
	UberParamRecord r;
	for (const ParamDesc& p : m.params)
	{
		r.paramDword.push_back(r.dwords);
		const uint32_t n = p.type ? p.type->size / 4 : 0;
		r.dwords += IsUberRangeParam(m.stage, p.type) ? n * 2 : n;     // Min, Max 두 벌 / 한 벌
	}
	return r;
}

void FillUberParams(const std::vector<ModuleDesc>& modules, std::vector<uint8_t>& out, std::vector<std::string>* errors)
{
	BuildModuleData(modules, out, nullptr, errors);
}

UberEmitterData BuildUberEmitterData(const std::vector<ModuleDesc>& modules, const CompiledEmitter& compiled)
{
	UberEmitterData d;
	EmitterLayoutCB& L = d.layout;

	// ---------- 1. 모듈 ----------
	// 인스턴스 목록은 스택 순서 그대로. 범위와 모듈 데이터는 BuildModuleData 가 만든다
	BuildModuleData(modules, d.params, &L, &d.errors);

	// ---------- 2. 속성 열의 위치 (trim 됐거나 없으면 NO_ATTRIBUTE) ----------
	L.ofsEmitterSpawnCount = FindOffset(compiled.emitterParams, Emitter::SpawnCount);
	L.ofsEmitterSpawnRemainder = FindOffset(compiled.emitterParams, Emitter::SpawnRemainder);
	L.ofsPosition = FindOffset(compiled.particles, Particles::Position);
	L.ofsColor = FindOffset(compiled.particles, Particles::Color);
	L.ofsSize = FindOffset(compiled.particles, Particles::Size);
	L.ofsLifetime = FindOffset(compiled.particles, Particles::Lifetime);
	L.ofsAge = FindOffset(compiled.particles, Particles::Age);
	L.ofsMass = FindOffset(compiled.particles, Particles::Mass);
	L.ofsNormalizedAge = FindOffset(compiled.particles, Particles::NormalizedAge);
	L.ofsVelocity = FindOffset(compiled.particles, Particles::Velocity);
	L.ofsAcceleration = FindOffset(compiled.particles, Particles::Acceleration);
	L.ofsSubImageIndex = FindOffset(compiled.particles, Particles::SubImageIndex);
	L.ofsSpriteSize = FindOffset(compiled.particles, Particles::SpriteSize);
	L.ofsSpriteRotation = FindOffset(compiled.particles, Particles::SpriteRotation);

	// 공유 셰이더가 모르는 열 (Previous/Initial 등): 셰이더가 읽지도 쓰지도 않는다
	for (const Slot& s : compiled.particles.Slots())
	{
		const std::string full = s.key.Full();
		bool known = false;
		for (const char* k : kKnownParticleAttributes)
			known |= (full == k);
		if (!known)
			d.warnings.push_back("'" + full + "': 공유 셰이더가 모르는 속성 (읽기/쓰기 안 됨)");
	}

	// ---------- 3. 이번 범위에서 지원하지 않는 것 ----------
	if (!compiled.copyAtSpawnEnd.empty() || !compiled.copyAtUpdateStart.empty())
		d.warnings.push_back("Previous / Initial 속성 복사는 공유 셰이더에서 아직 지원하지 않음 (해당 값은 0)");

	return d;
}

std::string DescribeUber(const UberEmitterData& d)
{
	const EmitterLayoutCB& L = d.layout;
	char buf[128];
	std::string s = "[Uber] ";

	const uint32_t* dw = reinterpret_cast<const uint32_t*>(d.params.data());
	const uint32_t dwCount = uint32_t(d.params.size() / 4);
	sprintf_s(buf, "ranges: Emitter %u+%u, Spawn %u+%u, Update %u+%u, b1 %u bytes\n",
		L.emitterInstBegin, L.emitterInstCount, L.spawnInstBegin, L.spawnInstCount,
		L.updateInstBegin, L.updateInstCount, uint32_t(d.params.size()));
	s += buf;
	const uint32_t instCount = L.emitterInstCount + L.spawnInstCount + L.updateInstCount;
	for (uint32_t i = 0; i < instCount && i * 4 + 3 < dwCount; ++i)
	{
		Uber::ModuleInstGpu inst;
		std::memcpy(&inst, dw + i * 4, sizeof(inst));
		const uint32_t next = (i + 1 < instCount) ? dw[(i + 1) * 4 + 2] : dwCount;
		sprintf_s(buf, "  [%u] %s slot %u base %u flags 0x%X :", i, ModuleIdName(ModuleId(inst.id)), inst.slot, inst.base, inst.flags);
		s += buf;
		for (uint32_t k = inst.base; k < next && k < dwCount; ++k)
		{
			// 값 종류를 모르므로 작은 정수는 uint, 나머지는 float 로 보여준다
			if (dw[k] != 0 && dw[k] < 0x00800000u) sprintf_s(buf, " %u", dw[k]);
			else { float f; std::memcpy(&f, dw + k, 4); sprintf_s(buf, " %g", f); }
			s += buf;
		}
		s += "\n";
	}

	s += "  Emitter  : SpawnCount " + OfsText(L.ofsEmitterSpawnCount) + ", SpawnRemainder " + OfsText(L.ofsEmitterSpawnRemainder) + "\n";
	s += "  Particles: Position " + OfsText(L.ofsPosition) + ", Color " + OfsText(L.ofsColor) + ", Size " + OfsText(L.ofsSize)
		+ ", Lifetime " + OfsText(L.ofsLifetime) + ", Age " + OfsText(L.ofsAge) + ", Mass " + OfsText(L.ofsMass)
		+ ", NormalizedAge " + OfsText(L.ofsNormalizedAge) + ", Velocity " + OfsText(L.ofsVelocity)
		+ ", Acceleration " + OfsText(L.ofsAcceleration) + ", SubImageIndex " + OfsText(L.ofsSubImageIndex)
		+ ", SpriteSize " + OfsText(L.ofsSpriteSize) + ", SpriteRotation " + OfsText(L.ofsSpriteRotation) + "\n";

	for (const std::string& w : d.warnings) s += "  [경고] " + w + "\n";
	for (const std::string& e : d.errors)   s += "  [오류] " + e + "\n";
	return s;
}

bool CheckUberShaderLayout(const ShaderCompiler& compiler, std::string& log)
{
	struct Shader { const char* name; const void* data; size_t size; };
	const Shader shaders[] =
	{
		{ "ParticleUberEmitterUpdateCS",  g_pParticleUberEmitterUpdateCS,  sizeof(g_pParticleUberEmitterUpdateCS) },
		{ "ParticleUberParticleUpdateCS", g_pParticleUberParticleUpdateCS, sizeof(g_pParticleUberParticleUpdateCS) },
		{ "ParticleUberParticleSpawnCS",  g_pParticleUberParticleSpawnCS,  sizeof(g_pParticleUberParticleSpawnCS) },
	};

	// EmitterLayout: C++ 구조체의 필드 위치
	struct Field { const char* hlsl; uint32_t offset; };
	const Field layoutFields[] =
	{
		{ "EmitterInstBegin",           uint32_t(offsetof(EmitterLayoutCB, emitterInstBegin)) },
		{ "EmitterInstCount",           uint32_t(offsetof(EmitterLayoutCB, emitterInstCount)) },
		{ "SpawnInstBegin",             uint32_t(offsetof(EmitterLayoutCB, spawnInstBegin)) },
		{ "SpawnInstCount",             uint32_t(offsetof(EmitterLayoutCB, spawnInstCount)) },
		{ "UpdateInstBegin",            uint32_t(offsetof(EmitterLayoutCB, updateInstBegin)) },
		{ "UpdateInstCount",            uint32_t(offsetof(EmitterLayoutCB, updateInstCount)) },
		{ "Ofs_Emitter_SpawnCount",     uint32_t(offsetof(EmitterLayoutCB, ofsEmitterSpawnCount)) },
		{ "Ofs_Emitter_SpawnRemainder", uint32_t(offsetof(EmitterLayoutCB, ofsEmitterSpawnRemainder)) },
		{ "Ofs_Position",               uint32_t(offsetof(EmitterLayoutCB, ofsPosition)) },
		{ "Ofs_Color",                  uint32_t(offsetof(EmitterLayoutCB, ofsColor)) },
		{ "Ofs_Size",                   uint32_t(offsetof(EmitterLayoutCB, ofsSize)) },
		{ "Ofs_Lifetime",               uint32_t(offsetof(EmitterLayoutCB, ofsLifetime)) },
		{ "Ofs_Age",                    uint32_t(offsetof(EmitterLayoutCB, ofsAge)) },
		{ "Ofs_Mass",                   uint32_t(offsetof(EmitterLayoutCB, ofsMass)) },
		{ "Ofs_NormalizedAge",          uint32_t(offsetof(EmitterLayoutCB, ofsNormalizedAge)) },
		{ "Ofs_Velocity",               uint32_t(offsetof(EmitterLayoutCB, ofsVelocity)) },
		{ "Ofs_Acceleration",           uint32_t(offsetof(EmitterLayoutCB, ofsAcceleration)) },
		{ "Ofs_SubImageIndex",          uint32_t(offsetof(EmitterLayoutCB, ofsSubImageIndex)) },
		{ "Ofs_SpriteSize",             uint32_t(offsetof(EmitterLayoutCB, ofsSpriteSize)) },
		{ "Ofs_SpriteRotation",         uint32_t(offsetof(EmitterLayoutCB, ofsSpriteRotation)) },
	};

	auto findVar = [](const ConstantBufferInfo& info, const std::string& name) -> const ConstantBufferInfo::Variable*
		{
			for (const ConstantBufferInfo::Variable& v : info.variables)
				if (v.name == name) return &v;
			return nullptr;
		};

	bool ok = true;
	log = "[Uber] cbuffer 배치 검사\n";

	// 모듈별 입력값 dword 수: C++ 배치 규칙 == 셰이더 모듈 함수가 읽는 번호
	for (uint32_t i = 0; i < uint32_t(ModuleId::Count); ++i)
	{
		const uint32_t cpp = MakeUberParamRecord(References()[i]).dwords;
		if (cpp != kExpectedRecordDwords[i])
		{
			log += std::string("  [불일치] ") + ModuleIdName(ModuleId(i)) + " 입력값 dword: C++ " + std::to_string(cpp)
				+ " / 셰이더 " + std::to_string(kExpectedRecordDwords[i]) + "\n";
			ok = false;
		}
	}

	for (const Shader& sh : shaders)
	{
		ConstantBufferInfo info;

		// ---- EmitterLayout (b2) ----
		if (!compiler.ReflectConstantBuffer(sh.data, sh.size, "EmitterLayout", info))
		{
			log += std::string("  ") + sh.name + ": EmitterLayout 반사 정보를 읽지 못함 (검사 건너뜀)\n";
			continue;
		}
		if (info.size != sizeof(EmitterLayoutCB))
		{
			log += std::string("  [불일치 ") + sh.name + "] EmitterLayout 크기: C++ " + std::to_string(sizeof(EmitterLayoutCB))
				+ " / 셰이더 " + std::to_string(info.size) + "\n";
			ok = false;
		}
		for (const Field& f : layoutFields)
		{
			const ConstantBufferInfo::Variable* v = findVar(info, f.hlsl);
			if (!v || v->offset != f.offset)
			{
				log += std::string("  [불일치 ") + sh.name + "] " + f.hlsl + ": C++ " + std::to_string(f.offset)
					+ " / 셰이더 " + (v ? std::to_string(v->offset) : std::string("없음")) + "\n";
				ok = false;
			}
		}
	}

	log += ok ? "  -> 일치\n" : "  -> 불일치! ParticleUber.hlsli 와 UberEmitterData 를 같이 고칠 것\n";
	return ok;
}
