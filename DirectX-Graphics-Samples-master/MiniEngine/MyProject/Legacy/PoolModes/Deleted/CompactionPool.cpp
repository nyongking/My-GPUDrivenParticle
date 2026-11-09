#include "pch.h"
#include "CompactionPool.h"

#include "GraphicsCore.h"
#include "GraphicsCommon.h"
#include "BufferManager.h"
#include "CommandContext.h"
#include "RootSignature.h"
#include "PipelineState.h"
#include "Camera.h"

#include "ParticleUtility.h"
#include "ParticleEmitter.h"

#include "CompiledShaders/ParticleEmitCompactCS.h"
#include "CompiledShaders/ParticlePreSim.h"
#include "CompiledShaders/ParticleSimulateCompactCS.h"
#include "CompiledShaders/ParticleAfterSimAliveCS.h"
#include "CompiledShaders/ParticleDrawCompactVS.h"
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

	bool			s_Initialized = false;
	RootSignature	s_ComputeSig;
	RootSignature	s_DrawSig;
	ComputePSO		s_EmitPSO(L"Particle Compact Emit PSO");
	ComputePSO		s_PreSimPSO(L"Particle Compact Pre Sim PSO");
	ComputePSO		s_SimulatePSO(L"Particle Compact Simulate PSO");
	ComputePSO		s_AfterSimPSO(L"Particle Compact After Sim PSO");

	GraphicsPSO		s_DrawPSO(L"Particle Compact Draw PSO");
	void InitializeSetting()
	{
		if (s_Initialized)
			return;
		// ----------------------
				//		RootSignature
				// ----------------------

		s_ComputeSig.Reset(kNumComputeRootParams, 0);
		s_ComputeSig[kComputeCBVs].InitAsConstantBuffer(0);
		s_ComputeSig[kComputeUAVs].InitAsDescriptorTable(3);
		s_ComputeSig[kComputeUAVs].SetTableRange(0, D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 0, 1);
		s_ComputeSig[kComputeUAVs].SetTableRange(1, D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 2, 1);
		s_ComputeSig[kComputeUAVs].SetTableRange(2, D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 6, 2);
		s_ComputeSig.Finalize(L"Particle Compact Compute RootSig");

		// 2) Draw RootSig : t0 = 파티클 하나만 (명단, 카운터 필요 없음)
		s_DrawSig.Reset(kNumDrawRootParams, 0);
		s_DrawSig[kDrawCBVs].InitAsConstantBuffer(0, D3D12_SHADER_VISIBILITY_VERTEX);
		s_DrawSig[kDrawSRVs].InitAsDescriptorRange(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 0, 1, D3D12_SHADER_VISIBILITY_VERTEX);
		s_DrawSig.Finalize(L"Particle Compact Draw RootSig");

		// ----------------------
		//			PSO
		// ----------------------
		s_EmitPSO.SetRootSignature(s_ComputeSig);
		s_EmitPSO.SetComputeShader(g_pParticleEmitCompactCS, sizeof(g_pParticleEmitCompactCS));
		s_EmitPSO.Finalize();

		s_PreSimPSO.SetRootSignature(s_ComputeSig);
		s_PreSimPSO.SetComputeShader(g_pParticlePreSim, sizeof(g_pParticlePreSim));
		s_PreSimPSO.Finalize();

		s_SimulatePSO.SetRootSignature(s_ComputeSig);
		s_SimulatePSO.SetComputeShader(g_pParticleSimulateCompactCS, sizeof(g_pParticleSimulateCompactCS));
		s_SimulatePSO.Finalize();

		s_AfterSimPSO.SetRootSignature(s_ComputeSig);
		s_AfterSimPSO.SetComputeShader(g_pParticleAfterSimAliveCS, sizeof(g_pParticleAfterSimAliveCS));
		s_AfterSimPSO.Finalize();

		s_DrawPSO.SetRootSignature(s_DrawSig);
		s_DrawPSO.SetRasterizerState(RasterizerTwoSided);
		s_DrawPSO.SetBlendState(BlendAdditive);
		s_DrawPSO.SetDepthStencilState(DepthStateDisabled);
		s_DrawPSO.SetPrimitiveTopologyType(D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE);
		s_DrawPSO.SetRenderTargetFormat(g_SceneColorBuffer.GetFormat(), DXGI_FORMAT_UNKNOWN);
		s_DrawPSO.SetVertexShader(g_pParticleDrawCompactVS, sizeof(g_pParticleDrawCompactVS));
		s_DrawPSO.SetPixelShader(g_pParticleDrawPS, sizeof(g_pParticleDrawPS));
		s_DrawPSO.Finalize();

		s_Initialized = true;
	}

}

void CompactionPool::Initialize(uint32_t maxParticles)
{
	if (!s_Initialized)
		InitializeSetting();

	CreateBuffers(maxParticles);

	m_initVerified = VerifyInitialState();

}

void CompactionPool::CreateBuffers(uint32_t maxParticles)
{
	m_MaxParticles = maxParticles;
	m_Current = 0;

	std::vector<Particle> zeroParticles(m_MaxParticles);
	m_ParticleBuffer[0].Create(L"Particle Compact Buffer 0", m_MaxParticles, sizeof(Particle), zeroParticles.data());
	m_ParticleBuffer[1].Create(L"Particle Compact Buffer 1", m_MaxParticles, sizeof(Particle), zeroParticles.data());

	CounterSnapshot initSnapshot = {};
	m_CounterBuffer.Create(L"Particle CounterBuffer", kCounterBufferSize / 4, sizeof(uint32_t), &initSnapshot);

	alignas(16) const uint32_t initArgs[kIndirectArgsSize / sizeof(uint32_t)] = { 0, 1, 1, 0, /*Compute Args*/ 6, 0, 0, 0 /*Draw Args*/ };
	m_IndirectArgsBuffer.Create(L"Particle IndirectBuffer", kIndirectArgsSize / sizeof(uint32_t), sizeof(uint32_t), initArgs);

	for (uint32_t i = 0; i < sReadbackLatency; ++i)
		m_CounterReadback[i].Create(L"Particle Counter ReadBack", kCounterBufferSize / 4, sizeof(uint32_t));
}

void CompactionPool::Shutdown()
{
	for (uint32_t i = 0; i < sReadbackLatency; ++i)
		m_CounterReadback[i].Destroy();
	m_CounterBuffer.Destroy();
	m_IndirectArgsBuffer.Destroy();
	m_ParticleBuffer[0].Destroy();
	m_ParticleBuffer[1].Destroy();
}

void CompactionPool::Update(float deltaT, const EmitRequest& emitRequest)
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

	// 1) Buffers -> UAV
	ctx.TransitionResource(Current(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
	ctx.TransitionResource(Next(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
	ctx.TransitionResource(m_IndirectArgsBuffer, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
	ctx.TransitionResource(m_CounterBuffer, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, true);

	// 2) Bind
	ctx.SetRootSignature(s_ComputeSig);
	ctx.SetDynamicConstantBufferView(kComputeCBVs, sizeof(sc), &sc);

	const D3D12_CPU_DESCRIPTOR_HANDLE uavs[4] =
	{
		Current().GetUAV(),				// u0 : 입력 (Emit이 끝에 붙이고, Simulate가 읽는다)
		m_CounterBuffer.GetUAV(),		// u2
		m_IndirectArgsBuffer.GetUAV(),	// u6
		Next().GetUAV()					// u7 : 출력 (Simulate가 생존자를 앞에서부터 쓴다)
	};
	ctx.SetDynamicDescriptors(kComputeUAVs, 0, 4, uavs);

	// 3) Emit : 입력 버퍼 끝에 붙이기
	{
		ScopedTimer _t(L"Particle Emit", ctx);
		if (emitRequest.emitCount > 0)
		{
			ctx.SetPipelineState(s_EmitPSO);
			ctx.Dispatch1D(emitRequest.emitCount, sThreadGroupSize);
		}
	}
	InsertUAVBarriers(ctx);

	// 4-1) ArgsPre : alive 수로 Simulate 그룹 수 계산
	{
		ScopedTimer _t(L"Particle ArgsPre", ctx);
		ctx.SetPipelineState(s_PreSimPSO);
		ctx.Dispatch(1, 1, 1);
	}
	ctx.TransitionResource(m_IndirectArgsBuffer, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT, true);

	// 4-2) Simulate : 입력 [0, alive) -> 출력 [0, survived)
	{
		ScopedTimer _t(L"Particle Simulate", ctx);
		ctx.SetPipelineState(s_SimulatePSO);
		ctx.DispatchIndirect(m_IndirectArgsBuffer, kSimulateDispatchArgs);
	}
	InsertUAVBarriers(ctx);

	// AfterSim이 IndirectArgs에 드로우 인자를 쓰므로 다시 UAV로
	ctx.TransitionResource(m_IndirectArgsBuffer, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, true);

	// 5) AfterSim : alive = survived, 드로우 인자 (6, survived, 0, 0)
	{
		ScopedTimer _t(L"Particle AfterSim", ctx);
		ctx.SetPipelineState(s_AfterSimPSO);
		ctx.Dispatch(1, 1, 1);
	}

	// 6) Readback
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

	// 출력 버퍼가 다음 프레임의 입력이 된다 (Render도 이 버퍼를 그린다)
	m_Current = 1 - m_Current;
}

void CompactionPool::Render(GraphicsContext& gfx, const Math::Camera& cam)
{
	gfx.TransitionResource(Current(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
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

	const D3D12_CPU_DESCRIPTOR_HANDLE srvs[1] = { Current().GetSRV() };	// t0
	gfx.SetDynamicDescriptors(kDrawSRVs, 0, 1, srvs);

	ScopedTimer _t(L"Particle Draw", gfx);
	gfx.DrawIndirect(m_IndirectArgsBuffer, kDrawArgs);
}

bool CompactionPool::VerifyInitialState()
{
	ReadbackBuffer verify;
	verify.Create(L"Particle Init Verify", 4, 4);

	ComputeContext& ctx = ComputeContext::Begin(L"Particle Init Verify");
	ctx.TransitionResource(m_CounterBuffer, D3D12_RESOURCE_STATE_COPY_SOURCE);
	ctx.CopyBufferRegion(verify, 0, m_CounterBuffer, 0, kCounterBufferSize);
	ctx.Finish(true);

	const uint32_t* data = static_cast<const uint32_t*>(verify.Map());
	const bool isVerified = (data[0] == 0) && (data[2] == 0) && (data[3] == 0);
	Utility::Printf("[Particle] Init verify %s | MAX=%u alive=%u (Compaction: no dead list)\n",
		isVerified ? "OK" : "FAILED", m_MaxParticles, data[0]);
	verify.Unmap();
	verify.Destroy();

	return isVerified;
}

void CompactionPool::InsertUAVBarriers(ComputeContext& ctx)
{
	ctx.InsertUAVBarrier(m_ParticleBuffer[0]);
	ctx.InsertUAVBarrier(m_ParticleBuffer[1]);
	ctx.InsertUAVBarrier(m_CounterBuffer, true);
}

#ifndef RELEASE
void CompactionPool::CollectCounterReadback()
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

	m_LastCounters.deadCount = m_MaxParticles - min(m_LastCounters.aliveCount, m_MaxParticles);
}
#endif
