#include "pch.h"
#include "GpuEmitterInstance.h"

#include "EmitterInstance.h"
#include "ShaderGenerator.h"

#include "CommandContext.h"
#include "CommandListManager.h"
#include "GraphicsCore.h"

#include "CompiledShaders/EmitterFinalizeCS.h"
#include "UberGpuProgram.h"

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

	// ModuleCommon.hlsli 의 cbuffer EngineConstants (b0) 와 같은 배치 (112 바이트)
	struct alignas(16) EngineConstants
	{
		float3   ownerPosition;       // 0
		float    deltaTime;           // 12
		uint32_t capacity;            // 16
		uint32_t randomSeed;          // 20
		uint32_t _pad[2];             // 24
		float3   ownerVelocity;       // 32
		float    _pad2;					// 44
		float4x4 ownerLocalToWorld;   // 48 (HLSL 은 row_major)
	};
	static_assert(sizeof(EngineConstants) == 112, "HLSL cbuffer EngineConstants(112 B) 와 일치해야 함");
	static constexpr float kMaxDeltaTime = 0.1f;

	// Counters 안의 위치 (ModuleCommon.hlsli 의 COUNTER_* 와 같게)
	const uint32_t kCounterAliveIn = 0;
	const uint32_t kCounterSpawnRequest = 8;
	const uint32_t kCounterBytes = 16;

	// m_FrameArgs 안의 위치 (EmitterFinalizeCS.hlsl 의 FRAME_ARGS_* 와 같게)
	const uint32_t kFrameArgsDraw = 0;
	const uint32_t kFrameArgsUpdate = 16;
	const uint32_t kFrameArgsBytes = 28;

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

	GpuSimFrameStats		s_FrameStats;
	const ID3D12PipelineState* s_LastPSO = nullptr;

	void SetPSO(ComputeContext& ctx, const ComputePSO& pso)
	{
		const ID3D12PipelineState* p = pso.GetPipelineStateObject();
		if (p != s_LastPSO)
		{
			++s_FrameStats.psoChanges;
			s_LastPSO = p;
		}
		ctx.SetPipelineState(pso);

	}
}

void GpuEmitterInstance::BeginFrameStats()
{
	s_FrameStats = GpuSimFrameStats();
	s_LastPSO = nullptr;
}

const GpuSimFrameStats& GpuEmitterInstance::FrameStats()
{
	return s_FrameStats;
}


bool GpuEmitterInstance::Initialize(const CompiledEmitter& compiled, const EmitterGpuProgram* program, uint32_t capacity)
{
	Shutdown();

#ifndef RELEASE
	m_Log.clear();
#endif

	if (!compiled.Ok() || (program != nullptr && !program->Ready()) || capacity == 0)
	{
#ifndef RELEASE
		m_Log = "[GpuEmitterInstance] CompiledEmitter 에러 / EmitterGpuProgram 미준비 / capacity 0 중 하나\n";
#endif
		return false;
	}

	m_Compiled = compiled;
	m_Program = program;
	m_Capacity = capacity;

	alignas(16) const uint32_t zeros[8] = {};

	const uint32_t particleBytes = RoundUp16(max(m_Compiled.particles.Stride(), 4u) * capacity);
	
	m_Particles[0].Create(L"GPU Emitter Particles A", particleBytes / 4, 4);
	m_Particles[1].Create(L"GPU Emitter Particles B", particleBytes / 4, 4);
	m_Current = 0;

	m_Counters.Create(L"GPU Emitter Counters", kCounterBytes / 4, 4, zeros);

	const uint32_t emitterBytes = RoundUp16(max(m_Compiled.emitterParams.Stride(), 4u));
	std::vector<uint8_t> emitterInit(emitterBytes, 0);
	m_EmitterData.Create(L"GPU Emitter Data", emitterBytes / 4, 4, emitterInit.data());

	m_SpawnArgs.Create(L"GPU Emitter Spawn Args", 3, 4, zeros);
	m_FrameArgs.Create(L"GPU Emitter Frame Args", kFrameArgsBytes / 4, 4, zeros);

	for (uint32_t i = 0; i < kReadbackSlots; ++i) // Latency에 따른 3개의 슬롯
	{
		m_Readback[i].Create(L"GPU Emitter Counters Readback", kCounterBytes / 4, 4);
		m_ReadbackFence[i] = 0;
	}

	// 16바이트 단위로 정렬된 cbuffer
	LoadModuleParams();

	InitializeFinalizePSO();

	m_Frame = 0;
	m_HasPrevOwner = false;
	m_ReadbackAlive = m_ReadbackRequest = 0;
	m_Ready = true;
	return true;
}

void GpuEmitterInstance::Shutdown()
{
	for (uint32_t i = 0; i < kReadbackSlots; ++i)
		m_Readback[i].Destroy();

	m_FrameArgs.Destroy();
	m_SpawnArgs.Destroy();
	m_EmitterData.Destroy();
	m_Counters.Destroy();
	for (uint32_t i = 0 ; i < 2; ++i)
		m_Particles[i].Destroy();

	m_ModuleParams.clear();
	m_Program = nullptr;
	m_WaitingSubmit = false;
	m_UberParams.clear();
	m_UberLayout = EmitterLayoutCB();
	m_UberOk = false;
	m_Ready = false;
}


void GpuEmitterInstance::Tick(ComputeContext& ctx, float deltaTime, const OwnerTransform& t)
{
	if (!BeginTick(deltaTime, t))
		return;

	for (uint32_t s = 0; s < uint32_t(GpuSimStage::End); ++s)
	{
		StageBarriers(ctx, GpuSimStage(s));
		StageWork(ctx, GpuSimStage(s));
	}

	EndTick();
}

void GpuEmitterInstance::Tick(ComputeContext& ctx, float deltaTime, const float3& ownerPosition)
{
	OwnerTransform t;
	t.position = ownerPosition;
	Tick(ctx, deltaTime, t);
}

void GpuEmitterInstance::OnSubmitted(uint64_t fence)
{
	if (!m_WaitingSubmit)
		return;
	m_ReadbackFence[m_SubmitSlot] = fence;
	m_WaitingSubmit = false;
}

bool GpuEmitterInstance::BeginTick(float deltaTime, const OwnerTransform& t)
{
	m_TickActive = false;
	if (!m_Ready)
		return false;

	ASSERT(!m_WaitingSubmit, "이전 Tick 이후 OnSubmitted 호출되지 않음");

	ReadCountersIfReady();

	EngineConstants ec = {};
	ec.ownerPosition = t.position;
	ec.deltaTime = min(max(deltaTime, 0.f), float(kMaxDeltaTime));
	ec.capacity = m_Capacity;
	ec.randomSeed = HashFrame(m_Frame + 1);
	ec.ownerVelocity = ComputeOwnerVelocity(t.position, m_HasPrevOwner ? &m_PrevOwnerPosition : nullptr, deltaTime);
	ec.ownerLocalToWorld = MakeLocalToWorld(t);
	m_PrevOwnerPosition = t.position;
	m_HasPrevOwner = true;

	std::memcpy(m_TickEngineCB, &ec, sizeof(ec));

	const bool wantUber = UberGpuProgram::Mode() == GpuSimMode::Uber;
	const bool uberPossible = UberGpuProgram::Ready() && m_UberOk;
	m_TickUber = uberPossible && (wantUber || m_Program == nullptr);
	if (!m_TickUber && m_Program == nullptr)
		return false;
	if (m_TickUber)
		++s_FrameStats.uberEmitters;
	else if (wantUber)
		++s_FrameStats.uberFallbacks;

	m_TickActive = true;
	return true;
}

void GpuEmitterInstance::StageBarriers(ComputeContext& ctx, GpuSimStage stage)
{
	if (!m_TickActive)
		return;

	ByteAddressBuffer& particlesIn = m_Particles[m_Current];
	ByteAddressBuffer& particlesOut = m_Particles[1 - m_Current];

	switch (stage)
	{
	case GpuSimStage::Prepare:
		ctx.TransitionResource(particlesIn, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
		ctx.TransitionResource(particlesOut, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
		ctx.TransitionResource(m_Counters, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
		ctx.TransitionResource(m_EmitterData, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
		ctx.TransitionResource(m_SpawnArgs, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
		
		break;
	case GpuSimStage::EmitterUpdate:
		break;
	case GpuSimStage::ParticleUpdate:
		ctx.TransitionResource(m_FrameArgs, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
		break;
	case GpuSimStage::ParticleSpawn:
		ctx.TransitionResource(m_SpawnArgs, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
		ctx.InsertUAVBarrier(m_Counters);
		ctx.InsertUAVBarrier(m_EmitterData);
		ctx.InsertUAVBarrier(particlesOut);
		break;
	case GpuSimStage::Finalize:
		ctx.InsertUAVBarrier(m_Counters);
		ctx.TransitionResource(m_FrameArgs, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
		break;
	case GpuSimStage::Readback:
		ctx.TransitionResource(m_Counters, D3D12_RESOURCE_STATE_COPY_SOURCE);
		break;

	case GpuSimStage::Present:
		ctx.TransitionResource(particlesOut, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
		ctx.TransitionResource(m_FrameArgs, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT, true);
		break;
	}

	ctx.FlushResourceBarriers();
}

void GpuEmitterInstance::StageWork(ComputeContext& ctx, GpuSimStage stage)
{
	if (!m_TickActive)
		return;

	switch (stage)
	{
	case GpuSimStage::EmitterUpdate:
		BindRoot(ctx, false);
		SetPSO(ctx, KernelPSO(EmitterGpuProgram::kEmitterUpdate));
		ctx.Dispatch(1, 1, 1);
		break;
	case GpuSimStage::ParticleUpdate:
		BindRoot(ctx, false);
		SetPSO(ctx, KernelPSO(EmitterGpuProgram::kParticleUpdate));
		ctx.DispatchIndirect(m_FrameArgs, kFrameArgsUpdate);
		break;
	case GpuSimStage::ParticleSpawn:
		BindRoot(ctx, false);
		SetPSO(ctx, KernelPSO(EmitterGpuProgram::kParticleSpawn));
		ctx.DispatchIndirect(m_SpawnArgs);
		break;
	case GpuSimStage::Finalize:
		BindRoot(ctx, true);
		SetPSO(ctx, s_FinalizePSO);
		ctx.Dispatch(1, 1, 1);
		s_FrameStats.dispatches += 4;
		s_FrameStats.emitters += 1;
		break;
	case GpuSimStage::Readback:
	{
		const uint32_t slot = m_Frame % kReadbackSlots;
		m_ReadbackFence[slot] = 0;
		m_SubmitSlot = slot;
		m_WaitingSubmit = true;
		ctx.CopyBufferRegion(m_Readback[slot], 0, m_Counters, 0, kCounterBytes);
	}
		break;				
	case GpuSimStage::Present:
		break;
	default:
		break;
	}
}

void GpuEmitterInstance::EndTick()
{
	if (!m_TickActive)
		return;
	m_TickActive = false;
	m_Current = 1 - m_Current;
	++m_Frame;
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

void GpuEmitterInstance::BindRoot(ComputeContext& ctx, bool finalizeStage)
{
	ctx.SetRootSignature(EmitterGpuProgram::RootSig());
	ctx.SetDynamicConstantBufferView(EmitterGpuProgram::kRootEngineCB, sizeof(m_TickEngineCB), m_TickEngineCB);
	if (m_TickUber)
		ctx.SetDynamicConstantBufferView(EmitterGpuProgram::kRootModuleCB, m_UberParams.size() * sizeof(Chunk16), m_UberParams.data());
	else
		ctx.SetDynamicConstantBufferView(EmitterGpuProgram::kRootModuleCB, m_ModuleParams.size() * sizeof(Chunk16), m_ModuleParams.data());
	ctx.SetDynamicConstantBufferView(EmitterGpuProgram::kRootLayoutCB, sizeof(m_UberLayout), &m_UberLayout);

	ByteAddressBuffer& particlesIn = m_Particles[m_Current];
	ByteAddressBuffer& particlesOut = m_Particles[1 - m_Current];
	D3D12_CPU_DESCRIPTOR_HANDLE uavs[EmitterGpuProgram::kNumUAVs] =
	{
		particlesIn.GetUAV(),
		particlesOut.GetUAV(),
		m_Counters.GetUAV(),
		m_EmitterData.GetUAV(),
		finalizeStage ? m_FrameArgs.GetUAV() : m_SpawnArgs.GetUAV()
	};
	ctx.SetDynamicDescriptors(EmitterGpuProgram::kRootUAVs, 0, EmitterGpuProgram::kNumUAVs, uavs);
}

const ComputePSO& GpuEmitterInstance::KernelPSO(EmitterGpuProgram::Kernel k) const
{
	return m_TickUber ? UberGpuProgram::PSO(k) : m_Program->PSO(k);
}

void GpuEmitterInstance::SetModuleParamData(const std::vector<uint8_t>& data)
{
	m_Compiled.moduleParamData = data;
	LoadModuleParams();
}

void GpuEmitterInstance::Rebind(const CompiledEmitter& compiled, const EmitterGpuProgram* program)
{
	m_Compiled = compiled;
	m_Program = program;
	LoadModuleParams();
}

void GpuEmitterInstance::SetUberData(const UberEmitterData& data)
{
	m_UberLayout = data.layout;
	m_UberOk = data.Ok() && !data.params.empty();

	const size_t bytes = (data.params.size() + 15) & ~size_t(15);
	m_UberParams.assign((std::max)(bytes, size_t(16)) / 16, Chunk16());
	std::memset(m_UberParams.data(), 0, m_UberParams.size() * sizeof(Chunk16));
	if (!data.params.empty())
		std::memcpy(m_UberParams.data(), data.params.data(), data.params.size());
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
