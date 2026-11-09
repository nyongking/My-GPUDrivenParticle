#include "pch.h"
#include "ShaderGenerator.h"

#include <regex>
#include <set>
#include <sstream>

namespace
{
	std::string DotsToUnderscore(std::string s)
	{
		for (char& c : s) if (c == '.') c = '_';
		return s;
	}

	// 모듈 이름 같은 사용자 문자열을 주석에 넣을 때 (ASCII 아닌 문자는 '?')
	std::string AsciiOnly(const std::string& s)
	{
		std::string o;
		for (unsigned char c : s) o += (c < 128 && c != '\n' && c != '\r') ? char(c) : '?';
		return o;
	}
}

namespace ShaderNames
{
	std::string Field(const AttributeKey& key)
	{
		std::string prefix;
		if (key.mod == Modifier::Previous)
			prefix = "Previous_";
		else if (key.mod == Modifier::Initial)
			prefix = "Initial_";

		return prefix + DotsToUnderscore(key.name);
	}

	std::string OffsetConst(const AttributeKey& key)
	{
		return "OFS_" + DotsToUnderscore(key.Full());
	}

	std::string Param(const AttributeKey& key)
	{
		return DotsToUnderscore(key.name);
	}

	const char* HlslType(const TypeInfo* type)
	{
		if (type == &Types::Float)
			return "float";
		if (type == &Types::Float2)
			return "float2";
		if (type == &Types::Float3)
			return "float3";
		if (type == &Types::Float4)
			return "float4";
		if (type == &Types::Int32)
			return "int";
		if (type == &Types::UInt32)
			return "uint";
		if (type == &Types::Bool)
			return "uint";
		return nullptr;
	}

}

namespace
{
	// 아래는 hlsl에 쓰기 편하게 하기 위한 Helper 함수들
	std::string Pad(const std::string& s, size_t width)
	{
		return s.size() >= width ? s : s + std::string(width - s.size(), ' ');
	}

	const char* LoadSuffix(const TypeInfo* t)
	{
		if (t == &Types::Float)  return "F1";
		if (t == &Types::Float2) return "F2";
		if (t == &Types::Float3) return "F3";
		if (t == &Types::Float4) return "F4";
		if (t == &Types::Int32)  return "I1";
		if (t == &Types::UInt32 || t == &Types::Bool) return "U1";
		return nullptr;
	}

	bool IsFloatType(const TypeInfo* t)
	{
		return t == &Types::Float || t == &Types::Float2 || t == &Types::Float3 || t == &Types::Float4;
	}

	std::string LoadN(const TypeInfo* t)
	{
		const uint32_t n = t->size / 4;
		return n <= 1 ? "Load" : "Load" + std::to_string(n);
	}

	std::string StoreN(const TypeInfo* t)
	{
		const uint32_t n = t->size / 4;
		return n <= 1 ? "Store" : "Store" + std::to_string(n);
	}

	class Writer
	{
	public:
		void Line(const std::string& s = "")
		{
			m_Text += s;
			m_Text += '\n';
			++m_Lines;
		}
		// 다음에 쓸 줄의 번호 (1 부터)
		int NextLineNumber() const { return m_Lines + 1; }
		const std::string& Text() const { return m_Text; }

	private:
		std::string m_Text;
		int         m_Lines = 0;
	};



	// 앞뒤 빈줄 제거, 다듬기
	std::vector<std::string> SplitFragment(const std::string& hlsl)
	{
		std::vector<std::string> lines;
		std::stringstream ss(hlsl);
		std::string l;
		while (std::getline(ss, l))
		{
			if (!l.empty() && l.back() == '\r') 
				l.pop_back();
			lines.push_back(l);
		}
		auto blank = [](const std::string& s) { return s.find_first_not_of(" \t") == std::string::npos; };
		while (!lines.empty() && blank(lines.front())) lines.erase(lines.begin());
		while (!lines.empty() && blank(lines.back()))  lines.pop_back();
		return lines;
	}

	// 주석 제거
	std::string StripComments(const std::string& s)
	{
		std::string o;
		o.reserve(s.size());
		for (size_t i = 0; i < s.size(); ++i)
		{
			if (s[i] == '/' && i + 1 < s.size() && s[i + 1] == '/')
			{
				while (i < s.size() && s[i] != '\n') ++i;
				if (i < s.size()) o += '\n';
			}
			else if (s[i] == '/' && i + 1 < s.size() && s[i + 1] == '*')
			{
				i += 2;
				while (i + 1 < s.size() && !(s[i] == '*' && s[i + 1] == '/')) { if (s[i] == '\n') o += '\n'; ++i; }
				++i;
				o += ' ';
			}
			else o += s[i];
		}
		return o;
	}

	std::set<std::string> FindAll(const std::string& src, const std::regex& re)
	{
		std::set<std::string> found;
		for (std::sregex_iterator it(src.begin(), src.end(), re), end; it != end; ++it)
			found.insert((*it)[1].str());
		return found;
	}

	void ValidateModule(const CompiledModule& m, GeneratedShader& out)
	{
		const std::string who = "[" + m.scope + "] " + m.name;
		const std::string src = StripComments(m.hlsl);

		std::set<std::string> declParticles, declParticleWrites, declEmitter, declEmitterWrites, declEngine;
		auto collect = [&](const std::vector<Usage>& list, bool isWrite)
			{
				for (const Usage& u : list)
				{
					const AttributeKey k = AttributeKey::Parse(u.attr);
					if (k.ns == Namespace::Particles)
					{
						declParticles.insert(ShaderNames::Field(k));
						if (isWrite) declParticleWrites.insert(ShaderNames::Field(k));
					}
					if (k.ns == Namespace::Emitter)
					{
						declEmitter.insert(ShaderNames::Field(k));
						if (isWrite) declEmitterWrites.insert(ShaderNames::Field(k));
					}
					if (k.ns == Namespace::Engine) declEngine.insert(k.Full());
				}
			};
		collect(m.reads, false);
		collect(m.writes, true);

		// ??
		static const std::regex reP(R"(\bp\.(\w+))");
		static const std::regex reE(R"(\be\.(\w+))");
		static const std::regex rePW(R"(\bp\.(\w+)(?:\.\w+)?\s*(?:[-+*/%&|^]?=(?!=)|\+\+|--))");
		static const std::regex reEW(R"(\be\.(\w+)(?:\.\w+)?\s*(?:[-+*/%&|^]?=(?!=)|\+\+|--))");
		static const std::regex reParam(R"(\bPARAM\s*\(\s*(\w+)\s*\))");
		static const std::regex reKill(R"(\bKILL\s*\()");
		static const std::regex reIndex(R"(\bindex\b)");
		static const std::regex reDt(R"(\bDeltaTime\b)");
		static const std::regex reOwner(R"(\bOwnerPosition\b)");

		const std::set<std::string> usedP = FindAll(src, reP);
		const std::set<std::string> usedE = FindAll(src, reE);
		const std::set<std::string> usedPW = FindAll(src, rePW);
		const std::set<std::string> usedEW = FindAll(src, reEW);
		const std::set<std::string> usedParams = FindAll(src, reParam);
		const bool usesKill = std::regex_search(src, reKill);
		const bool usesIndex = std::regex_search(src, reIndex);

		for (const std::string& f : usedP)
			if (!declParticles.count(f))
				out.errors.push_back(who + ": 조각이 p." + f + " 를 쓰는데 reads/writes 에 선언이 없음");
		for (const std::string& f : usedPW)
			if (declParticles.count(f) && !declParticleWrites.count(f))
				out.errors.push_back(who + ": 조각이 p." + f + " 에 값을 넣는데 writes 에 선언이 없음 (reads 에만 있음)");
		for (const std::string& f : usedE)
			if (!declEmitter.count(f))
				out.errors.push_back(who + ": 조각이 e." + f + " 를 쓰는데 reads/writes 에 선언이 없음");
		for (const std::string& f : usedEW)
			if (declEmitter.count(f) && !declEmitterWrites.count(f))
				out.errors.push_back(who + ": 조각이 e." + f + " 에 값을 넣는데 writes 에 선언이 없음 (reads 에만 있음)");

		for (const std::string& n : usedParams)
			if (std::find(m.params.begin(), m.params.end(), n) == m.params.end())
				out.errors.push_back(who + ": PARAM(" + n + ") 는 이 모듈의 params 에 없음");
		for (const std::string& n : m.params)
			if (!usedParams.count(n))
				out.warnings.push_back(who + ": 입력값 '" + n + "' 를 조각에서 쓰지 않음");

		// 엔진 값은 선언하지 않아도 동작은 하지만, CPU 와 선언을 맞추라는 뜻으로 경고
		if (std::regex_search(src, reDt) && !declEngine.count("Engine.DeltaTime"))
			out.warnings.push_back(who + ": DeltaTime 을 쓰는데 reads 에 Engine.DeltaTime 선언이 없음");
		if (std::regex_search(src, reOwner) && !declEngine.count("Engine.Owner.Position"))
			out.warnings.push_back(who + ": OwnerPosition 을 쓰는데 reads 에 Engine.Owner.Position 선언이 없음");

		// 단계별 규칙 (단계 함수의 인자에 없는 것은 쓸 수 없다)
		switch (m.stage)
		{
		case Stage::EmitterUpdate:
			if (!usedP.empty()) out.errors.push_back(who + ": EmitterUpdate 단계에는 파티클(p.) 이 없음");
			if (usesIndex)      out.errors.push_back(who + ": EmitterUpdate 단계에는 index 가 없음");
			if (usesKill)       out.errors.push_back(who + ": KILL() 은 ParticleUpdate 단계에서만 쓸 수 있음");
			break;
		case Stage::ParticleSpawn:
			if (!usedE.empty()) out.errors.push_back(who + ": ParticleSpawn 조각에서는 아직 e. 를 쓸 수 없음 (GPU 에서 Emitter 값을 넘기지 않음)");
			if (usesKill)       out.errors.push_back(who + ": KILL() 은 ParticleUpdate 단계에서만 쓸 수 있음");
			break;
		case Stage::ParticleUpdate:
			if (!usedE.empty()) out.errors.push_back(who + ": ParticleUpdate 조각에서는 아직 e. 를 쓸 수 없음 (GPU 에서 Emitter 값을 넘기지 않음)");
			break;
		default:
			break;
		}

		// 한글 등 ASCII 아닌 문자 (DXC 는 UTF-8 로 읽음)
		for (unsigned char c : m.hlsl)
			if (c >= 128) { out.errors.push_back(who + ": 조각에 ASCII 가 아닌 문자가 있음 (주석의 한글 포함)"); break; }
	}

	// 모듈 함수 하나를 쓰는 함수
	void WriteModuleBlock(Writer& w, const CompiledModule& m, const std::string& fileName)
	{
		const std::string index = m.scope.substr(0, m.scope.find('_')); // M1, M2와 같은 형태
		w.Line("    // ---- [" + index + "] " + AsciiOnly(m.name) + "----");
		// 시작지점
		w.Line("    {");
		w.Line("#define PARAM(n) " + m.scope + "_##n");
		w.Line("#line 1 \"" + m.scope + "\"");
		for (const std::string& l : SplitFragment(m.hlsl))
			w.Line(l.empty() ? "" : "        " + l);   // 들여쓰기 8칸 (에러의 열 번호는 +8)
		// #line N : 바로 다음 줄이 N 번째 줄. 지금 쓰는 줄이 NextLineNumber() 이므로 +1
		w.Line("#line " + std::to_string(w.NextLineNumber() + 1) + " \"" + fileName + "\"");
		w.Line("#undef PARAM");
		w.Line("    }");
	}

	void WriteCopies(Writer& w, const std::vector<CopyOp>& ops, const char* what)
	{
		if (ops.empty())
		{
			w.Line(std::string("    // ---- [") + what + "copy] none ----");
			return;
		}
		w.Line(std::string("    // ---- [") + what + " copy] (engine) ----");
		for (const CopyOp& op : ops)
		{
			const AttributeKey to = AttributeKey::Parse(op.to);
			const AttributeKey from = AttributeKey::Parse(op.from);
			w.Line("    p." + ShaderNames::Field(to) + " = p." + ShaderNames::Field(from) + ";");
		}
	}
}


GeneratedShader GenerateEmitterShader(const CompiledEmitter& compiled, const std::string& fileName)
{
	GeneratedShader out;
	out.fileName = fileName;

	if (!compiled.Ok())
	{
		out.errors.push_back("CompiledEmitter 에 에러가 있어서 셰이더를 만들 수 없음");
		return out;
	}

	// ---------- 검사 ----------
	auto checkType = [&](const AttributeKey& k, const TypeInfo* t)
		{
			if (!ShaderNames::HlslType(t))
				out.errors.push_back("'" + k.Full() + "' 타입 " + (t ? t->name : std::string("null")) + " 은 HLSL 로 옮길 수 없음");
		};
	for (const DeclaredAttribute& d : compiled.declaredParticles) checkType(d.key, d.type);
	for (const DeclaredAttribute& d : compiled.declaredEmitter)   checkType(d.key, d.type);
	for (const Slot& s : compiled.moduleParams.Slots())           checkType(s.key, s.type);

	for (const CompiledModule& m : compiled.modules)
	{
		const bool gpuStage = m.stage == Stage::EmitterUpdate || m.stage == Stage::ParticleSpawn || m.stage == Stage::ParticleUpdate;
		if (m.hlsl.find_first_not_of(" \t\r\n") == std::string::npos)
		{
			out.warnings.push_back("[" + m.scope + "] " + m.name + ": hlsl 조각이 비어 있음 -> GPU 셰이더에서 빠짐 (CPU 와 결과가 달라짐)");
			continue;
		}
		if (!gpuStage)
		{
			out.errors.push_back("[" + m.scope + "] " + m.name + ": " + StageName(m.stage) + " 단계는 아직 GPU 셰이더에서 지원하지 않음");
			continue;
		}
		ValidateModule(m, out);
	}

	// 생성 요청 수는 Emitter.SpawnCount 에서 나온다
	const Slot* spawnCount = compiled.emitterParams.Find("Emitter.SpawnCount");
	if (spawnCount && spawnCount->type != &Types::UInt32 && spawnCount->type != &Types::Int32)
		out.errors.push_back("Emitter.SpawnCount 는 uint 또는 int 여야 함 (지금: " + spawnCount->type->name + ")");
	if (!spawnCount)
		out.warnings.push_back("Emitter.SpawnCount 를 쓰는 모듈이 없음 -> 파티클이 생기지 않음");

	if (!out.Ok()) 
		return out;


	// ----------------------진짜 쓰기 시작-----------------------
	Writer w;
	const std::string bar = "// =====================================================================";
	w.Line(bar);
	w.Line("// " + AsciiOnly(fileName) + "  (generated by ShaderGenerator - do not edit)");
	w.Line("//   modules:");
	for (const CompiledModule& m : compiled.modules)
		w.Line("//     " + Pad(m.scope, 28) + Pad(StageName(m.stage), 16) + (m.hlsl.empty() ? "(no hlsl, skipped)" : ""));
	w.Line(bar);
	w.Line();
	w.Line("#include \"ModuleCommon.hlsli\"");
	w.Line();

	// --------	[1] 레이아웃 -------
	w.Line(bar);
	w.Line("// 1. Layout offsets (particle buffer: SoA column start = offset * Capacity)");
	w.Line(bar);

	for (const Slot& s : compiled.particles.Slots())
		w.Line("static const uint " + Pad(ShaderNames::OffsetConst(s.key), 36) + " = " + std::to_string(s.offset) + ";   // " + ShaderNames::HlslType(s.type));
	for (const DeclaredAttribute& d : compiled.declaredParticles)
		if (!d.inLayout) w.Line("// " + d.key.Full() + " : trimmed (nobody reads it) -> no offset, still in struct");
	w.Line();
	for (const Slot& s : compiled.emitterParams.Slots())
		w.Line("static const uint " + Pad(ShaderNames::OffsetConst(s.key), 36) + " = " + std::to_string(s.offset) + ";   // " + ShaderNames::HlslType(s.type));
	w.Line();

	// --------	[2] 모듈 입력값 상수 버퍼 -------
	w.Line(bar);
	w.Line("// 2. Module params (b1). Declared in layout order -> HLSL packs to the same offsets");
	w.Line(bar);
	w.Line("cbuffer ModuleParams : register(b1)");
	w.Line("{");

	if (compiled.moduleParams.Slots().empty())
		w.Line("    uint _NoModuleParams;");
	for (const Slot& s : compiled.moduleParams.Slots())
		w.Line("    " + Pad(ShaderNames::HlslType(s.type), 7) + Pad(ShaderNames::Param(s.key) + ";", 40) + "// " + std::to_string(s.offset));
	w.Line("};");
	w.Line();


	// --------	[3] 구조체 -------
	w.Line(bar);
	w.Line("// 3. Structs");
	w.Line(bar);
	// -------- Particles 구조체 -------
	w.Line("struct Particle");
	w.Line("{");
	if (compiled.declaredParticles.empty()) w.Line("    uint _Empty;");
	for (const DeclaredAttribute& d : compiled.declaredParticles)
		w.Line("    " + Pad(ShaderNames::HlslType(d.type), 7) + ShaderNames::Field(d.key) + ";" + (d.inLayout ? "" : "   // trimmed"));
	w.Line("};");
	w.Line();
	// -------- Emitter 구조체 -------
	w.Line("struct EmitterState");
	w.Line("{");
	if (compiled.declaredEmitter.empty()) w.Line("    uint _Empty;");
	for (const DeclaredAttribute& d : compiled.declaredEmitter)
		w.Line("    " + Pad(ShaderNames::HlslType(d.type), 7) + ShaderNames::Field(d.key) + ";");
	w.Line("};");
	w.Line();

	// ------- [4] 읽기/저장 --------
	w.Line(bar);
	w.Line("// 4. Load / Store");
	w.Line(bar);
	w.Line("Particle LoadParticle(RWByteAddressBuffer b, uint i)");
	w.Line("{");
	w.Line("    Particle p = (Particle)0;");
	for (const Slot& s : compiled.particles.Slots())
		w.Line("    p." + Pad(ShaderNames::Field(s.key), 20) + " = Load" + LoadSuffix(s.type) + "(b, " + ShaderNames::OffsetConst(s.key) + ", i);");
	w.Line("    return p;");
	w.Line("}");
	w.Line();
	w.Line("void StoreParticle(RWByteAddressBuffer b, uint i, Particle p)");
	w.Line("{");
	for (const Slot& s : compiled.particles.Slots())
		w.Line(std::string("    Store") + LoadSuffix(s.type) + "(b, " + Pad(ShaderNames::OffsetConst(s.key) + ",", 38) + "i, p." + ShaderNames::Field(s.key) + ");");
	w.Line("}");
	w.Line();
	w.Line("EmitterState LoadEmitter()");
	w.Line("{");
	w.Line("    EmitterState e = (EmitterState)0;");
	for (const Slot& s : compiled.emitterParams.Slots())
	{
		const std::string raw = "EmitterData." + LoadN(s.type) + "(" + ShaderNames::OffsetConst(s.key) + ")";
		std::string expr = raw;
		if (IsFloatType(s.type))       expr = "asfloat(" + raw + ")";
		else if (s.type == &Types::Int32) expr = "asint(" + raw + ")";
		w.Line("    e." + Pad(ShaderNames::Field(s.key), 20) + " = " + expr + ";");
	}
	w.Line("    return e;");
	w.Line("}");
	w.Line();
	w.Line("void StoreEmitter(EmitterState e)");
	w.Line("{");
	for (const Slot& s : compiled.emitterParams.Slots())
		w.Line("    EmitterData." + StoreN(s.type) + "(" + Pad(ShaderNames::OffsetConst(s.key) + ",", 38) + "asuint(e." + ShaderNames::Field(s.key) + "));");
	w.Line("}");
	w.Line();

	// ------- [5] 각 단계별 모듈 함수 -------
	w.Line(bar);
	w.Line("// 5. Stage functions (module fragments in order)");
	w.Line(bar);

	// 각 단계별 모듈 함수를 쓰는 람다
	auto writeStage = [&](Stage stage)
		{
			for (const CompiledModule& m : compiled.modules)
			{
				if (m.stage == stage && !SplitFragment(m.hlsl).empty())
					WriteModuleBlock(w, m, fileName);
			}
		};

	w.Line("void EmitterUpdate(inout EmitterState e)");
	w.Line("{");
	writeStage(Stage::EmitterUpdate);
	w.Line("}");
	w.Line();

	w.Line("void ParticleSpawn(inout Particle p, uint index)");
	w.Line("{");
	writeStage(Stage::ParticleSpawn);
	WriteCopies(w, compiled.copyAtSpawnEnd, "Initial"); // Spawn 끝난 값이 initial
	w.Line("}");
	w.Line();

	w.Line("void ParticleUpdate(inout Particle p, uint index, inout bool alive)");
	w.Line("{");
	WriteCopies(w, compiled.copyAtUpdateStart, "Previous"); // update 이전 값이 previous
	writeStage(Stage::ParticleUpdate);
	w.Line("}");
	w.Line();

	// ------- [6] 각 단계 고정 함수 -------
	w.Line(bar);
	w.Line("// 6. Fixed shape function");
	w.Line(bar);
	
	// ----- Emitter Update -----
	w.Line("[numthreads(1, 1, 1)]");
	w.Line("void EmitterUpdateMain()");
	w.Line("{");
	w.Line("    EmitterState e = LoadEmitter();");
	if (spawnCount)
		w.Line("    e.SpawnCount = 0;   // spawn modules accumulate with +=");
	w.Line("    EmitterUpdate(e);");
	w.Line("    StoreEmitter(e);");
	w.Line();
	if (!spawnCount)
		w.Line("    const uint request = 0;   // no module writes Emitter.SpawnCount");
	else if (spawnCount->type == &Types::Int32)
		w.Line("    const uint request = min((uint)max(e.SpawnCount, 0), Capacity);");
	else
		w.Line("    const uint request = min(e.SpawnCount, Capacity);");
	w.Line("    Counters.Store(COUNTER_SPAWN_REQUEST, request);");
	w.Line("    IndirectArgs.Store3(0, uint3((request + GROUP_SIZE - 1) / GROUP_SIZE, 1, 1));");
	w.Line("}");
	w.Line();

	// ----- Particle Update -----
	w.Line("[numthreads(GROUP_SIZE, 1, 1)]");
	w.Line("void ParticleUpdateMain(uint3 dtid : SV_DispatchThreadID)");
	w.Line("{");
	w.Line("    const uint i = dtid.x;");
	w.Line("    if (i >= Counters.Load(COUNTER_ALIVE_IN))");
	w.Line("        return;");
	w.Line();
	w.Line("    Particle p = LoadParticle(ParticlesIn, i);");
	w.Line("    bool alive = true;");
	w.Line("    ParticleUpdate(p, i, alive);");
	w.Line("    if (!alive)");
	w.Line("        return;   // not written to Out = compaction");
	w.Line();
	w.Line("    uint dst;");
	w.Line("    Counters.InterlockedAdd(COUNTER_WRITE_CURSOR, 1, dst);");
	w.Line("    StoreParticle(ParticlesOut, dst, p);");
	w.Line("}");
	w.Line();

	// ----- Particle Spawn -----
	w.Line("[numthreads(GROUP_SIZE, 1, 1)]");
	w.Line("void ParticleSpawnMain(uint3 dtid : SV_DispatchThreadID)");
	w.Line("{");
	w.Line("    if (dtid.x >= Counters.Load(COUNTER_SPAWN_REQUEST))");
	w.Line("        return;");
	w.Line();
	w.Line("    uint dst;");
	w.Line("    Counters.InterlockedAdd(COUNTER_WRITE_CURSOR, 1, dst);");
	w.Line("    if (dst >= Capacity)");
	w.Line("        return;   // overflow is clamped by Finalize");
	w.Line();
	w.Line("    Particle p = (Particle)0;   // new particles start from 0 (CPU ZeroRange)");
	w.Line("    ParticleSpawn(p, dst);      // random index = slot index (same as CPU)");
	w.Line("    StoreParticle(ParticlesOut, dst, p);");
	w.Line("}");

	out.source = w.Text();
	return out;
}
