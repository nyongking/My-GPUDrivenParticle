#include "pch.h"
#include "SpriteRenderer.h"

#include "EmitterInstance.h"
#include "GpuEmitterInstance.h"

#include "GraphicsCore.h"
#include "GraphicsCommon.h"
#include "BufferManager.h"
#include "CommandContext.h"
#include "RootSignature.h"
#include "PipelineState.h"
#include "Camera.h"

#include "TagLibrary.h"

#include "CompiledShaders/ParticleDrawPS.h"
#include "CompiledShaders/ParticleSpriteVS.h"

#include <DirectXMath.h>
#include <numeric>

using namespace Graphics;

namespace
{


	// HLSL cbuffer SpriteConstants 와 같은 배치 (112 바이트)
	struct alignas(16) SpriteConstants
	{
		DirectX::XMFLOAT4X4 viewProj;
		DirectX::XMFLOAT4   camRight;
		DirectX::XMFLOAT4   camUp;
		uint32_t            positionOffset;
		uint32_t            colorOffset;
		uint32_t            sizeOffset;
		uint32_t            capacity;

		uint32_t			sortEnabled;
		uint32_t			_pad[3];
	};
	static_assert(sizeof(SpriteConstants) == 128, "HLSL cbuffer SpriteConstants(112 B)와 일치해야 함");

	enum RootParams { kRootCBV, kRootSRVs, kNumRootParams };
	static const uint32_t kNumSRVs = 2;

	// 루트 시그니처 / PSO 는 모든 SpriteRenderer 가 같이 쓴다 (한 번만 만든다)
	bool          s_PipelineReady = false;
	RootSignature s_RootSig;
	GraphicsPSO   s_PSO(L"Particle Sprite PSO");

	uint32_t	RoundUp4(uint32_t v) { return (v + 3u) & ~3u; } // 4단위로 끊는다.
	float3		ToFloat3(const Math::Vector3& v) { return { float(v.GetX()), float(v.GetY()), float(v.GetZ()) }; }

	void InitializePipeline()
	{
		if (s_PipelineReady)
			return;

		s_RootSig.Reset(kNumRootParams, 0);
		s_RootSig[kRootCBV].InitAsConstantBuffer(0, D3D12_SHADER_VISIBILITY_VERTEX);
		s_RootSig[kRootSRVs].InitAsDescriptorRange(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 0, kNumSRVs, D3D12_SHADER_VISIBILITY_VERTEX);
		s_RootSig.Finalize(L"Particle Sprite RootSig");

		s_PSO.SetRootSignature(s_RootSig);
		s_PSO.SetRasterizerState(RasterizerTwoSided);

		s_PSO.SetBlendState(BlendPreMultiplied);

		s_PSO.SetDepthStencilState(DepthStateDisabled);
		s_PSO.SetRenderTargetFormat(g_SceneColorBuffer.GetFormat(), DXGI_FORMAT_UNKNOWN);

		s_PSO.SetPrimitiveTopologyType(D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE);
		s_PSO.SetVertexShader(g_pParticleSpriteVS, sizeof(g_pParticleSpriteVS));
		s_PSO.SetPixelShader(g_pParticleDrawPS, sizeof(g_pParticleDrawPS));
		s_PSO.Finalize();

		s_PipelineReady = true;
	}
}

RendererDesc SpriteRenderer::Desc()
{
	RendererDesc desc = {};

	desc.name = "Sprite Renderer";
	desc.reads =
	{
		{Particles::Position, &Types::Float3},
		{Particles::Color, &Types::Float4},
		{Particles::Size, &Types::Float}
	};

	return desc;
}

bool SpriteRenderer::Initialize(const EmitterInstance& emitter)
{
	m_Log.clear();
	m_Ready = false;

	if (!ReadLayout(emitter.Compiled().particles, emitter.Capacity()))
		return false;

	m_BufferBytes = static_cast<uint32_t>(emitter.ParticleDataBytes());
	m_ParticleBuffer.Create(L"Sprite Render Buffer", m_BufferBytes / 4, 4);

	m_SortedIndices.clear();

	const uint32_t idxCount = RoundUp4(m_Capacity);
	m_SortedIndices.resize(idxCount);
	std::iota(m_SortedIndices.begin(), m_SortedIndices.end(), 0u);
	m_SortedIndexBuffer.Create(L"Sorted Index Buffer", idxCount, sizeof(uint32_t), m_SortedIndices.data());

	InitializePipeline();

	m_Ready = true;
	return true;
}


bool SpriteRenderer::ReadLayout(const Block& layout, uint32_t capacity)
{
	const Slot* pos = layout.Find(Particles::Position);
	const Slot* col = layout.Find(Particles::Color);
	const Slot* siz = layout.Find(Particles::Size);

	if (!pos || !col || !siz)
	{
		m_Log = "[SpriteRenderer] Position/Color/Size 중 레이아웃에 없는 값이 있음. "
			"EmitterInstance::Initialize 에 SpriteRenderer::Desc() 를 넣었는지 확인\n";
		return false;
	}
	if (pos->type != &Types::Float3 || col->type != &Types::Float4 || siz->type != &Types::Float)
	{
		m_Log = "[SpriteRenderer] Position(float3) / Color(float4) / Size(float) 타입이 다름\n";
		return false;
	}

	m_PositionOffset = pos->offset;
	m_ColorOffset = col->offset;
	m_SizeOffset = siz->offset;
	m_Capacity = capacity;
	return true;
}

bool SpriteRenderer::InitializeGpu(const GpuEmitterInstance& emitter)
{
	m_Log.clear();
	m_Ready = false;

	if (!emitter.Ready() || !ReadLayout(emitter.Compiled().particles, emitter.Capacity()))
		return false;

	// 파티클 버퍼는 GPU 이미터 것을 쓰므로 만들지 않는다.
	// t1 은 정렬을 끈 상태라 읽지 않지만, SRV 테이블을 채우려고 작은 버퍼를 하나 둔다 (G3 에서 정렬 결과로 바뀜)
	alignas(16) const uint32_t dummy[4] = { 0, 1, 2, 3 };
	m_SortedIndexBuffer.Create(L"Sorted Index Buffer (GPU, unused)", 4, sizeof(uint32_t), dummy);
	m_BufferBytes = 0;

	InitializePipeline();

	m_Ready = true;
	return true;
}

void SpriteRenderer::Shutdown()
{
	m_SortedIndexBuffer.Destroy();
	m_ParticleBuffer.Destroy();

	m_SortedIndices.clear();

	m_Log.clear();
	m_Ready = false;
}

void SpriteRenderer::Render(GraphicsContext& gfx, const Math::Camera& camera, const EmitterInstance& emitter)
{
	if (!m_Ready || emitter.Count() == 0)
		return;

	// 1) 업로드 버퍼
	gfx.TransitionResource(m_ParticleBuffer, D3D12_RESOURCE_STATE_COPY_DEST, true);
	gfx.WriteBuffer(m_ParticleBuffer, 0, emitter.ParticleData(), m_BufferBytes);
	gfx.TransitionResource(m_ParticleBuffer, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, true);

	// 2) 정렬 여부에 따라 정렬 + 업로드 버퍼
	if (m_SortMode == SortMode::ViewDepth)
		UploadSortedIndices(gfx, camera, emitter);
	else
		gfx.TransitionResource(m_SortedIndexBuffer, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, true);

	// 3) 상수 버퍼 / 파이프라인 / SRV
	BindForDraw(gfx, camera, m_ParticleBuffer, m_SortMode == SortMode::ViewDepth);

	// 4) 그리기: 파티클 하나 = 사각형(정점 6개), 인스턴스 = 파티클 수
	gfx.DrawInstanced(6, emitter.Count());
}

void SpriteRenderer::RenderGpu(GraphicsContext& gfx, const Math::Camera& camera, GpuEmitterInstance& emitter)
{
	if (!m_Ready || !emitter.Ready())
		return;

	// GPU 이미터의 Tick 이 이미 전환해 두지만, 비교(CompareWithCpu) 등으로 상태가 바뀌었을 수 있어 한 번 더
	gfx.TransitionResource(emitter.ParticleBuffer(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
	gfx.TransitionResource(emitter.DrawArgs(), D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
	gfx.TransitionResource(m_SortedIndexBuffer, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, true);

	BindForDraw(gfx, camera, emitter.ParticleBuffer(), false);

	// 개수는 Finalize 가 Draw 인자 (6, alive, 0, 0) 로 써 둔 것. CPU 는 개수를 몰라도 된다
	gfx.DrawIndirect(emitter.DrawArgs());
}

void SpriteRenderer::BindForDraw(GraphicsContext& gfx, const Math::Camera& camera, ByteAddressBuffer& particles, bool sorted)
{
	SpriteConstants sc = {};
	DirectX::XMStoreFloat4x4(&sc.viewProj, camera.GetViewProjMatrix());
	DirectX::XMStoreFloat4(&sc.camRight, camera.GetRightVec());
	DirectX::XMStoreFloat4(&sc.camUp, camera.GetUpVec());
	sc.camRight.w = 0.f;
	sc.camUp.w = 0.f;
	sc.positionOffset = m_PositionOffset;
	sc.colorOffset = m_ColorOffset;
	sc.sizeOffset = m_SizeOffset;
	sc.capacity = m_Capacity;
	sc.sortEnabled = sorted ? 1u : 0u;

	gfx.SetRootSignature(s_RootSig);
	gfx.SetPipelineState(s_PSO);
	gfx.SetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	gfx.SetDynamicConstantBufferView(kRootCBV, sizeof(sc), &sc);

	const D3D12_CPU_DESCRIPTOR_HANDLE srvs[kNumSRVs] = { particles.GetSRV(), m_SortedIndexBuffer.GetSRV() };
	gfx.SetDynamicDescriptors(kRootSRVs, 0, kNumSRVs, srvs);
}

void SpriteRenderer::UploadSortedIndices(GraphicsContext& gfx, const Math::Camera& camera, const EmitterInstance& emitter)
{
	uint32_t count = emitter.Count();

	const float3* pos = reinterpret_cast<const float3*>(emitter.ParticleData()
		+ size_t(m_PositionOffset) * m_Capacity);

	SortBackToFront(pos, count,
		ToFloat3(camera.GetPosition()), ToFloat3(camera.GetForwardVec()),
		m_SortedIndices, m_SortScratch);

	const size_t bytes = size_t(RoundUp4(count)) * sizeof(uint32_t);
	gfx.TransitionResource(m_SortedIndexBuffer, D3D12_RESOURCE_STATE_COPY_DEST, true);
	gfx.WriteBuffer(m_SortedIndexBuffer, 0, m_SortedIndices.data(), bytes);
	gfx.TransitionResource(m_SortedIndexBuffer, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, true);


}
