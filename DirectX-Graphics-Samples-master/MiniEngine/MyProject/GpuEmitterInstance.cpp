#include "pch.h"
#include "GpuEmitterInstance.h"

#include "EmitterInstance.h"
#include "ShaderGenerator.h"

#include "CommandContext.h"
#include "CommandListManager.h"
#include "GraphicsCore.h"

#include "CompiledShaders/EmitterFinalizeCS.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <set>
#include <sstream>

using namespace Graphics;

namespace
{
	uint32_t HashFrame(uint32_t x)
	{
		x ^= x >> 16; x *= 0x7feb352dU;
		x ^= x >> 15; x *= 0x846ca68bU;
		x ^= x >> 16;
		return x;
	}

	uint32_t RoundUp16(uint32_t v) { return (v + 15u) & ~15u; }

	// ModuleCommon.hlsli 의 cbuffer EngineConstants (b0) 와 같은 배치 (32 바이트)
	struct alignas(16) EngineConstants
	{
		float3   ownerPosition;
		float    deltaTime;
		uint32_t capacity;
		uint32_t randomSeed;
		uint32_t _pad[2];
	};
	static_assert(sizeof(EngineConstants) == 32, "HLSL cbuffer EngineConstants(32 B) 와 일치해야 함");
	static constexpr float kMaxDeltaTime = 0.1f;

	// Counters 안의 위치 (ModuleCommon.hlsli 의 COUNTER_* 와 같게)
	const uint32_t kCounterAliveIn = 0;
	const uint32_t kCounterSpawnRequest = 8;
	const uint32_t kCounterBytes = 16;

	// Finalize 는 모듈과 관계없는 고정 커널이라 빌드할 때 컴파일된 것을 쓴다
	bool       s_FinalizeReady = false;
	ComputePSO s_FinalizePSO(L"Emitter Finalize PSO");

	void InitializeFinalizePSO()
	{
		if (s_FinalizeReady)
			return;
		s_FinalizePSO.SetRootSignature(EmitterGpuProgram::RootSig());
		s_FinalizePSO.SetComputeShader(g_pEmitterFinalizeCS, sizeof(g_pEmitterFinalizeCS));
		s_FinalizePSO.Finalize();
		s_FinalizeReady = true;
	}
}


bool GpuEmitterInstance::Initialize(const CompiledEmitter& compiled, const EmitterGpuProgram& program, uint32_t capacity)
{
	Shutdown();

#ifndef RELEASE
	m_Log.clear();
#endif

	if (!compiled.Ok() || !program.Ready() || capacity == 0)
	{
#ifndef RELEASE
		m_Log = "[GpuEmitterInstance] CompiledEmitter 에러 / EmitterGpuProgram 미준비 / capacity 0 중 하나\n";
#endif
		return false;
	}

	m_Compiled = compiled;
	m_Program = &program;
	m_Capacity = capacity;

	const uint32_t zeros[4] = {};

	const uint32_t particleBytes = RoundUp16(max(m_Compiled.particles.Stride(), 4u) * capacity);
	m_Particles.Create(L"GPU Emitter Particles", particleBytes / 4, 4);
	m_Counters.Create(L"GPU Emitter Counters", kCounterBytes / 4, 4, zeros);

	const uint32_t emitterBytes = RoundUp16(max(m_Compiled.emitterParams.Stride(), 4u));
	std::vector<uint8_t> emitterInit(emitterBytes, 0);
	m_EmitterData.Create(L"GPU Emitter Data", emitterBytes / 4, 4, emitterInit.data());

	m_SpawnArgs.Create(L"GPU Emitter Spawn Args", 3, 4, zeros);
	m_DrawArgs.Create(L"GPU Emitter Draw Args", 4, 4, zeros);

	for (uint32_t i = 0; i < kReadbackSlots; ++i) // Latency에 따른 3개의 슬ㄹ롯
	{
		m_Readback[i].Create(L"GPU Emitter Counters Readback", kCounterBytes / 4, 4);
		m_ReadbackFence[i] = 0;
	}

	// 16바이트 단위로 정렬된 cbuffer
	LoadModuleParams();

	InitializeFinalizePSO();

	m_Frame = 0;
	m_ReadbackAlive = m_ReadbackRequest = 0;
	m_Ready = true;
	return true;
}

void GpuEmitterInstance::LoadModuleParams()
{
	const uint32_t cbBytes = max(m_Compiled.moduleParams.ConstantBufferSize(), 16u);
	m_ModuleParams.assign(cbBytes / 16, Chunk16());
	std::memset(m_ModuleParams.data(), 0, cbBytes);
	if (!m_Compiled.moduleParamData.empty())
		std::memcpy(m_ModuleParams.data(), m_Compiled.moduleParamData.data(),
			std::min<size_t>(m_Compiled.moduleParamData.size(), cbBytes));
}

void GpuEmitterInstance::SetModuleParamData(const std::vector<uint8_t>& data)
{
	m_Compiled.moduleParamData = data;
	LoadModuleParams();
}

void GpuEmitterInstance::Rebind(const CompiledEmitter& compiled, const EmitterGpuProgram& program)
{
	m_Compiled = compiled;
	m_Program = &program;
	LoadModuleParams();
}

void GpuEmitterInstance::Shutdown()
{
	for (uint32_t i = 0; i < kReadbackSlots; ++i)
		m_Readback[i].Destroy();

	m_DrawArgs.Destroy();
	m_SpawnArgs.Destroy();
	m_EmitterData.Destroy();
	m_Counters.Destroy();
	m_Particles.Destroy();

	m_ModuleParams.clear();
	m_Program = nullptr;
	m_Ready = false;
}

void GpuEmitterInstance::Tick(float deltaTime, const float3& ownerPosition)
{
	if (!m_Ready)
		return;

	ReadCountersIfReady();

	EngineConstants ec = {};
	ec.deltaTime = min(max(deltaTime, 0.f), float(kMaxDeltaTime));
	ec.ownerPosition = ownerPosition;
	ec.capacity = m_Capacity;
	ec.randomSeed = HashFrame(m_Frame + 1);

	ComputeContext& ctx = ComputeContext::Begin(L"Gpu Emitter");

	ctx.SetRootSignature(EmitterGpuProgram::RootSig());
	ctx.SetDynamicConstantBufferView(EmitterGpuProgram::kRootEngineCB, sizeof(ec), &ec);
	ctx.SetDynamicConstantBufferView(EmitterGpuProgram::kRootModuleCB, m_ModuleParams.size() * sizeof(Chunk16), m_ModuleParams.data());

	ctx.TransitionResource(m_Particles, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
	ctx.TransitionResource(m_Counters, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
	ctx.TransitionResource(m_EmitterData, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
	ctx.TransitionResource(m_SpawnArgs, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

	D3D12_CPU_DESCRIPTOR_HANDLE uavs[EmitterGpuProgram::kNumUAVs] =
	{
		m_Particles.GetUAV(),
		m_Particles.GetUAV(),
		m_Counters.GetUAV(),
		m_EmitterData.GetUAV(),
		m_SpawnArgs.GetUAV()
	};
	ctx.SetDynamicDescriptors(EmitterGpuProgram::kRootUAVs, 0, EmitterGpuProgram::kNumUAVs, uavs);

	// --------- 1. Emitter Update ---------
	ctx.SetPipelineState(m_Program->PSO(EmitterGpuProgram::kEmitterUpdate));
	ctx.Dispatch(1, 1, 1);

	// --------- 2. Particle Spawn ---------
	ctx.TransitionResource(m_SpawnArgs, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
	ctx.InsertUAVBarrier(m_Counters);
	ctx.InsertUAVBarrier(m_EmitterData);
	ctx.SetPipelineState(m_Program->PSO(EmitterGpuProgram::kParticleSpawn));
	ctx.DispatchIndirect(m_SpawnArgs);

	// --------- 3. Finalize (스레드 1개) ---------
	ctx.InsertUAVBarrier(m_Counters);
	ctx.TransitionResource(m_DrawArgs, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
	uavs[4] = m_DrawArgs.GetUAV(); // uav 4번에 드로우 인자로 교체
	ctx.SetDynamicDescriptors(EmitterGpuProgram::kRootUAVs, 0, EmitterGpuProgram::kNumUAVs, uavs);
	ctx.SetPipelineState(s_FinalizePSO);
	ctx.Dispatch(1, 1, 1);

	// ---------- 4. 개수 Readback ----------
	const uint32_t slot = m_Frame % kReadbackSlots;
	ctx.TransitionResource(m_Counters, D3D12_RESOURCE_STATE_COPY_SOURCE);
	ctx.CopyBufferRegion(m_Readback[slot], 0, m_Counters, 0, kCounterBytes);

	// ---------- 5. 그리기 준비 ----------
	ctx.TransitionResource(m_Particles, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
	ctx.TransitionResource(m_DrawArgs, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT, true);

	m_ReadbackFence[slot] = ctx.Finish();
	++m_Frame;
}

void GpuEmitterInstance::ReadCountersIfReady()
{
	const uint32_t slot = m_Frame % kReadbackSlots;
	if (m_ReadbackFence[slot] == 0 || !g_CommandManager.IsFenceComplete(m_ReadbackFence[slot]))
	{
		return;
	}
	// Readback으로 가져오기
	const uint8_t* data = static_cast<const uint8_t*>(m_Readback[slot].Map());
	std::memcpy(&m_ReadbackAlive, data + kCounterAliveIn, 4);
	std::memcpy(&m_ReadbackRequest, data + kCounterSpawnRequest, 4);

	m_Readback[slot].Unmap();


}
