#include "pch.h"
#include "ParticlePool.h"

#include "GraphicsCore.h"
#include "GraphicsCommon.h"
#include "BufferManager.h"
#include "CommandContext.h"
#include "RootSignature.h"
#include "PipelineState.h"
#include "Camera.h"

#include "ParticleUtility.h"
#include "ParticleEmitter.h"

#include "CompiledShaders/ParticleEmitAliveCS.h"
#include "CompiledShaders/ParticlePreSim.h"
#include "CompiledShaders/ParticleSimulateAliveWaveCS.h"
#include "CompiledShaders/ParticleAfterSimAliveCS.h"
#include "CompiledShaders/ParticleDrawAliveVS.h"
#include "CompiledShaders/ParticleDrawPS.h"

#include <vector>
#include <algorithm>

using namespace Graphics;
using namespace Math;
using namespace Particles;

namespace
{
	enum ComputeRootParams { kComputeCBVs, kComputeUAVs, kNumComputeRootParams };
	enum DrawRootParams { kDrawCBVs, kDrawSRVs, kNumDrawRootParams };

	// u0 Particles, u1 DeadList, u2 Counters, u3 AliveIn, u4 AliveOut, u5 IndirectArgs
	static constexpr uint32_t kNumComputeUAVs = 6;

	bool			s_Initialized = false;
	RootSignature	s_ComputeSig;
	RootSignature	s_DrawSig;
	ComputePSO		s_EmitPSO(L"Particle Emit PSO");
	ComputePSO		s_PreSimPSO(L"Particle Pre Sim PSO");
	ComputePSO		s_SimulatePSO(L"Particle Simulate Wave PSO");
	ComputePSO		s_AfterSimPSO(L"Particle After Sim PSO");

	GraphicsPSO		s_DrawPSO(L"Particle Draw PSO");

	void InitializeSetting()
	{
		if (s_Initialized)
			return;

		// ----------------------
		//		RootSignature
		// ----------------------

		// 1) Compute RootSig
		s_ComputeSig.Reset(kNumComputeRootParams, 0);
		s_ComputeSig[kComputeCBVs].InitAsConstantBuffer(0);
		s_ComputeSig[kComputeUAVs].InitAsDescriptorRange(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 0, kNumComputeUAVs);
		s_ComputeSig.Finalize(L"Particle Compute RootSig");

		// 2) Draw RootSig
		s_DrawSig.Reset(kNumDrawRootParams, 0);
		s_DrawSig[kDrawCBVs].InitAsConstantBuffer(0, D3D12_SHADER_VISIBILITY_VERTEX);
		s_DrawSig[kDrawSRVs].InitAsDescriptorRange(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 0, 3, D3D12_SHADER_VISIBILITY_VERTEX);
		s_DrawSig.Finalize(L"Particle Draw RootSig");

		// ----------------------
		//			PSO
		// ----------------------
		// 1) Compute PSO
		s_EmitPSO.SetRootSignature(s_ComputeSig);
		s_EmitPSO.SetComputeShader(g_pParticleEmitAliveCS, sizeof(g_pParticleEmitAliveCS));
		s_EmitPSO.Finalize();

		s_PreSimPSO.SetRootSignature(s_ComputeSig);
		s_PreSimPSO.SetComputeShader(g_pParticlePreSim, sizeof(g_pParticlePreSim));
		s_PreSimPSO.Finalize();

		s_SimulatePSO.SetRootSignature(s_ComputeSig);
		s_SimulatePSO.SetComputeShader(g_pParticleSimulateAliveWaveCS, sizeof(g_pParticleSimulateAliveWaveCS));
		s_SimulatePSO.Finalize();

		s_AfterSimPSO.SetRootSignature(s_ComputeSig);
		s_AfterSimPSO.SetComputeShader(g_pParticleAfterSimAliveCS, sizeof(g_pParticleAfterSimAliveCS));
		s_AfterSimPSO.Finalize();

		// 2) Draw PSO
		s_DrawPSO.SetRootSignature(s_DrawSig);
		s_DrawPSO.SetRasterizerState(RasterizerTwoSided);
		s_DrawPSO.SetBlendState(BlendAdditive);
		s_DrawPSO.SetDepthStencilState(DepthStateDisabled);
		s_DrawPSO.SetPrimitiveTopologyType(D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE);
		s_DrawPSO.SetRenderTargetFormat(g_SceneColorBuffer.GetFormat(), DXGI_FORMAT_UNKNOWN);
		s_DrawPSO.SetVertexShader(g_pParticleDrawAliveVS, sizeof(g_pParticleDrawAliveVS));
		s_DrawPSO.SetPixelShader(g_pParticleDrawPS, sizeof(g_pParticleDrawPS));
		s_DrawPSO.Finalize();

		s_Initialized = true;
	}
};


void ParticlePool::Initialize(uint32_t maxParticles)
{
	if (!s_Initialized)
		InitializeSetting();

	CreateBuffers(maxParticles);

	m_initVerified = VerifyInitialState();
}

void ParticlePool::CreateBuffers(uint32_t maxParticles)
{
	m_MaxParticles = maxParticles;

	// 1) Particle Buffer;
	std::vector<Particle> zeroParticles(m_MaxParticles);
	m_ParticleBuffer.Create(L"Particle Buffer", m_MaxParticles, sizeof(Particle), zeroParticles.data());

	// 2) DeadList Buffer
	std::vector<uint32_t> zeroDeadList(m_MaxParticles);
	for (uint32_t i = 0; i < m_MaxParticles; ++i)
		zeroDeadList[i] = m_MaxParticles - 1 - i;
	m_DeadListBuffer.Create(L"Particle DeadList", m_MaxParticles, sizeof(uint32_t), zeroDeadList.data());

	// 3) AliveList Buffer
	std::vector<uint32_t> zeroAliveList(m_MaxParticles, 0u);
	m_AliveListBuffer[0].Create(L"Particle AliveList 0", m_MaxParticles, sizeof(uint32_t), zeroAliveList.data());
	m_AliveListBuffer[1].Create(L"Particle AliveList 1", m_MaxParticles, sizeof(uint32_t), zeroAliveList.data());

	// 4) Counter Buffer
	CounterSnapshot initSnapshot = {};
	initSnapshot.deadCount = m_MaxParticles;
	m_CounterBuffer.Create(L"Particle CounterBuffer", kCounterBufferSize / 4, sizeof(uint32_t), &initSnapshot);

	// 5) Indirect Args Buffer
	alignas(16) const uint32_t initArgs[kIndirectArgsSize / sizeof(uint32_t)] = {0, 1, 1, 0, /*Compute Args*/ 6, 0, 0, 0 /*Draw Args*/};
	m_IndirectArgsBuffer.Create(L"Particle IndirectBuffer", kIndirectArgsSize / sizeof(uint32_t), sizeof(uint32_t), initArgs);

	// Readback Buffer
	for (uint32_t i = 0; i < sReadbackLatency; ++i)
		m_CounterReadback[i].Create(L"Particle Counter ReadBack", kCounterBufferSize / 4, sizeof(uint32_t));
}

void ParticlePool::Shutdown()
{
	for (uint32_t i = 0; i < sReadbackLatency; ++i)
		m_CounterReadback[i].Destroy();
	m_CounterBuffer.Destroy();
	m_IndirectArgsBuffer.Destroy();
	m_AliveListBuffer[0].Destroy();
	m_AliveListBuffer[1].Destroy();
	m_DeadListBuffer.Destroy();
	m_ParticleBuffer.Destroy();
}

void ParticlePool::Update(float deltaT, const EmitRequest& emitRequest)
{
#ifndef RELEASE
	CollectCounterReadback();
#endif

	Simconstants sc = {};
	XMStoreFloat3(&sc.emitterPos, emitRequest.emitPosition);
	sc.deltaTime = deltaT;
	sc.gravity = { 0.f, -2.f, 0.f };			// TEMP
	sc.emitCount = emitRequest.emitCount;
	sc.minLife = emitRequest.emitParams.minLife;
	sc.maxLife = emitRequest.emitParams.maxLife;
	sc.minSpeed = 1.f;
	sc.maxSpeed = 4.f;
	sc.particleSize = emitRequest.emitParams.minSize;	// TEMP
	sc.maxParticles = m_MaxParticles;
	sc.randomSeed = HashU32(m_FrameIndex + 1);
	sc.baseColor = PackColor(255, 128, 45, 255); // TEMP 주황

	// -------------------------
	//		BEGIN COMPUTE
	// -------------------------
	ComputeContext& ctx = ComputeContext::Begin(L"Particle Update");

	alignas(16) static const uint32_t kZeros[4] = { 0, 0, 0, 0 };
	ctx.WriteBuffer(m_CounterBuffer, kEmitCount, kZeros, 8); // EmitCount, AfterSim 초기화

	// 1) Convert Buffers -> UAV
	ctx.TransitionResource(m_ParticleBuffer, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
	ctx.TransitionResource(m_DeadListBuffer, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
	ctx.TransitionResource(m_AliveListBuffer[0], D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
	ctx.TransitionResource(m_AliveListBuffer[1], D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
	ctx.TransitionResource(m_IndirectArgsBuffer, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
	ctx.TransitionResource(m_CounterBuffer, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, true);

	// 2) Bind
	ctx.SetRootSignature(s_ComputeSig);
	ctx.SetDynamicConstantBufferView(kComputeCBVs, sizeof(sc), &sc);

	const D3D12_CPU_DESCRIPTOR_HANDLE uavs[kNumComputeUAVs] =
	{
		m_ParticleBuffer.GetUAV(),		// u0
		m_DeadListBuffer.GetUAV(),		// u1
		m_CounterBuffer.GetUAV(),		// u2
		AliveIn().GetUAV(),				// u3
		AliveOut().GetUAV(),			// u4
		m_IndirectArgsBuffer.GetUAV()	// u5
	};
	ctx.SetDynamicDescriptors(kComputeUAVs, 0, kNumComputeUAVs, uavs);

	// 3) Emit
	{
		ScopedTimer _t(L"Particle Emit", ctx);
		if (emitRequest.emitCount > 0)
		{
			ctx.SetPipelineState(s_EmitPSO);
			ctx.Dispatch1D(emitRequest.emitCount, sThreadGroupSize);
		}
	}
	InsertUAVBarriers(ctx);

	// 4) PreSim : alive 수 -> Simulate Dispatch Args
	{
		ScopedTimer _t(L"Particle ArgsPre", ctx);
		ctx.SetPipelineState(s_PreSimPSO);
		ctx.Dispatch(1, 1, 1);
	}
	ctx.TransitionResource(m_IndirectArgsBuffer, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT, true);

	// 5) Simulate (Wave)
	{
		ScopedTimer _t(L"Particle Simulate", ctx);
		ctx.SetPipelineState(s_SimulatePSO);
		ctx.DispatchIndirect(m_IndirectArgsBuffer, kSimulateDispatchArgs);
	}
	InsertUAVBarriers(ctx);

	ctx.TransitionResource(m_IndirectArgsBuffer, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, true);

	// 6) AfterSimulate : 살아남은 수 확정 + Draw Args
	{
		ScopedTimer _t(L"Particle AfterSim", ctx);
		ctx.SetPipelineState(s_AfterSimPSO);
		ctx.Dispatch(1, 1, 1);
	}

	// 7) Readback
#ifndef RELEASE
	const uint32_t slot = m_ReadbackSlot;
	ctx.TransitionResource(m_CounterBuffer, D3D12_RESOURCE_STATE_COPY_SOURCE);
	ctx.CopyBufferRegion(m_CounterReadback[m_ReadbackSlot], 0, m_CounterBuffer, 0, kCounterBufferSize);
	m_ReadbackFence[slot] = ctx.Finish();
	m_ReadbackSlot = (m_ReadbackSlot + 1) % sReadbackLatency;
#else
	ctx.Finish();
#endif
	++m_FrameIndex;

	// Alive In <-> Alive Out 바꾸기
	m_AliveCurrent = 1 - m_AliveCurrent;
}

void ParticlePool::Render(GraphicsContext& gfx, const Math::Camera& cam)
{
	gfx.TransitionResource(m_ParticleBuffer, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
	gfx.TransitionResource(m_CounterBuffer, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
	gfx.TransitionResource(AliveIn(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
	gfx.TransitionResource(m_IndirectArgsBuffer, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);

	DrawConstants dc = {};
	XMStoreFloat4x4(&dc.viewProj, cam.GetViewProjMatrix());
	XMStoreFloat4(&dc.camRight, cam.GetRightVec());
	dc.camRight.w = 0.f;
	XMStoreFloat4(&dc.camUp, cam.GetUpVec());
	dc.camUp.w = 0.f;

	gfx.SetRootSignature(s_DrawSig);
	gfx.SetPipelineState(s_DrawPSO);
	gfx.SetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	gfx.SetDynamicConstantBufferView(kDrawCBVs, sizeof(dc), &dc);

	const D3D12_CPU_DESCRIPTOR_HANDLE srvs[3] =
	{
		m_ParticleBuffer.GetSRV(),	// t0
		AliveIn().GetSRV(),			// t1
		m_CounterBuffer.GetSRV()	// t2
	};
	gfx.SetDynamicDescriptors(kDrawSRVs, 0, 3, srvs);

	{
		ScopedTimer _t(L"Particle Draw", gfx);
		gfx.DrawIndirect(m_IndirectArgsBuffer, kDrawArgs);
	}
}

bool ParticlePool::VerifyInitialState()
{
	ReadbackBuffer verify;
	verify.Create(L"Particle Init Verify", 6, 4);

	// Begin Compute verify
	ComputeContext& ctx = ComputeContext::Begin(L"Particle Init Verify");

	ctx.TransitionResource(m_CounterBuffer, D3D12_RESOURCE_STATE_COPY_SOURCE);
	ctx.TransitionResource(m_DeadListBuffer, D3D12_RESOURCE_STATE_COPY_SOURCE);

	const size_t lastOffset = size_t(m_MaxParticles - 1) * 4;
	ctx.CopyBufferRegion(verify, 0, m_CounterBuffer, 0, kCounterBufferSize);
	ctx.CopyBufferRegion(verify, 16, m_DeadListBuffer, lastOffset, 4); // -> 값이 0에 해당해야 함
	ctx.CopyBufferRegion(verify, 20, m_DeadListBuffer, 0, 4); // -> 값이 Max-1에 해당해야 함
	ctx.Finish(true);

	const uint32_t* data = static_cast<const uint32_t*>(verify.Map());
	bool isVerified = (data[0] == 0) && (data[1] == m_MaxParticles) && (data[2] == 0) && (data[4] == 0) && (data[5] == m_MaxParticles - 1);
	Utility::Printf("[Particle] Init verify %s | MAX=%u alive=%u dead=%u deadTop=%u deadBottom=%u\n",
		isVerified ? "OK" : "FAILED", m_MaxParticles, data[0], data[1], data[4], data[5]);
	verify.Unmap();
	verify.Destroy();

	return isVerified;
}

void ParticlePool::InsertUAVBarriers(ComputeContext& ctx)
{
	ctx.InsertUAVBarrier(m_ParticleBuffer);
	ctx.InsertUAVBarrier(m_DeadListBuffer);
	ctx.InsertUAVBarrier(m_AliveListBuffer[0]);
	ctx.InsertUAVBarrier(m_AliveListBuffer[1]);
	ctx.InsertUAVBarrier(m_CounterBuffer, true);
}

#ifndef RELEASE
void ParticlePool::CollectCounterReadback()
{
	const uint32_t readbackSlot = m_ReadbackSlot;
	const uint64_t readbackFence = m_ReadbackFence[readbackSlot];
	if (readbackFence == 0)
		return;

	if (!g_CommandManager.IsFenceComplete(readbackFence))
		g_CommandManager.WaitForFence(readbackFence);

	std::memcpy(&m_LastCounters, m_CounterReadback[readbackSlot].Map(), sizeof(CounterSnapshot));
	m_CounterReadback[readbackSlot].Unmap();
	m_ReadbackFence[readbackSlot] = 0;
}
#endif
