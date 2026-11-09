#include "pch.h"
#include "SlotReusePool.h"

#include "GraphicsCore.h"
#include "GraphicsCommon.h"
#include "BufferManager.h"
#include "CommandContext.h"
#include "RootSignature.h"
#include "PipelineState.h"
#include "Camera.h"

#include "ParticleUtility.h"
#include "ParticleEmitter.h"

#include "CompiledShaders/ParticleEmitReuseCS.h"
#include "CompiledShaders/ParticleSimulateCS.h"
#include "CompiledShaders/ParticleSimulateFlagsCS.h"
#include "CompiledShaders/ParticleDrawReuseVS.h"
#include "CompiledShaders/ParticleDrawReuseFlagsVS.h"
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
	ComputePSO		s_EmitPSO(L"Particle Slot Reuse Emit PSO");
	ComputePSO		s_SimulatePSO(L"Particle Slot Reuse Simulate PSO");
	ComputePSO		s_SimulateFlagsPSO(L"Particle Slot Reuse Flags Simulate PSO");

	GraphicsPSO		s_DrawPSO(L"Particle Slot Reuse Draw PSO");
	GraphicsPSO		s_DrawFlagsPSO(L"Particle Slot Reuse Flags Draw PSO");


	void InitializeSetting()
	{
		if (s_Initialized)
			return;

		// ----------------------
		//		RootSignature
		// ----------------------

		// 1) Compute Root Sig
		s_ComputeSig.Reset(kNumComputeRootParams, 0);
		s_ComputeSig[kComputeCBVs].InitAsConstantBuffer(0);
		s_ComputeSig[kComputeUAVs].InitAsDescriptorRange(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 0, 4);
		s_ComputeSig.Finalize(L"Particle Slot Reuse Compute RootSig");

		// 2) Draw Root Sig
		s_DrawSig.Reset(kNumDrawRootParams, 0);
		s_DrawSig[kDrawCBVs].InitAsConstantBuffer(0, D3D12_SHADER_VISIBILITY_VERTEX);
		s_DrawSig[kDrawSRVs].InitAsDescriptorRange(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 0, 2, D3D12_SHADER_VISIBILITY_VERTEX);
		s_DrawSig.Finalize(L"Particle Slot Reuse Draw RootSig");

		// ----------------------
		//			PSO
		// ----------------------

		s_EmitPSO.SetRootSignature(s_ComputeSig);
		s_EmitPSO.SetComputeShader(g_pParticleEmitReuseCS, sizeof(g_pParticleEmitReuseCS));
		s_EmitPSO.Finalize();

		s_SimulatePSO.SetRootSignature(s_ComputeSig);
		s_SimulatePSO.SetComputeShader(g_pParticleSimulateCS, sizeof(g_pParticleSimulateCS));
		s_SimulatePSO.Finalize();

		s_SimulateFlagsPSO.SetRootSignature(s_ComputeSig);
		s_SimulateFlagsPSO.SetComputeShader(g_pParticleSimulateFlagsCS, sizeof(g_pParticleSimulateFlagsCS));
		s_SimulateFlagsPSO.Finalize();

		auto setupDrawPSO = [](GraphicsPSO& pso, const void* vs, size_t vsSize)
			{
				pso.SetRootSignature(s_DrawSig);
				pso.SetRasterizerState(RasterizerTwoSided);
				pso.SetBlendState(BlendAdditive);
				pso.SetDepthStencilState(DepthStateDisabled);
				pso.SetPrimitiveTopologyType(D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE);
				pso.SetRenderTargetFormat(g_SceneColorBuffer.GetFormat(), DXGI_FORMAT_UNKNOWN);
				pso.SetVertexShader(vs, vsSize);
				pso.SetPixelShader(g_pParticleDrawPS, sizeof(g_pParticleDrawPS));
				pso.Finalize();
			};

		setupDrawPSO(s_DrawPSO, g_pParticleDrawReuseVS, sizeof(g_pParticleDrawReuseVS));
		setupDrawPSO(s_DrawFlagsPSO, g_pParticleDrawReuseFlagsVS, sizeof(g_pParticleDrawReuseFlagsVS));

		s_Initialized = true;
	}
};

void SlotReusePool::Initialize(uint32_t maxParticles)
{
	if (!s_Initialized)
		InitializeSetting();

	CreateBuffers(maxParticles);

	m_initVerified = VerifyInitialState();
}

void SlotReusePool::CreateBuffers(uint32_t maxParticles)
{
	m_MaxParticles = maxParticles;

	// 1) Particle Buffer
	std::vector<Particle> zeroParticles(m_MaxParticles);
	m_ParticleBuffer.Create(L"Particle Buffer", m_MaxParticles, sizeof(Particle), zeroParticles.data());

	// 2) DeadList Buffer
	std::vector<uint32_t> zeroDeadList(m_MaxParticles);
	for (uint32_t i = 0; i < m_MaxParticles; ++i)
		zeroDeadList[i] = m_MaxParticles - 1 - i;
	m_DeadListBuffer.Create(L"Particle DeadList", m_MaxParticles, sizeof(uint32_t), zeroDeadList.data());

	// 3) AliveFlags Buffer
	std::vector<uint32_t> zeroFlags(m_MaxParticles, 0u);
	m_AliveFlagBuffer.Create(L"Particle AliveFlags", m_MaxParticles, sizeof(uint32_t), zeroFlags.data());

	// 4) Counter Buffer
	CounterSnapshot initSnapshot{}; // deadCount = paritcle count / 나머지 = 0
	initSnapshot.deadCount = m_MaxParticles;
	m_CounterBuffer.Create(L"Particle CounterBuffer", kCounterBufferSize / 4, sizeof(uint32_t), &initSnapshot);

	// Readback Buffer
	for (uint32_t i = 0; i < sReadbackLatency; ++i)
		m_CounterReadback[i].Create(L"Particle Counter ReadBack", kCounterBufferSize / 4, sizeof(uint32_t));

}

void SlotReusePool::Shutdown()
{
	for (uint32_t i = 0; i < sReadbackLatency; ++i)
		m_CounterReadback[i].Destroy();
	m_CounterBuffer.Destroy();
	m_AliveFlagBuffer.Destroy();
	m_DeadListBuffer.Destroy();
	m_ParticleBuffer.Destroy();
}

void SlotReusePool::Update(float deltaT, const EmitRequest& emitRequest)
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
	ctx.WriteBuffer(m_CounterBuffer, kEmitCount, kZeros, 4);

	// 1) Buffers -> UAV
	ctx.TransitionResource(m_ParticleBuffer, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
	ctx.TransitionResource(m_DeadListBuffer, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
	ctx.TransitionResource(m_AliveFlagBuffer, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
	ctx.TransitionResource(m_CounterBuffer, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, true);

	// 2) Bind
	ctx.SetRootSignature(s_ComputeSig);
	ctx.SetDynamicConstantBufferView(kComputeCBVs, sizeof(sc), &sc);

	const D3D12_CPU_DESCRIPTOR_HANDLE uavs[4]
		= { m_ParticleBuffer.GetUAV(), m_DeadListBuffer.GetUAV(), m_CounterBuffer.GetUAV(), m_AliveFlagBuffer.GetUAV() };
	ctx.SetDynamicDescriptors(kComputeUAVs, 0, 4, uavs);

	// 3) Emit
	{
		ScopedTimer _t(L"Particle Emit", ctx); // RAII
		if (emitRequest.emitCount > 0)
		{
			ctx.SetPipelineState(s_EmitPSO);
			ctx.Dispatch1D(emitRequest.emitCount, sThreadGroupSize);
		}
	}

	ctx.InsertUAVBarrier(m_ParticleBuffer);
	ctx.InsertUAVBarrier(m_DeadListBuffer);
	ctx.InsertUAVBarrier(m_AliveFlagBuffer);
	ctx.InsertUAVBarrier(m_CounterBuffer, true);

	// 4) Simulate
	{
		const bool useFlagArray = (m_Mode == Mode::SlotReuseFlags);
		ScopedTimer _t(L"Particle Simulate", ctx);
		ctx.SetPipelineState(useFlagArray ? s_SimulateFlagsPSO : s_SimulatePSO);
		ctx.Dispatch1D(m_MaxParticles, sThreadGroupSize);
	}

#ifndef RELEASE
	// 5) Readback
	const uint32_t slot = m_ReadbackSlot;
	ctx.TransitionResource(m_CounterBuffer, D3D12_RESOURCE_STATE_COPY_SOURCE);
	ctx.CopyBufferRegion(m_CounterReadback[m_ReadbackSlot], 0, m_CounterBuffer, 0, kCounterBufferSize);
	m_ReadbackFence[slot] = ctx.Finish();
	m_ReadbackSlot = (m_ReadbackSlot + 1) % sReadbackLatency;
#else
	ctx.Finish();
#endif

	++m_FrameIndex;
}

void SlotReusePool::Render(GraphicsContext& gfx, const Math::Camera& cam)
{
	gfx.TransitionResource(m_ParticleBuffer, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
	gfx.TransitionResource(m_AliveFlagBuffer, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
	const bool useFlagArray = (m_Mode == Mode::SlotReuseFlags);

	DrawConstants dc = {};
	XMStoreFloat4x4(&dc.viewProj, cam.GetViewProjMatrix());
	XMStoreFloat4(&dc.camRight, cam.GetRightVec());
	dc.camRight.w = 0.f;
	XMStoreFloat4(&dc.camUp, cam.GetUpVec());
	dc.camUp.w = 0.f;

	gfx.SetRootSignature(s_DrawSig);
	gfx.SetPipelineState(useFlagArray ? s_DrawFlagsPSO : s_DrawPSO);
	gfx.SetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	gfx.SetDynamicConstantBufferView(kDrawCBVs, sizeof(dc), &dc);

	const D3D12_CPU_DESCRIPTOR_HANDLE srvs[2] = { m_ParticleBuffer.GetSRV(), m_AliveFlagBuffer.GetSRV() };
	gfx.SetDynamicDescriptors(kDrawSRVs, 0, 2, srvs);

	ScopedTimer _t(L"Particle Draw", gfx);
	gfx.DrawInstanced(6, m_MaxParticles);
}

bool SlotReusePool::VerifyInitialState()
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


#ifndef RELEASE
void SlotReusePool::CollectCounterReadback()
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