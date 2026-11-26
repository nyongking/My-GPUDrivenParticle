#pragma once
#include "MaterialShaderGenerator.h"
#include "ShaderCompiler.h"

#include "PipelineState.h"

#include <memory>
#include <string>

// 머티리얼을 그리는 방식 (전역 설정, Sim 의 GpuSimMode 와 같은 역할)
//   Interpreted = ParticleSpritePS 하나 (EffectMode 분기 / MaterialVM 해석)
//   Generated   = 그래프마다 만든 픽셀 셰이더
// 공용 렌더 목록(ParticleRenderList)은 이 설정과 상관없이 항상 Interpreted 로 그린다
enum class MaterialShaderMode { Interpreted, Generated };

// 생성된 머티리얼 픽셀 셰이더 하나 + 그 PSO (EmitterGpuProgram 의 머티리얼 판)
class MaterialGpuProgram
{
public:
	struct Stats
	{
		uint32_t programs = 0;		// 지금 캐시에 있는 프로그램 수
		uint32_t builds = 0;		// 실제로 컴파일한 횟수
		double   buildMs = 0.0;		// 컴파일에 쓴 시간 합
		uint32_t cacheHits = 0;		// 이미 있는 셰이더를 그대로 쓴 횟수
	};
	static const Stats& GetStats();

	static MaterialShaderMode Mode();
	static void SetMode(MaterialShaderMode mode);
	static const char* ModeName(MaterialShaderMode mode);

	// 소스가 같은 프로그램이 이미 있으면 그것을, 없으면 컴파일해서 돌려준다. 실패하면 nullptr (log 에 이유)
	//   한 번 만든 프로그램은 프로그램이 끝날 때까지 캐시에 남는다:
	//   MiniEngine 의 PSO 해시가 셰이더 바이트코드의 "주소 + 크기" 를 쓰므로 바이트코드를 해제하면 안 된다
	static std::shared_ptr<MaterialGpuProgram> Acquire(const GeneratedShader& generated,
		const ShaderCompiler& compiler, const std::string& name, std::string& log);
	// 캐시만 확인 (컴파일러가 필요 없다). 없으면 nullptr
	static std::shared_ptr<MaterialGpuProgram> Find(const std::string& source);
	static void ClearCache();	// 종료할 때

	MaterialGpuProgram() = default;
	MaterialGpuProgram(const MaterialGpuProgram&) = delete;
	MaterialGpuProgram& operator=(const MaterialGpuProgram&) = delete;

	bool Ready() const { return m_Ready; }
	const GraphicsPSO& PSO() const { return m_PSO; }
	const std::string& Source() const { return m_Source; }
	const std::string& Log() const { return m_Log; }

private:
	bool Build(const GeneratedShader& generated, const ShaderCompiler& compiler, const std::string& name);

	std::string          m_Source;
	std::vector<uint8_t> m_ByteCode;
	GraphicsPSO          m_PSO;
	bool                 m_Ready = false;
	std::string          m_Log;
};
