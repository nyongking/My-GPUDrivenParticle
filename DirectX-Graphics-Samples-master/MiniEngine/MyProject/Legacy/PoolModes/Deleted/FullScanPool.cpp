#include "pch.h"
#include "FullScanPool.h"

#include "GraphicsCore.h"
#include "GraphicsCommon.h"
#include "BufferManager.h"
#include "CommandContext.h"
#include "RootSignature.h"
#include "PipelineState.h"
#include "Camera.h"

#include "ParticleUtility.h"
#include "ParticleEmitter.h"

#include "CompiledShaders/ParticleEmitFullScanCS.h"
#include "CompiledShaders/ParticleSimulateFullScanCS.h"
#include "CompiledShaders/ParticleDrawReuseVS.h"
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
	ComputePSO		s_EmitPSO(L"Particle Emit PSO");
	ComputePSO		s_SimulatePSO(L"Particle Simulate PSO");

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
		s_ComputeSig[kComputeUAVs].InitAsDescriptorTable(2);
		s_ComputeSig[kComputeUAVs].SetTableRange(0, D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 0, 1);
		s_ComputeSig[kComputeUAVs].SetTableRange(1, D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 2, 1);
		s_ComputeSig.Finalize(L"Particle Compute RoogSig");

		// 2) Draw RootSig
		s_DrawSig.Reset(kNumDrawRootParams, 0);
		s_DrawSig[kDrawCBVs].InitAsConstantBuffer(0, D3D12_SHADER_VISIBILITY_VERTEX);
		s_DrawSig[kDrawSRVs].InitAsDescriptorRange(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 0, 1, D3D12_SHADER_VISIBILITY_VERTEX);
		s_DrawSig.Finalize(L"Particle Draw RootSig");

		// ----------------------
		//			PSO
		// ----------------------
		// 1) Compute PSO
		s_EmitPSO.SetRootSignature(s_ComputeSig);
		s_EmitPSO.SetComputeShader(g_pParticleEmitFullScanCS, sizeof(g_pParticleEmitFullScanCS));
		s_EmitPSO.Finalize();

		s_SimulatePSO.SetRootSignature(s_ComputeSig);
		s_SimulatePSO.SetComputeShader(g_pParticleSimulateFullScanCS, sizeof(g_pParticleSimulateFullScanCS));
		s_SimulatePSO.Finalize();

		// 2) Draw PSO
		s_DrawPSO.SetRootSignature(s_DrawSig);
		s_DrawPSO.SetRasterizerState(RasterizerTwoSided);
		s_DrawPSO.SetBlendState(BlendAdditive);
		s_DrawPSO.SetDepthStencilState(DepthStateDisabled);
		s_DrawPSO.SetPrimitiveTopologyType(D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE);
		s_DrawPSO.SetRenderTargetFormat(g_SceneColorBuffer.GetFormat(), DXGI_FORMAT_UNKNOWN);
		s_DrawPSO.SetVertexShader(g_pParticleDrawReuseVS, sizeof(g_pParticleDrawReuseVS));
		s_DrawPSO.SetPixelShader(g_pParticleDrawPS, sizeof(g_pParticleDrawPS));
		s_DrawPSO.Finalize();

		s_Initialized = true;
	}
}

void FullScanPool::Initialize(uint32_t maxParticles)
{
	if (!s_Initialized)
		InitializeSetting();

	CreateBuffers(maxParticles);
}

void FullScanPool::CreateBuffers(uint32_t maxParticles)
{
	m_MaxParticles = maxParticles;

	// 1) Particle Buffer;
	std::vector<Particle> zeroParticles(m_MaxParticles);
	m_ParticleBuffer.Create(L"Particle Buffer", m_MaxParticles, sizeof(Particle), zeroParticles.data());

	// 2) Counter Buffer
	CounterSnapshot initSnapshot = {};
	initSnapshot.deadCount = m_MaxParticles;
	m_CounterBuffer.Create(L"Particle CounterBuffer", kCounterBufferSize / 4, sizeof(uint32_t), &initSnapshot);

	// Readback Buffer
	for (uint32_t i = 0; i < sReadbackLatency; ++i)
		m_CounterReadback[i].Create(L"Particle Counter ReadBack", kCounterBufferSize / 4, sizeof(uint32_t));
}

void FullScanPool::Shutdown()
{
	for (uint32_t i = 0; i < sReadbackLatency; ++i)
		m_CounterReadback[i].Destroy();
	m_CounterBuffer.Destroy();
	m_ParticleBuffer.Destroy();
}

void FullScanPool::Update(float deltaT, const EmitRequest& emitRequest)
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
	sc.baseColor = PackColor(255, 128, 45, 255); // TEMP ÁÖÈ²

	// -------------------------
	//		BEGIN COMPUTE
	// -------------------------
	ComputeContext& ctx = ComputeContext::Begin(L"Particle Update");

	alignas(16) static const uint32_t kZeros[4] = { 0, 0, 0, 0 };
	ctx.WriteBuffer(m_CounterBuffer, 0, kZeros, sizeof(kZeros));

	// 1) Convert Buffers -> UAV
	ctx.TransitionResource(m_ParticleBuffer, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
	ctx.TransitionResource(m_CounterBuffer, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, true);

	// 2) Bind
	ctx.SetRootSignature(s_ComputeSig);
	ctx.SetDynamicConstantBufferView(kComputeCBVs, sizeof(sc), &sc);

	const D3D12_CPU_DESCRIPTOR_HANDLE uavs[6] =
	{
		m_ParticleBuffer.GetUAV(), // u0
		m_CounterBuffer.GetUAV(), // u2
	};

	ctx.SetDynamicDescriptors(kComputeUAVs, 0, 2, uavs);

	// 3) Emit
	{
		ScopedTimer _t(L"Particle Emit", ctx);
		if (emitRequest.emitCount > 0)
		{
			ctx.SetPipelineState(s_EmitPSO);
			ctx.Dispatch1D(m_MaxParticles, sThreadGroupSize);
		}
	}
	
	ctx.InsertUAVBarrier(m_ParticleBuffer);
	ctx.InsertUAVBarrier(m_CounterBuffer, true);

	// 4) Simulate
	{
		ScopedTimer _t(L"Particle Simulate", ctx);
		ctx.SetPipelineState(s_SimulatePSO);
		ctx.Dispatch1D(m_MaxParticles, sThreadGroupSize);
	}
	
	// 5 Readback
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
}

void FullScanPool::Render(GraphicsContext& gfx, const Math::Camera& cam)
{
	gfx.TransitionResource(m_ParticleBuffer, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, true);

	DrawConstants dc = {};
	XMStoreFloat4x4(&dc.viewProj, cam.GetViewProjMatrix());
	XMStoreFloat4(&dc.camRight, cam.GetRightVec());	dc.camRight.w = 0.f;
	XMStoreFloat4(&dc.camUp, cam.GetUpVec());		dc.camUp.w = 0.f;

	gfx.SetRootSignature(s_DrawSig);
	gfx.SetPipelineState(s_DrawPSO);
	gfx.SetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	gfx.SetDynamicConstantBufferView(kDrawCBVs, sizeof(dc), &dc);

	gfx.SetDynamicDescriptor(kDrawSRVs, 0, m_ParticleBuffer.GetSRV());

	ScopedTimer _t(L"Particle Draw", gfx);
	gfx.DrawInstanced(6, m_MaxParticles);
}

#ifndef RELEASE
void FullScanPool::CollectCounterReadback()
{
	const uint32_t slot = m_ReadbackSlot;
	const uint64_t fence = m_ReadbackFence[slot];
	if (fence == 0)
		return;

	if (!g_CommandManager.IsFenceComplete(fence))
		g_CommandManager.WaitForFence(fence);

	std::memcpy(&m_LastCounters, m_CounterReadback[slot].Map(), sizeof(CounterSnapshot));
	m_CounterReadback[slot].Unmap();
	m_ReadbackFence[slot] = 0;
}
#endif
