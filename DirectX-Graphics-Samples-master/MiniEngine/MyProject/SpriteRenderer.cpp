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
#include "TextureManager.h"
#include "TextureConvert.h"

#include "TagLibrary.h"

#include "CompiledShaders/ParticleSpritePS.h"
#include "CompiledShaders/ParticleSpriteVS.h"

#include <DirectXMath.h>
#include <numeric>
#include <algorithm>

using namespace Graphics;

namespace
{
	// HLSL cbuffer SpriteConstants 와 같은 배치... 계속 추가될 수 있음
	struct alignas(16) SpriteConstants
	{
		DirectX::XMFLOAT4X4 viewProj;        // 0
		DirectX::XMFLOAT4   camRight;        // 64
		DirectX::XMFLOAT4   camUp;           // 80

		uint32_t            positionOffset;  // 96   (0xFFFFFFFF = 연결 없음 -> 기본값)
		uint32_t            colorOffset;     // 100
		uint32_t            sizeOffset;      // 104
		uint32_t            capacity;        // 108

		float4              defaultColor;    // 112
		float3              defaultPosition; // 128
		float               defaultSize;     // 140

		uint32_t            sortEnabled;     // 144
		uint32_t            subImageOffset;  // 148  (T2) Particles.SubImageIndex
		float               defaultSubImage; // 152
		uint32_t            subUVBlend;      // 156

		float               subImageSize[2]; // 160  (가로 칸, 세로 칸)
		float               _pad[2];         // 168
	};
	static_assert(sizeof(SpriteConstants) == 176, "HLSL cbuffer SpriteConstants(160 B)와 일치해야 함");
	static_assert(offsetof(SpriteConstants, defaultColor) == 112, "DefaultColor offset 112");
	static_assert(offsetof(SpriteConstants, defaultPosition) == 128, "DefaultPosition offset 128");
	static_assert(offsetof(SpriteConstants, sortEnabled) == 144, "SortEnabled offset 144");
	static_assert(offsetof(SpriteConstants, subImageOffset) == 148, "SubImageOffset offset 148");
	static_assert(offsetof(SpriteConstants, subImageSize) == 160, "SubImageSize offset 160");


	enum RootParams { kRootCBV, kRootSRVs, kRootTexture, kRootPSConstants, kNumRootParams };
	static const uint32_t kNumSRVs = 2;

	// 루트 시그니처 / PSO 는 모든 SpriteRenderer 가 같이 쓴다 (한 번만 만든다)
	bool          s_PipelineReady = false;
	RootSignature s_RootSig;
	GraphicsPSO   s_PSO(L"Particle Sprite PSO");

	// 텍스처를 고르지 않았을 때 쓰는 기본 원형 (예전 PS 의 falloff 를 텍스처로 구운 것)
	Texture       s_SoftCircle;

	void CreateSoftCircle()
	{
		const uint32_t kSize = 64;
		std::vector<uint32_t> texels(kSize * kSize);
		for (uint32_t y = 0; y < kSize; ++y)
		{
			for (uint32_t x = 0; x < kSize; ++x)
			{
				// 텍셀 중심을 [-1, 1] 로: 예전 PS 의 uv(corner) 와 같은 공간
				const float u = (x + 0.5f) / kSize * 2.f - 1.f;
				const float v = (y + 0.5f) / kSize * 2.f - 1.f;
				const float falloff = (std::max)(0.f, 1.f - (u * u + v * v));    // 가장자리 = 0, 중심 = 1
				const uint32_t a = static_cast<uint32_t>(falloff * falloff * 255.f + 0.5f);
				texels[y * kSize + x] = 0x00FFFFFFu | (a << 24);                 // RGBA8: 흰색 + alpha
			}
		}
		s_SoftCircle.Create2D(kSize * sizeof(uint32_t), kSize, kSize, DXGI_FORMAT_R8G8B8A8_UNORM, texels.data());
	}

	uint32_t	RoundUp4(uint32_t v) { return (v + 3u) & ~3u; } // 4단위로 끊는다.
	float3		ToFloat3(const Math::Vector3& v) { return { float(v.GetX()), float(v.GetY()), float(v.GetZ()) }; }

	void InitializePipeline()
	{
		if (s_PipelineReady)
			return;

		s_RootSig.Reset(kNumRootParams, 1);
		s_RootSig[kRootCBV].InitAsConstantBuffer(0, D3D12_SHADER_VISIBILITY_VERTEX);
		s_RootSig[kRootSRVs].InitAsDescriptorRange(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 0, kNumSRVs, D3D12_SHADER_VISIBILITY_VERTEX);
		// T1: PS 텍스처 t2 + 샘플러 s0 (uv 0~1 안에서만 쓰므로 Clamp: 반대편 가장자리가 번지지 않게)
		s_RootSig[kRootTexture].InitAsDescriptorRange(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 2, 1, D3D12_SHADER_VISIBILITY_PIXEL);
		s_RootSig.InitStaticSampler(0, SamplerLinearClampDesc, D3D12_SHADER_VISIBILITY_PIXEL);
		s_RootSig.Finalize(L"Particle Sprite RootSig");

		s_PSO.SetRootSignature(s_RootSig);
		s_PSO.SetRasterizerState(RasterizerTwoSided);

		s_PSO.SetBlendState(BlendPreMultiplied);

		s_PSO.SetDepthStencilState(DepthStateReadOnly);
		s_PSO.SetRenderTargetFormat(g_SceneColorBuffer.GetFormat(), g_SceneDepthBuffer.GetFormat());

		s_PSO.SetPrimitiveTopologyType(D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE);
		s_PSO.SetVertexShader(g_pParticleSpriteVS, sizeof(g_pParticleSpriteVS));
		s_PSO.SetPixelShader(g_pParticleSpritePS, sizeof(g_pParticleSpritePS));
		s_PSO.Finalize();

		CreateSoftCircle();

		s_PipelineReady = true;
	}
}

RendererDesc SpriteRenderer::Desc()
{
	RendererDesc desc = {};

	desc.name = "Sprite Renderer";
	desc.bindings =
	{
		MakeRenderBinding<float3>("Position", Particles::Position, float3{ 0.f, 0.f, 0.f }),
		MakeRenderBinding<float4>("Color",    Particles::Color,    float4{ 1.f, 1.f, 1.f, 1.f }),
		MakeRenderBinding<float>("Size",      Particles::Size,     0.1f)
	};

	return desc;
}

bool SpriteRenderer::Initialize(const EmitterInstance& emitter, const RendererDesc& desc)
{
	m_Log.clear();
	m_Ready = false;

	if (!ReadLayout(emitter.Compiled().particles, emitter.Capacity(), desc))
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
	const bool sorted = (m_SortMode == SortMode::ViewDepth) && (m_Offsets[kPosition] != kNoAttribute);
	if (sorted)
		UploadSortedIndices(gfx, camera, emitter);
	else
		gfx.TransitionResource(m_SortedIndexBuffer, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, true);

	// 3) 상수 버퍼 / 파이프라인 / SRV
	BindForDraw(gfx, camera, m_ParticleBuffer, m_SortMode == SortMode::ViewDepth);

	// 4) 그리기: 파티클 하나 = 사각형(정점 6개), 인스턴스 = 파티클 수
	gfx.DrawInstanced(6, emitter.Count());
}


bool SpriteRenderer::ReadLayout(const Block& layout, uint32_t capacity, const RendererDesc& desc)
{
	// Desc() 와 같은 칸 구성인지 (칸 순서 = 셰이더 cbuffer 순서)
	static const TypeInfo* kSlotTypes[kNumSlots] = { &Types::Float3, &Types::Float4, &Types::Float };
	if (desc.bindings.size() != kNumSlots)
	{
		m_Log = "[SpriteRenderer] 바인딩 칸 수가 다름 (SpriteRenderer::Desc() 로 만든 desc 를 넘겨야 함)\n";
		return false;
	}

	for (uint32_t i = 0; i < kNumSlots; ++i)
	{
		const RenderBinding& b = desc.bindings[i];
		if (b.type != kSlotTypes[i])
		{
			m_Log = "[SpriteRenderer] '" + b.slot + "' 칸 타입이 다름\n";
			return false;
		}

		// 연결된 속성이 레이아웃에 있으면 그 offset, 없으면 (아무도 안 씀 / 비워 둠) 기본값
		const Slot* s = b.attribute.empty() ? nullptr : layout.Find(b.attribute);
		if (s && s->type != b.type)
		{
			m_Log = "[SpriteRenderer] '" + b.slot + "' <- '" + b.attribute + "' 타입이 다름\n";
			return false;
		}
		m_Offsets[i] = s ? s->offset : kNoAttribute;
	}

	SetDefaults(desc);
	m_Capacity = capacity;
	return true;
}

bool SpriteRenderer::InitializeGpu(const GpuEmitterInstance& emitter, const RendererDesc& desc)
{
	m_Log.clear();
	m_Ready = false;

	if (!emitter.Ready() || !ReadLayout(emitter.Compiled().particles, emitter.Capacity(), desc))
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

void SpriteRenderer::SetDefaults(const RendererDesc& desc)
{
	if (desc.bindings.size() != kNumSlots)
		return;
	std::memcpy(&m_DefaultPosition, desc.bindings[kPosition].defaultValue.data(), sizeof(float3));
	std::memcpy(&m_DefaultColor, desc.bindings[kColor].defaultValue.data(), sizeof(float4));
	std::memcpy(&m_DefaultSize, desc.bindings[kSize].defaultValue.data(), sizeof(float));
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
	sc.positionOffset = m_Offsets[kPosition];
	sc.colorOffset = m_Offsets[kColor];
	sc.sizeOffset = m_Offsets[kSize];
	sc.capacity = m_Capacity;
	sc.defaultColor = m_DefaultColor;
	sc.defaultPosition = m_DefaultPosition;
	sc.defaultSize = m_DefaultSize;
	sc.sortEnabled = sorted ? 1u : 0u;

	gfx.SetRootSignature(s_RootSig);
	gfx.SetPipelineState(s_PSO);
	gfx.SetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	gfx.SetDynamicConstantBufferView(kRootCBV, sizeof(sc), &sc);

	const D3D12_CPU_DESCRIPTOR_HANDLE srvs[kNumSRVs] = { particles.GetSRV(), m_SortedIndexBuffer.GetSRV() };
	gfx.SetDynamicDescriptors(kRootSRVs, 0, kNumSRVs, srvs);
	gfx.SetDynamicDescriptor(kRootTexture, 0, TextureSRV());
}

void SpriteRenderer::UploadSortedIndices(GraphicsContext& gfx, const Math::Camera& camera, const EmitterInstance& emitter)
{
	uint32_t count = emitter.Count();

	const float3* pos = reinterpret_cast<const float3*>(emitter.ParticleData()
		+ size_t(m_Offsets[kPosition]) * m_Capacity);

	SortBackToFront(pos, count,
		ToFloat3(camera.GetPosition()), ToFloat3(camera.GetForwardVec()),
		m_SortedIndices, m_SortScratch);

	const size_t bytes = size_t(RoundUp4(count)) * sizeof(uint32_t);
	gfx.TransitionResource(m_SortedIndexBuffer, D3D12_RESOURCE_STATE_COPY_DEST, true);
	gfx.WriteBuffer(m_SortedIndexBuffer, 0, m_SortedIndices.data(), bytes);
	gfx.TransitionResource(m_SortedIndexBuffer, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, true);


}

bool SpriteRenderer::SetTexture(const std::wstring& path)
{
	m_Texture = nullptr;
	if (path.empty())
		return true;    // 기본 원형

	auto it = m_Textures.find(path);
	if (it == m_Textures.end())
	{
		// PNG 등은 처음 한 번 옆에 DDS 로 변환해 둔다 (원본이 더 새로우면 다시 변환)
		// 색 텍스처라 sRGB, 파티클은 알파가 중요하므로 4채널 유지
		CompileTextureOnDemand(path, TextureOptions(true, true));
		const std::wstring ddsPath = Utility::RemoveExtension(path) + L".dds";

		// 실패해도 magenta 로 그려져서 바로 눈에 띈다
		it = m_Textures.emplace(path, TextureManager::LoadDDSFromFile(ddsPath, Graphics::kMagenta2D, true)).first;
	}

	m_Texture = &it->second;
	if (!m_Texture->IsValid())
	{
		Utility::Printf(L"[SpriteRenderer] Texture load failed : %ws\n", path.c_str());
		return false;
	}
	return true;
}

void SpriteRenderer::ReleaseTextures()
{
	// 프레임이 아직 쓰고 있을 수 있으므로 GPU idle 뒤에만 부른다
	m_Texture = nullptr;
	m_Textures.clear();
}

void SpriteRenderer::SetSettings(const SpriteSettings& settings)
{
}

D3D12_CPU_DESCRIPTOR_HANDLE SpriteRenderer::TextureSRV() const
{
	return m_Texture ? m_Texture->GetSRV() : s_SoftCircle.GetSRV();
}
