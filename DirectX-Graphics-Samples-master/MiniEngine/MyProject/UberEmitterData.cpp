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
	constexpr uint32_t kExpectedRecordDwords[] = { 1, 20, 3, 17, 1, 3, 2, 4 };

	const Stage kUberStages[] = { Stage::EmitterUpdate, Stage::ParticleSpawn, Stage::ParticleUpdate };

	bool ValidId(ModuleId id) { return uint32_t(id) < kModuleCount; }

	// 고정 자리 Block 을 만들 기준 모듈들. ModuleId 순서 그대로 (값은 쓰지 않으므로 아무거나)
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
			SubUVAnimation(0.f, 0.f)
		};
		for (uint32_t i = 0; i < uint32_t(m.size()); ++i)
			ASSERT(uint32_t(m[i].id) == i, "ReferenceModules 순서가 ModuleId 와 다름");
		return m;
	}

	const std::vector<ModuleDesc>& References()
	{
		static const std::vector<ModuleDesc> sRef = ReferenceModules();
		return sRef;
	}

	void BuildModuleData(const std::vector<ModuleDesc>& modules, std::vector<uint8_t>& out,
		EmitterLayoutCB* L, std::vector<std::string>* errors)
	{
		std::vector<uint32_t> order;
		uint32_t begin[3] = {};
		uint32_t count[3] = {};
		for (uint32_t i = 0; i < 3; ++i)
		{
			begin[i] = uint32_t(order.size());
			for (uint32_t j = 0; j < modules.size(); ++j)
			{
				const ModuleDesc& m = modules[j];
				if (!ValidId(m.id) || kUberStages[i] != m.stage)
					continue;
				if (References()[uint32_t(m.id)].stage != m.stage)
					continue;

				order.push_back(j);

			}
			count[i] = uint32_t(order.size()) - begin[i];
		}

		if (errors)
		{
			for (const ModuleDesc& m : modules)
			{
				if (!ValidId(m.id))
				{
					errors->push_back(m.name + ": 공유 셰이더가 모르는 모듈, Id가 없음");
				}
				else if (References()[uint32_t(m.id)].stage != m.stage)
				{
					errors->push_back(m.name + "공유 셰이더가 이 모듈을 실행하는 단계와 다름");
				}
			}
		}

		uint32_t instCount = uint32_t(order.size());
		uint32_t total = instCount * 4;
		std::vector<UberParamRecord> records(instCount);
		for (uint32_t i = 0; i < instCount; ++i)
		{
			records[i] = MakeUberParamRecord()
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
		Particles::Mass, Particles::NormalizedAge, Particles::Velocity, Particles::Acceleration, Particles::SubImageIndex
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
		"ParticleState", "GravityForce", "SolveForceAndVelocity", "SubUVAnimation"
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
	UberParamRecord ret;
	for (const ParamDesc& desc : m.params)
	{
		ret.paramDword.push_back(ret.dwords);
		const uint32_t s = desc.type ? desc.type->size / 4 : 0;
		ret.dwords += IsUberRangeParam(m.stage, desc.type) ? s * 2 : s;
	}
	return ret;
}

const Block& UberParamBlock()
{
	static const Block s_Block = []()
		{
			Block b(Packing::ConstantBuffer);
			for (const ModuleDesc& m : ReferenceModules())
			{
				const std::string scope = UberScope(m.name);
				for (const ParamDesc& p : m.params)
					for (const std::string& slot : UberSlotNames(m, p))
						b.Add(AttributeKey::Parse("Module." + scope + "." + slot), p.type);
			}
			return b;
		}();
	return s_Block;
}

void FillUberParams(const std::vector<ModuleDesc>& modules, std::vector<uint8_t>& out, std::vector<std::string>* errors)
{
	const Block& block = UberParamBlock();
	out.assign(block.ConstantBufferSize(), 0);     // 스택에 없는 모듈의 자리는 0

	auto fail = [&](const std::string& msg) { if (errors) errors->push_back(msg); };

	for (const ModuleDesc& m : modules)
	{
		if (!ValidId(m.id))
			continue;   // BuildUberEmitterData 가 오류로 남긴다

		const std::string scope = UberScope(m.name);
		for (const ParamDesc& p : m.params)
		{
			const std::string who = m.name + " 입력값 '" + p.name + "'";
			if (p.type == nullptr)
				continue;   // EmitterCompiler 가 이미 오류로 남김

			const size_t typeSize = p.type->size;
			const size_t srcCount = ParamSlotNames(p.name, p.kind).size();     // Constant 1개 / RandomRange 2개
			if (p.value.size() != typeSize * srcCount)
				continue;   // EmitterCompiler 가 이미 오류로 남김

			// dst 자리에 src 의 i 번째 값을 쓴다
			auto write = [&](const std::string& slotName, size_t srcIndex)
				{
					const Slot* s = block.Find("Module." + scope + "." + slotName);
					if (s == nullptr) { fail(who + ": 공유 셰이더에 자리가 없음 (ParticleUber.hlsli 와 ModuleLibrary 가 다름)"); return; }
					if (s->type != p.type) { fail(who + ": 공유 셰이더의 자리 타입이 다름"); return; }
					std::memcpy(out.data() + s->offset, p.value.data() + srcIndex * typeSize, typeSize);
				};

			if (IsUberRangeParam(m.stage, p.type))
			{
				// 상수는 Min 과 Max 에 같은 값 -> 셰이더가 랜덤을 건너뛴다
				write(p.name + "_Min", 0);
				write(p.name + "_Max", srcCount > 1 ? 1 : 0);
			}
			else
			{
				write(p.name, 0);
			}
		}
	}
}

UberEmitterData BuildUberEmitterData(const std::vector<ModuleDesc>& modules, const CompiledEmitter& compiled)
{
	UberEmitterData d;
	EmitterLayoutCB& L = d.layout;

	// ---------- 1. 모듈: 마스크, 순번, 검사 ----------
	int lastIdInStage[6] = { -1, -1, -1, -1, -1, -1 };   // Stage 별로 마지막에 본 ModuleId
	for (size_t mi = 0; mi < modules.size(); ++mi)
	{
		const ModuleDesc& m = modules[mi];
		if (!ValidId(m.id))
		{
			d.errors.push_back(m.name + ": 공유 셰이더가 모르는 모듈 (ModuleDesc::id 가 없음)");
			continue;
		}

		const uint32_t bit = 1u << uint32_t(m.id);
		if (L.moduleMask & bit)
		{
			d.errors.push_back(m.name + ": 같은 모듈이 두 번 있음 (공유 셰이더에는 자리가 하나뿐)");
			continue;
		}
		L.moduleMask |= bit;

		// 실행 순서: 같은 단계 안에서 ModuleId 가 줄어들면 스택 순서와 실제 순서가 다르다
		int& last = lastIdInStage[int(m.stage)];
		if (int(m.id) < last)
			d.warnings.push_back(m.name + ": 스택 순서와 달리 공유 셰이더의 고정 순서로 실행됨 (" + ModuleIdName(ModuleId(last)) + " 보다 먼저)");
		last = (std::max)(last, int(m.id));

		// RandomRange salt 를 생성 방식과 같게 만들기 위한 스택 순번 (EmitterCompiler 의 mi 와 같다)
		if (m.id == ModuleId::InitializeParticle) L.slotInitializeParticle = uint32_t(mi);
		if (m.id == ModuleId::SphereLocation)     L.slotSphereLocation = uint32_t(mi);
		if (m.id == ModuleId::AddVelocityInCone)  L.slotAddVelocityInCone = uint32_t(mi);

		for (const ParamDesc& p : m.params)
		{
			if (p.kind != ParamKind::RandomRange || !p.perComponent)
				continue;
			if (m.id == ModuleId::InitializeParticle && p.name == "Pos")      L.perComponentMask |= Uber::kPC_InitializeParticlePos;
			if (m.id == ModuleId::InitializeParticle && p.name == "Color")    L.perComponentMask |= Uber::kPC_InitializeParticleColor;
			if (m.id == ModuleId::AddVelocityInCone && p.name == "ConeAxis") L.perComponentMask |= Uber::kPC_AddVelocityInConeAxis;
		}
	}

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

	// ---------- 4. 입력값 ----------
	FillUberParams(modules, d.params, &d.errors);
	return d;
}

std::string DescribeUber(const UberEmitterData& d)
{
	const EmitterLayoutCB& L = d.layout;
	char buf[128];
	std::string s = "[Uber] ";

	sprintf_s(buf, "moduleMask 0x%02X :", L.moduleMask);
	s += buf;
	for (uint32_t i = 0; i < kModuleCount; ++i)
		if (L.moduleMask & (1u << i))
			s += std::string(" ") + ModuleIdName(ModuleId(i));
	s += "\n";

	sprintf_s(buf, "  slot Init %u / Sphere %u / Cone %u, perComponent 0x%X\n",
		L.slotInitializeParticle, L.slotSphereLocation, L.slotAddVelocityInCone, L.perComponentMask);
	s += buf;

	s += "  Emitter  : SpawnCount " + OfsText(L.ofsEmitterSpawnCount) + ", SpawnRemainder " + OfsText(L.ofsEmitterSpawnRemainder) + "\n";
	s += "  Particles: Position " + OfsText(L.ofsPosition) + ", Color " + OfsText(L.ofsColor) + ", Size " + OfsText(L.ofsSize)
		+ ", Lifetime " + OfsText(L.ofsLifetime) + ", Age " + OfsText(L.ofsAge) + ", Mass " + OfsText(L.ofsMass)
		+ ", NormalizedAge " + OfsText(L.ofsNormalizedAge) + ", Velocity " + OfsText(L.ofsVelocity)
		+ ", Acceleration " + OfsText(L.ofsAcceleration) + ", SubImageIndex " + OfsText(L.ofsSubImageIndex) + "\n";

	// 입력값: 0 이 아닌 자리만
	sprintf_s(buf, "  params (%u bytes, 0 이 아닌 자리만):\n", uint32_t(d.params.size()));
	s += buf;
	for (const Slot& slot : UberParamBlock().Slots())
	{
		if (slot.offset + slot.type->size > d.params.size())
			continue;
		const uint8_t* p = d.params.data() + slot.offset;
		bool nonzero = false;
		for (uint32_t b = 0; b < slot.type->size; ++b)
			nonzero |= (p[b] != 0);
		if (!nonzero)
			continue;

		const bool isUint = std::strcmp(ShaderNames::HlslType(slot.type) ? ShaderNames::HlslType(slot.type) : "", "uint") == 0;
		s += "    " + ShaderNames::Param(slot.key) + " =";
		for (uint32_t c = 0; c < slot.type->size / 4; ++c)
		{
			if (isUint) { uint32_t v; std::memcpy(&v, p + c * 4, 4); sprintf_s(buf, " %u", v); }
			else { float v;    std::memcpy(&v, p + c * 4, 4); sprintf_s(buf, " %g", v); }
			s += buf;
		}
		s += "\n";
	}

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
		{ "ModuleMask",                 uint32_t(offsetof(EmitterLayoutCB, moduleMask)) },
		{ "PerComponentMask",           uint32_t(offsetof(EmitterLayoutCB, perComponentMask)) },
		{ "Slot_InitializeParticle",    uint32_t(offsetof(EmitterLayoutCB, slotInitializeParticle)) },
		{ "Slot_SphereLocation",        uint32_t(offsetof(EmitterLayoutCB, slotSphereLocation)) },
		{ "Slot_AddVelocityInCone",     uint32_t(offsetof(EmitterLayoutCB, slotAddVelocityInCone)) },
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
	};

	auto findVar = [](const ConstantBufferInfo& info, const std::string& name) -> const ConstantBufferInfo::Variable*
		{
			for (const ConstantBufferInfo::Variable& v : info.variables)
				if (v.name == name) return &v;
			return nullptr;
		};

	const Block& params = UberParamBlock();
	bool ok = true;
	log = "[Uber] cbuffer 배치 검사\n";

	for (const Shader& sh : shaders)
	{
		// ---- ModuleParams (b1) ----
		ConstantBufferInfo info;
		if (!compiler.ReflectConstantBuffer(sh.data, sh.size, "ModuleParams", info))
		{
			log += std::string("  ") + sh.name + ": ModuleParams 반사 정보를 읽지 못함 (검사 건너뜀)\n";
		}
		else
		{
			if (info.size != params.ConstantBufferSize())
			{
				log += std::string("  [불일치 ") + sh.name + "] ModuleParams 크기: C++ " + std::to_string(params.ConstantBufferSize())
					+ " / 셰이더 " + std::to_string(info.size) + "\n";
				ok = false;
			}
			for (const Slot& s : params.Slots())
			{
				const std::string var = ShaderNames::Param(s.key);
				const ConstantBufferInfo::Variable* v = findVar(info, var);
				if (!v)
				{
					log += std::string("  [불일치 ") + sh.name + "] " + var + " 가 셰이더에 없음\n";
					ok = false;
				}
				else if (v->offset != s.offset)
				{
					log += std::string("  [불일치 ") + sh.name + "] " + var + ": C++ " + std::to_string(s.offset)
						+ " / 셰이더 " + std::to_string(v->offset) + "\n";
					ok = false;
				}
			}
		}

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
