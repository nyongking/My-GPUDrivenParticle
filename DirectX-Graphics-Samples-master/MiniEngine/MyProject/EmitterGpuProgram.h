#pragma once
#include "CompiledEmitter.h"
#include "ShaderGenerator.h"
#include "ShaderCompiler.h"

#include "PipelineState.h"
#include "RootSignature.h"

#include <string>
#include <vector>

class EmitterGpuProgram
{
public:
	// 각 단계의 main 번호
	enum Kernel { kEmitterUpdate, kParticleUpdate, kParticleSpawn, kNumKernels };
	// kRootModuleData = t0 루트 SRV (공유 셰이더의 모듈 인스턴스 + 입력값). 생성 방식 셰이더는 쓰지 않는다
	// kRootCurveData  = t1 루트 SRV (전역 커브 버퍼, CurveLibrary). 두 방식이 같이 쓴다
	enum RootParam { kRootEngineCB, kRootModuleCB, kRootUAVs, kRootLayoutCB, kRootModuleData, kRootCurveData, kNumRootParams };
	// u0 ParticlesIn, u1 ParticlesOut, u2 Counters, u3 EmitterData, u4 IndirectArgs, u5 RenderIn, u6 RenderOut
	static const uint32_t kNumUAVs = 7;

	struct Stats
	{
		uint32_t livePrograms = 0;
		uint32_t builds = 0;
		double buildMs = 0.0;
		uint32_t lifetimeBuilds = 0;
	};
	static const Stats& GetStats();
	static void ResetBuildStats();

	EmitterGpuProgram() = default;
	~EmitterGpuProgram() { Shutdown(); }
	EmitterGpuProgram(const EmitterGpuProgram&) = delete;
	EmitterGpuProgram& operator=(const EmitterGpuProgram&) = delete;
	
	bool Build(const CompiledEmitter& compiled, const ShaderCompiler& compiler,
		const std::string& name);
	void Shutdown();

	bool Ready() const { return m_Ready; }

	const ComputePSO& PSO(Kernel k) const { return m_PSO[k]; }
	static const RootSignature& RootSig();

	const std::string& Source() const { return m_Generated.source; }
	const std::string& Log() const { return m_Log; }

private:
	bool CheckModuleParams(const CompiledEmitter& compiled, const ConstantBufferInfo& info,
		const char* entry);

	GeneratedShader		m_Generated;
	std::vector<uint8_t> m_ByteCode[kNumKernels];
	ComputePSO			m_PSO[kNumKernels];
	bool				m_Ready = false;
	std::string			m_Log;
};

