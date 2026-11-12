#include "pch.h"
#include "EmitterGpuProgram.h"

#include "SystemTime.h"
#include <fstream>

namespace
{
	EmitterGpuProgram::Stats s_Stats;

	const char* kEntryNames[EmitterGpuProgram::kNumKernels] =
	{
		"EmitterUpdateMain",
		"ParticleUpdateMain",
		"ParticleSpawnMain"
	};

	const wchar_t* kPsoNames[EmitterGpuProgram::kNumKernels] =
	{
		L"Emitter Update PSO",
		L"Particle Update PSO",
		L"Particle Spawn PSO"
	};

	bool			s_RootSigReady = false;
	RootSignature	s_RootSig;

	void InitializeRootSignature()
	{
		if (s_RootSigReady)
			return;

		s_RootSig.Reset(EmitterGpuProgram::kNumRootParams, 0);
		s_RootSig[EmitterGpuProgram::kRootEngineCB].InitAsConstantBuffer(0);   // b0
		s_RootSig[EmitterGpuProgram::kRootModuleCB].InitAsConstantBuffer(1);   // b1
		s_RootSig[EmitterGpuProgram::kRootUAVs].InitAsDescriptorRange(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 0, EmitterGpuProgram::kNumUAVs);   // u0~u4
		s_RootSig[EmitterGpuProgram::kRootLayoutCB].InitAsConstantBuffer(2);   // b2  

		s_RootSig.Finalize(L"Emitter Compute RootSig");

		s_RootSigReady = true;
	}

	// 생성된 소스를 파일로 남긴다. 실패해도 진행한다.
	void DumpSource(const std::string& fileName, const std::string& source, std::string& log)
	{
		CreateDirectoryW(L"Generated", nullptr);
		std::ofstream f("Generated/" + fileName, std::ios::binary);
		if (f)
		{
			f << source;
			log += " 소스 저장: Generated/" + fileName + "\n";
		}
	}
}

const EmitterGpuProgram::Stats& EmitterGpuProgram::GetStats()
{
	return s_Stats;
}

void EmitterGpuProgram::ResetBuildStats()
{
	s_Stats.buildMs = 0.0;
	s_Stats.builds = 0;
}

bool EmitterGpuProgram::Build(const CompiledEmitter& compiled, const ShaderCompiler& compiler,
	const std::string& name)
{
	Shutdown();
	m_Log = "[EmitterGpuProgram]" + name + "\n";

	// 시간 측정
	const int64_t startTick = SystemTime::GetCurrentTick();

	if (!compiler.Ready())
	{
		m_Log += "   ShaderCompiler가 준비되지 않음\n";
		return false;
	}

	// --------- 1. 셰이더 문자열 만들기 ----------
	const std::string fileName = name + ".generated.hlsl";
	m_Generated = GenerateEmitterShader(compiled, fileName);

	for (const std::string& warning : m_Generated.warnings)
		m_Log += "  [생성 경고] " + warning + "\n";
	for (const std::string& error : m_Generated.errors)
		m_Log += "  [생성 에러] " + error + "\n";
	if (!m_Generated.Ok())
		return false;

	DumpSource(fileName, m_Generated.source, m_Log);

	bool allOk = true;
	// --------- 2. 세 단계의 과정 컴파일 ---------
	for (int i = 0; i < kNumKernels; ++i)
	{
		const ShaderCompileResult r = compiler.Compile(m_Generated.source,
			fileName, kEntryNames[i]);

		if (!r.messages.empty())
			m_Log += std::string("[DXC ") + kEntryNames[i] + "]\n" + r.messages + "\n";
		
		if (!r.ok)
		{
			allOk = false;
			continue;
		}

		// ConstantBuffer의 배치표가 같은지 확인
		ConstantBufferInfo info;
		if (compiler.ReflectConstantBuffer(r, "ModuleParams", info))
		{
			if (!CheckModuleParams(compiled, info, kEntryNames[i]))
				allOk = false;
		}

		m_ByteCode[i] = r.bytecode;
	}
	if (!allOk)
		return false;

	// ---------- 3. PSO 설정 ----------
	InitializeRootSignature();
	for (int i = 0; i < kNumKernels; ++i)
	{
		m_PSO[i] = ComputePSO(kPsoNames[i]);
		m_PSO[i].SetRootSignature(s_RootSig);
		m_PSO[i].SetComputeShader(m_ByteCode[i].data(), m_ByteCode[i].size());
		m_PSO[i].Finalize();
	}

	m_Log += "  PSO 3개 생성 완료\n";
	m_Ready = true;

	++s_Stats.livePrograms;
	++s_Stats.builds;
	++s_Stats.lifetimeBuilds;
	s_Stats.buildMs += SystemTime::TimeBetweenTicks(startTick, SystemTime::GetCurrentTick()) * 1000.0;

	return true;
}


void EmitterGpuProgram::Shutdown()
{
	if (m_Ready && s_Stats.livePrograms > 0)
		--s_Stats.livePrograms;

	for (int i = 0; i < kNumKernels; ++i)
	{
		m_PSO[i] = ComputePSO(kPsoNames[i]);
		m_ByteCode[i].clear();
	}
	m_Generated = GeneratedShader();
	m_Ready = false;
}

const RootSignature& EmitterGpuProgram::RootSig()
{
	InitializeRootSignature();
	return s_RootSig;
}

bool EmitterGpuProgram::CheckModuleParams(const CompiledEmitter& compiled, const ConstantBufferInfo& info, const char* entry)
{
	const Block& params = compiled.moduleParams;
	if (params.Slots().empty())
		return true;   // 입력값이 없으면 생성기가 자리채움 변수만 넣는다

	bool ok = true;
	if (info.size != params.ConstantBufferSize())
	{
		m_Log += std::string("  [배치 불일치 ") + entry + "] ModuleParams 크기: CPU " + std::to_string(params.ConstantBufferSize())
			+ " / DXC " + std::to_string(info.size) + "\n";
		ok = false;
	}

	for (const Slot& s : params.Slots())
	{
		const std::string var = ShaderNames::Param(s.key);
		const ConstantBufferInfo::Variable* found = nullptr;
		for (const ConstantBufferInfo::Variable& v : info.variables)
			if (v.name == var) { found = &v; break; }

		if (!found)
		{
			m_Log += std::string("  [배치 불일치 ") + entry + "] " + var + " 가 DXC 반사 정보에 없음\n";
			ok = false;
		}
		else if (found->offset != s.offset)
		{
			m_Log += std::string("  [배치 불일치 ") + entry + "] " + var + ": CPU offset " + std::to_string(s.offset)
				+ " / DXC " + std::to_string(found->offset) + "\n";
			ok = false;
		}
	}
	return ok;
}
