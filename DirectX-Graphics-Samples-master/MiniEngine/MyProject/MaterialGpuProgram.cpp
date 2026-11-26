#include "pch.h"
#include "MaterialGpuProgram.h"

#include "ParticleMaterial.h"
#include "SpriteRenderer.h"
#include "SystemTime.h"

#include <fstream>
#include <unordered_map>

namespace
{
	MaterialGpuProgram::Stats s_Stats;
	MaterialShaderMode s_Mode = MaterialShaderMode::Generated;

	// 키 = 생성된 소스 전체
	std::unordered_map<std::string, std::shared_ptr<MaterialGpuProgram>> s_Cache;

	// 생성한 소스를 파일로 남긴다. 실패해도 무시한다.
	void DumpSource(const std::string& fileName, const std::string& source, std::string& log)
	{
		CreateDirectoryW(L"Generated", nullptr);
		std::ofstream f("Generated/" + fileName, std::ios::binary);
		if (f)
		{
			f << source;
			log += "  소스 저장: Generated/" + fileName + "\n";
		}
	}
}

const MaterialGpuProgram::Stats& MaterialGpuProgram::GetStats()
{
	return s_Stats;
}

MaterialShaderMode MaterialGpuProgram::Mode()
{
	return s_Mode;
}

void MaterialGpuProgram::SetMode(MaterialShaderMode mode)
{
	s_Mode = mode;
}

const char* MaterialGpuProgram::ModeName(MaterialShaderMode mode)
{
	return mode == MaterialShaderMode::Generated ? "Generated" : "Interpreted";
}

std::shared_ptr<MaterialGpuProgram> MaterialGpuProgram::Acquire(const GeneratedShader& generated,
	const ShaderCompiler& compiler, const std::string& name, std::string& log)
{
	if (!generated.Ok())
	{
		for (const std::string& error : generated.errors)
			log += "  [생성 오류] " + error + "\n";
		return nullptr;
	}

	if (std::shared_ptr<MaterialGpuProgram> cached = Find(generated.source))
		return cached;

	std::shared_ptr<MaterialGpuProgram> program = std::make_shared<MaterialGpuProgram>();
	const bool built = program->Build(generated, compiler, name);
	log += program->Log();
	if (!built)
		return nullptr;

	s_Cache[generated.source] = program;
	s_Stats.programs = static_cast<uint32_t>(s_Cache.size());
	return program;
}

std::shared_ptr<MaterialGpuProgram> MaterialGpuProgram::Find(const std::string& source)
{
	auto it = s_Cache.find(source);
	if (it == s_Cache.end())
		return nullptr;

	++s_Stats.cacheHits;
	return it->second;
}

void MaterialGpuProgram::ClearCache()
{
	s_Cache.clear();
	s_Stats.programs = 0;
}

bool MaterialGpuProgram::Build(const GeneratedShader& generated, const ShaderCompiler& compiler, const std::string& name)
{
	m_Ready = false;
	m_Log = "[MaterialGpuProgram] " + name + "\n";

	const int64_t startTick = SystemTime::GetCurrentTick();

	if (!compiler.Ready())
	{
		m_Log += "  ShaderCompiler 가 준비되지 않음\n";
		return false;
	}

	for (const std::string& warning : generated.warnings)
		m_Log += "  [생성 경고] " + warning + "\n";

	m_Source = generated.source;
	const std::string fileName = name + ".material.generated.hlsl";
	DumpSource(fileName, m_Source, m_Log);

	// ---------- 1. 컴파일 ----------
	const ShaderCompileResult r = compiler.Compile(m_Source, fileName, "main", "ps_6_2");
	if (!r.messages.empty())
		m_Log += "[DXC main]\n" + r.messages + "\n";
	if (!r.ok)
		return false;

	// ---------- 2. 배치 확인: CPU 가 올리는 MaterialParams 크기와 같아야 한다 ----------
	//   값 칸을 하나도 안 쓰는 그래프는 cbuffer 가 지워져서 반사 정보에 없다 (그 경우는 확인할 것이 없음)
	ConstantBufferInfo info;
	if (compiler.ReflectConstantBuffer(r, "MaterialParamsCB", info))
	{
		const uint32_t expected = static_cast<uint32_t>(sizeof(float4)) * ParticleMaterial::kMaxParams;
		if (info.size != expected)
		{
			m_Log += "  [배치 불일치] MaterialParamsCB 크기: CPU " + std::to_string(expected)
				+ " / DXC " + std::to_string(info.size) + "\n";
			return false;
		}
	}

	// ---------- 3. PSO ----------
	m_ByteCode = r.bytecode;
	SpriteRenderer::CreateMaterialPSO(m_PSO, m_ByteCode.data(), m_ByteCode.size());

	m_Ready = true;
	++s_Stats.builds;
	s_Stats.buildMs += SystemTime::TimeBetweenTicks(startTick, SystemTime::GetCurrentTick()) * 1000.0;

	m_Log += "  PS 컴파일 + PSO 생성 완료\n";
	return true;
}
