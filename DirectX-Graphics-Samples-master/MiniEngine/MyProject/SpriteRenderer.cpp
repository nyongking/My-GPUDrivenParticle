#include "pch.h"
#include "SpriteRenderer.h"

#include "EmitterInstance.h"
#include "GpuEmitterInstance.h"
#include "ParticleRenderList.h"
#include "ParticleMaterial.h"

#include "GraphicsCore.h"
#include "GraphicsCommon.h"
#include "BufferManager.h"
#include "CommandContext.h"
#include "RootSignature.h"
#include "PipelineState.h"
#include "Camera.h"

#include "TagLibrary.h"
#include "BitonicSort.h"

#include "GlobalData.h"

#include "CompiledShaders/ParticleSpritePS.h"
#include "CompiledShaders/ParticleSpriteVS.h"
#include "CompiledShaders/ParticleSortKeyCS.h"

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
		DirectX::XMFLOAT4   camPosition;     // 96
		DirectX::XMFLOAT4   camForward;      // 112

		uint32_t            positionOffset;  // 128  (0xFFFFFFFF = 연결 없음 -> 기본값)
		uint32_t            colorOffset;     // 132
		uint32_t            sizeOffset;      // 136
		uint32_t            capacity;        // 140

		float4              defaultColor;    // 144
		float3              defaultPosition; // 160
		float               defaultSize;     // 172

		uint32_t            sortEnabled;     // 176
		uint32_t            subImageOffset;  // 180  (T2) Particles.SubImageIndex
		float               defaultSubImage; // 184
		uint32_t            subUVBlend;      // 188

		float               subImageSize[2]; // 192  (가로 칸, 세로 칸)
		uint32_t            sortStride;      // 200  정렬 목록 한 칸 바이트 (CPU 4 / GPU 8)
		uint32_t            spriteSizeOffset;// 204  연결이 없으면 (Size * 2, Size * 2)

		uint32_t            spriteRotationOffset;   // 208
		float               defaultSpriteRotation;  // 212
		uint32_t            velocityOffset;         // 216
		uint32_t            spriteFacingOffset;     // 220

		float3              defaultVelocity;        // 224
		uint32_t            spriteAlignmentOffset;  // 236
		float3              defaultSpriteFacing;    // 240
		uint32_t            facingMode;             // 252
		float3              defaultSpriteAlignment; // 256
		uint32_t            alignmentMode;          // 268

		float2              pivot;                  // 272
		float               cameraOffset;           // 280
		float               velocityStretch;        // 284
		float               blendNear;              // 288
		float               blendFar;               // 292
		float               _pad[2];                // 296
	};
	static_assert(sizeof(SpriteConstants) == 304, "HLSL cbuffer SpriteConstants(304 B)와 일치해야 함");
	static_assert(offsetof(SpriteConstants, positionOffset) == 128, "PositionOffset offset 128");
	static_assert(offsetof(SpriteConstants, defaultColor) == 144, "DefaultColor offset 144");
	static_assert(offsetof(SpriteConstants, sortEnabled) == 176, "SortEnabled offset 176");
	static_assert(offsetof(SpriteConstants, subImageSize) == 192, "SubImageSize offset 192");
	static_assert(offsetof(SpriteConstants, spriteRotationOffset) == 208, "SpriteRotationOffset offset 208");
	static_assert(offsetof(SpriteConstants, defaultVelocity) == 224, "DefaultVelocity offset 224");
	static_assert(offsetof(SpriteConstants, defaultSpriteFacing) == 240, "DefaultSpriteFacing offset 240");
	static_assert(offsetof(SpriteConstants, defaultSpriteAlignment) == 256, "DefaultSpriteAlignment offset 256");
	static_assert(offsetof(SpriteConstants, pivot) == 272, "Pivot offset 272");
	static_assert(offsetof(SpriteConstants, blendFar) == 292, "BlendFar offset 292");

	// GPU 정렬 키 생성 (ParticleSortKeyCS.hlsl 의 cbuffer SortKeyConstants 와 같은 배치)
	struct SortKeyConstants
	{
		uint32_t positionOffset;  // 0
		uint32_t capacity;        // 4
		uint32_t countOffset;     // 8   개수 버퍼(DrawArgs) 안의 alive 위치
		uint32_t _pad0;           // 12
		float    camPos[3];       // 16
		float    _pad1;           // 28
		float    camForward[3];   // 32
		float    _pad2;           // 44
	};
	static_assert(sizeof(SortKeyConstants) == 48, "SortKeyConstants 48 B");

	struct PSConstants
	{
		uint32_t blendMode;   
		uint32_t textureAlpha;
		float    time;        
		uint32_t effectMode;  
		float    cellSize[2]; 
	};
	static_assert(sizeof(PSConstants) == 24, "HLSL SpritePSConstants (6 x 4 B) 와 같아야 함");

	enum SortKeyRootParams { kSortKeyCB, kSortKeySRVs, kSortKeyUAV, kNumSortKeyRootParams };
	const uint32_t kSortKeyGroupSize = 256;            // ParticleSortKeyCS 의 numthreads
	const uint32_t kDrawArgsInstanceCountOffset = 4;   // D3D12_DRAW_ARGUMENTS::InstanceCount (= alive)

	enum RootParams { kRootCBV, kRootSRVs, kRootTexture, kRootPSConstants, kRootMaterialCB, kNumRootParams };
	static const uint32_t kNumSRVs = 2;

	// 루트 시그니처 / PSO 는 모든 SpriteRenderer 가 같이 쓴다 (한 번만 만든다)
	bool          s_PipelineReady = false;
	RootSignature s_RootSig;
	GraphicsPSO   s_PSO(L"Particle Sprite PSO");

	// GPU 정렬 키 생성용 (모든 SpriteRenderer 공용)
	RootSignature s_SortKeyRootSig;
	ComputePSO    s_SortKeyPSO(L"Particle Sort Key CS");

	// 텍스처를 고르지 않았을 때 쓰는 기본 원형 (예전 PS 의 falloff 를 텍스처로 구운 것)
	Texture       s_SoftCircle;

	// 기본 텍스처 생성
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

		s_RootSig.Reset(kNumRootParams, 2);
		s_RootSig[kRootCBV].InitAsConstantBuffer(0, D3D12_SHADER_VISIBILITY_VERTEX);
		s_RootSig[kRootSRVs].InitAsDescriptorRange(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 0, kNumSRVs, D3D12_SHADER_VISIBILITY_VERTEX);
		
		// 텍스처 및 샘플러 설정
		s_RootSig[kRootTexture].InitAsDescriptorRange(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 2, ParticleMaterial::kNumTextureSlot, D3D12_SHADER_VISIBILITY_PIXEL);

		s_RootSig.InitStaticSampler(0, SamplerLinearClampDesc, D3D12_SHADER_VISIBILITY_PIXEL);
		s_RootSig.InitStaticSampler(1, SamplerLinearWrapDesc, D3D12_SHADER_VISIBILITY_PIXEL);

		// Blendmode 상수버퍼 설정
		s_RootSig[kRootPSConstants].InitAsConstants(1, sizeof(PSConstants) / 4, D3D12_SHADER_VISIBILITY_PIXEL);

		// Material Cbuffer 설정
		s_RootSig[kRootMaterialCB].InitAsConstantBuffer(2, D3D12_SHADER_VISIBILITY_PIXEL);


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

		// b0 = SortKeyConstants, t0 = 파티클, t1 = 개수 버퍼(DrawArgs), u0 = 정렬 목록
		s_SortKeyRootSig.Reset(kNumSortKeyRootParams, 0);
		s_SortKeyRootSig[kSortKeyCB].InitAsConstants(0, sizeof(SortKeyConstants) / 4);
		s_SortKeyRootSig[kSortKeySRVs].InitAsDescriptorRange(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 0, 2);
		s_SortKeyRootSig[kSortKeyUAV].InitAsDescriptorRange(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 0, 1);
		s_SortKeyRootSig.Finalize(L"Particle Sort Key RootSig");

		s_SortKeyPSO.SetRootSignature(s_SortKeyRootSig);
		s_SortKeyPSO.SetComputeShader(g_pParticleSortKeyCS, sizeof(g_pParticleSortKeyCS));
		s_SortKeyPSO.Finalize();

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
		MakeRenderBinding<float>("Size",      Particles::Size,     0.1f),
		MakeRenderBinding<float>("SubImageIndex",	Particles::SubImageIndex, 0.f),
		// 빌보드: SpriteSize 기본값은 쓰지 않는다 (연결이 없으면 Size 로 정사각형)
		MakeRenderBinding<float2>("SpriteSize",      Particles::SpriteSize,      float2{ 0.2f, 0.2f }),
		MakeRenderBinding<float>("SpriteRotation",   Particles::SpriteRotation,  0.f),
		MakeRenderBinding<float3>("Velocity",        Particles::Velocity,        float3{ 0.f, 0.f, 0.f }),
		MakeRenderBinding<float3>("SpriteFacing",    Particles::SpriteFacing,    float3{ 0.f, 1.f, 0.f }),
		MakeRenderBinding<float3>("SpriteAlignment", Particles::SpriteAlignment, float3{ 0.f, 1.f, 0.f })
	};

	return desc;
}

const char* SpriteFacingName(SpriteFacingMode mode)
{
	switch (mode)
	{
	case SpriteFacingMode::FaceCameraPlane:         return "Face Camera Plane";
	case SpriteFacingMode::FaceCameraPosition:      return "Face Camera Position";
	case SpriteFacingMode::CustomFacingVector:      return "Custom Facing Vector";
	case SpriteFacingMode::FaceCameraDistanceBlend: return "Face Camera Distance Blend";
	default:                                        return "?";
	}
}

const char* SpriteAlignmentName(SpriteAlignment mode)
{
	switch (mode)
	{
	case SpriteAlignment::Unaligned:       return "Unaligned";
	case SpriteAlignment::VelocityAligned: return "Velocity Aligned";
	case SpriteAlignment::CustomAlignment: return "Custom Alignment";
	default:                               return "?";
	}
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
	m_GpuSortBuffer.Destroy();
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
	BindForDraw(gfx, camera, m_ParticleBuffer, m_SortedIndexBuffer, sorted, sizeof(uint32_t));

	// 4) 그리기: 파티클 하나 = 사각형(정점 6개), 인스턴스 = 파티클 수
	gfx.DrawInstanced(6, emitter.Count());
}


bool SpriteRenderer::ReadLayout(const Block& layout, uint32_t capacity, const RendererDesc& desc)
{
	// Desc() 와 같은 칸 구성인지 (칸 순서 = 셰이더 cbuffer 순서)
	static const TypeInfo* kSlotTypes[kNumSlots] =
	{
		&Types::Float3, &Types::Float4, &Types::Float, &Types::Float,
		&Types::Float2, &Types::Float, &Types::Float3, &Types::Float3, &Types::Float3
	};
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
	// t1 = GPU 정렬 목록 uint2(index, key). 개수는 GPU 만 알기 때문에 capacity 만큼 잡는다
	//   BitonicSort 는 개수 버퍼의 alive 까지만 읽고 쓰므로 초기값은 필요 없다 (정렬을 끈 상태에서도 SRV 자리를 채움)
	m_GpuSortBuffer.Create(L"GPU Sort Key-Index List", (std::max)(m_Capacity, 1u), sizeof(uint32_t) * 2);
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
	std::memcpy(&m_DefaultSubImage, desc.bindings[kSubImageIndex].defaultValue.data(), sizeof(float));
	std::memcpy(&m_DefaultSpriteRotation, desc.bindings[kSpriteRotation].defaultValue.data(), sizeof(float));
	std::memcpy(&m_DefaultVelocity, desc.bindings[kVelocity].defaultValue.data(), sizeof(float3));
	std::memcpy(&m_DefaultSpriteFacing, desc.bindings[kSpriteFacing].defaultValue.data(), sizeof(float3));
	std::memcpy(&m_DefaultSpriteAlignment, desc.bindings[kSpriteAlignment].defaultValue.data(), sizeof(float3));
}


void SpriteRenderer::RenderGpu(GraphicsContext& gfx, const Math::Camera& camera, GpuEmitterInstance& emitter)
{
	if (!m_Ready || !emitter.Ready())
		return;

	gfx.TransitionResource(emitter.ParticleBuffer(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

	// 정렬 (CPU 경로와 같은 조건). 개수를 CPU 로 읽지 않고 DrawArgs 의 alive 를 GPU 에서 바로 쓴다
	const bool sorted = (m_SortMode == SortMode::ViewDepth) && (m_Offsets[kPosition] != kNoAttribute);
	if (sorted)
		SortOnGpu(gfx, camera, emitter);

	gfx.TransitionResource(emitter.DrawArgs(), D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
	gfx.TransitionResource(m_GpuSortBuffer, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, true);

	BindForDraw(gfx, camera, emitter.ParticleBuffer(), m_GpuSortBuffer, sorted, sizeof(uint32_t) * 2);

	// 개수는 Finalize 가 Draw 인자 (6, alive, 0, 0) 로 써둔 것. CPU 는 개수를 몰라도 된다
	gfx.DrawIndirect(emitter.DrawArgs());
}

void SpriteRenderer::SortOnGpu(GraphicsContext& gfx, const Math::Camera& camera, GpuEmitterInstance& emitter)
{
	ComputeContext& cc = gfx.GetComputeContext();
	IndirectArgsBuffer& countBuffer = emitter.DrawArgs();   // [4] = alive (Finalize 가 씀)

	// 1) 키 생성: 파티클 i -> uint2(i, 깊이 키). alive 이후 칸은 셰이더가 건너뛴다
	SortKeyConstants kc = {};
	kc.positionOffset = m_Offsets[kPosition];
	kc.capacity = m_Capacity;
	kc.countOffset = kDrawArgsInstanceCountOffset;
	const float3 camPos = ToFloat3(camera.GetPosition());
	const float3 camFwd = ToFloat3(camera.GetForwardVec());
	kc.camPos[0] = camPos.x; kc.camPos[1] = camPos.y; kc.camPos[2] = camPos.z;
	kc.camForward[0] = camFwd.x; kc.camForward[1] = camFwd.y; kc.camForward[2] = camFwd.z;

	cc.TransitionResource(countBuffer, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
	cc.TransitionResource(m_GpuSortBuffer, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, true);

	cc.SetRootSignature(s_SortKeyRootSig);
	cc.SetPipelineState(s_SortKeyPSO);
	cc.SetConstantArray(kSortKeyCB, sizeof(kc) / 4, &kc);
	const D3D12_CPU_DESCRIPTOR_HANDLE srvs[2] = { emitter.ParticleBuffer().GetSRV(), countBuffer.GetSRV() };
	cc.SetDynamicDescriptors(kSortKeySRVs, 0, 2, srvs);
	cc.SetDynamicDescriptor(kSortKeyUAV, 0, m_GpuSortBuffer.GetUAV());
	// 개수를 CPU 가 모르므로 capacity 기준으로 띄우고, alive 를 넘는 스레드는 바로 끝난다
	cc.Dispatch((m_Capacity + kSortKeyGroupSize - 1) / kSortKeyGroupSize, 1, 1);

	// 2) 정렬: 키 내림차순 = 카메라에서 먼 것부터 (CPU SortBackToFront 와 같은 순서)
	//    BitonicSort 가 키 목록 UAV 배리어 / 개수 버퍼 SRV 전환을 직접 한다
	BitonicSort::Sort(cc, m_GpuSortBuffer, countBuffer, kDrawArgsInstanceCountOffset, false, false);
}

void SpriteRenderer::SubmitToList(GraphicsContext& gfx, ParticleRenderList& list, const EmitterInstance& emitter)
{
	if (!m_Ready || emitter.Count() == 0)
		return;

	// CPU 시뮬레이션 결과를 올린다 (Render 의 1 단계와 같음). 개수는 CPU 가 알고 있으므로 상수로 넘긴다
	gfx.TransitionResource(m_ParticleBuffer, D3D12_RESOURCE_STATE_COPY_DEST, true);
	gfx.WriteBuffer(m_ParticleBuffer, 0, emitter.ParticleData(), m_BufferBytes);
	gfx.TransitionResource(m_ParticleBuffer, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, true);

	ParticleRenderList::Source src = MakeListSource();
	src.particles = &m_ParticleBuffer;
	src.constCount = emitter.Count();
	list.Add(gfx, src);
}

void SpriteRenderer::SubmitToListGpu(GraphicsContext& gfx, ParticleRenderList& list, GpuEmitterInstance& emitter)
{
	if (!m_Ready || !emitter.Ready())
		return;

	// 개수는 Finalize 가 DrawArgs.InstanceCount 에 써 둔 alive 를 GPU 에서 바로 읽는다
	ParticleRenderList::Source src = MakeListSource();
	src.particles = &emitter.ParticleBuffer();
	src.countBuffer = &emitter.DrawArgs();
	src.countOffset = kDrawArgsInstanceCountOffset;
	list.Add(gfx, src);
}

ParticleRenderList::Source SpriteRenderer::MakeListSource() const
{
	ParticleRenderList::Source src;
	src.capacity = m_Capacity;
	src.positionOffset = m_Offsets[kPosition];
	src.colorOffset = m_Offsets[kColor];
	src.sizeOffset = m_Offsets[kSize];
	src.subImageOffset = m_Offsets[kSubImageIndex];
	src.defaultPosition = m_DefaultPosition;
	src.defaultColor = m_DefaultColor;
	src.defaultSize = m_DefaultSize;
	src.defaultSubImage = m_DefaultSubImage;
	src.spriteSizeOffset = m_Offsets[kSpriteSize];
	src.spriteRotationOffset = m_Offsets[kSpriteRotation];
	src.velocityOffset = m_Offsets[kVelocity];
	src.spriteFacingOffset = m_Offsets[kSpriteFacing];
	src.spriteAlignmentOffset = m_Offsets[kSpriteAlignment];
	src.defaultSpriteRotation = m_DefaultSpriteRotation;
	src.defaultVelocity = m_DefaultVelocity;
	src.defaultSpriteFacing = m_DefaultSpriteFacing;
	src.defaultSpriteAlignment = m_DefaultSpriteAlignment;
	src.facingMode = static_cast<uint32_t>(m_Settings.facing);
	src.alignmentMode = static_cast<uint32_t>(m_Settings.alignment);
	src.pivot = m_Settings.pivot;
	src.cameraOffset = m_Settings.cameraOffset;
	src.velocityStretch = m_Settings.velocityStretch;
	src.blendNear = m_Settings.blendNear;
	src.blendFar = m_Settings.blendFar;
	src.subImageCols = m_Settings.subImageCols;
	src.subImageRows = m_Settings.subImageRows;
	src.subUVBlend = m_Settings.subUVBlend;
	src.blendMode = static_cast<uint32_t>(m_Settings.blend);
	src.textureAlpha = static_cast<uint32_t>(m_Settings.textureAlpha);
	src.texture = TextureSRV(ParticleMaterial::kBaseTexture);

	return src;
}

void SpriteRenderer::BindForDraw(GraphicsContext& gfx, const Math::Camera& camera, ByteAddressBuffer& particles,
	ByteAddressBuffer& sortedIndices, bool sorted, uint32_t sortStride)
{
	SpriteConstants sc = {};
	DirectX::XMStoreFloat4x4(&sc.viewProj, camera.GetViewProjMatrix());
	DirectX::XMStoreFloat4(&sc.camRight, camera.GetRightVec());
	DirectX::XMStoreFloat4(&sc.camUp, camera.GetUpVec());
	sc.camRight.w = 0.f;
	sc.camUp.w = 0.f;
	{
		const float3 camPos = ToFloat3(camera.GetPosition());
		const float3 camFwd = ToFloat3(camera.GetForwardVec());
		sc.camPosition = { camPos.x, camPos.y, camPos.z, 0.f };
		sc.camForward = { camFwd.x, camFwd.y, camFwd.z, 0.f };
	}
	sc.positionOffset = m_Offsets[kPosition];
	sc.colorOffset = m_Offsets[kColor];
	sc.sizeOffset = m_Offsets[kSize];
	sc.capacity = m_Capacity;
	sc.defaultColor = m_DefaultColor;
	sc.defaultPosition = m_DefaultPosition;
	sc.defaultSize = m_DefaultSize;
	sc.sortEnabled = sorted ? 1u : 0u;
	sc.subImageOffset = m_Offsets[kSubImageIndex];
	sc.defaultSubImage = m_DefaultSubImage;
	sc.subUVBlend = m_Settings.subUVBlend ? 1u : 0u;
	sc.subImageSize[0] = static_cast<float>(m_Settings.subImageCols);
	sc.subImageSize[1] = static_cast<float>(m_Settings.subImageRows);
	sc.sortStride = sortStride;

	sc.spriteSizeOffset = m_Offsets[kSpriteSize];
	sc.spriteRotationOffset = m_Offsets[kSpriteRotation];
	sc.defaultSpriteRotation = m_DefaultSpriteRotation;
	sc.velocityOffset = m_Offsets[kVelocity];
	sc.spriteFacingOffset = m_Offsets[kSpriteFacing];
	sc.defaultVelocity = m_DefaultVelocity;
	sc.spriteAlignmentOffset = m_Offsets[kSpriteAlignment];
	sc.defaultSpriteFacing = m_DefaultSpriteFacing;
	sc.facingMode = static_cast<uint32_t>(m_Settings.facing);
	sc.defaultSpriteAlignment = m_DefaultSpriteAlignment;
	sc.alignmentMode = static_cast<uint32_t>(m_Settings.alignment);
	sc.pivot = m_Settings.pivot;
	sc.cameraOffset = m_Settings.cameraOffset;
	sc.velocityStretch = m_Settings.velocityStretch;
	sc.blendNear = m_Settings.blendNear;
	sc.blendFar = m_Settings.blendFar;

	gfx.SetRootSignature(s_RootSig);
	gfx.SetPipelineState(s_PSO);
	gfx.SetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	gfx.SetDynamicConstantBufferView(kRootCBV, sizeof(sc), &sc);

	const D3D12_CPU_DESCRIPTOR_HANDLE srvs[kNumSRVs] = { particles.GetSRV(), sortedIndices.GetSRV() };
	gfx.SetDynamicDescriptors(kRootSRVs, 0, kNumSRVs, srvs);

	D3D12_CPU_DESCRIPTOR_HANDLE texs[ParticleMaterial::kNumTextureSlot];
	for (uint32_t i = 0; i < ParticleMaterial::kNumTextureSlot; ++i)
		texs[i] = TextureSRV(i);

	gfx.SetDynamicDescriptors(kRootTexture, 0, ParticleMaterial::kNumTextureSlot, texs);

	PSConstants pc = {};
	pc.blendMode = static_cast<uint32_t>(m_Settings.blend);
	pc.textureAlpha = static_cast<uint32_t>(m_Settings.textureAlpha);
	pc.time = sGlobal.exeTime;
	pc.effectMode = m_Material ? static_cast<uint32_t>(m_Material->GetEffect()) : 0u;
	pc.cellSize[0] = 1.f / static_cast<float>(m_Settings.subImageCols);    // SetSettings 가 1 이상으로 맞춰 둔다
	pc.cellSize[1] = 1.f / static_cast<float>(m_Settings.subImageRows);
	gfx.SetConstantArray(kRootPSConstants, sizeof(pc) / 4, &pc);

	if (m_Material)
	{
		gfx.SetDynamicConstantBufferView(kRootMaterialCB, sizeof(float4) * ParticleMaterial::kMaxParams, m_Material->Params());
	}
	else
	{
		float4 defaults[ParticleMaterial::kMaxParams];
		for (uint32_t k = 0; k < ParticleMaterial::kMaxParams; ++k)
			defaults[k] = ParticleMaterial::DefaultParam(k);
		gfx.SetDynamicConstantBufferView(kRootMaterialCB, sizeof(defaults), defaults);
	}

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


void SpriteRenderer::SetSettings(const SpriteSettings& settings)
{
	m_Settings = settings;

	m_Settings.subImageCols = max(m_Settings.subImageCols, 1u);
	m_Settings.subImageRows = max(m_Settings.subImageRows, 1u);
}

D3D12_CPU_DESCRIPTOR_HANDLE SpriteRenderer::TextureSRV(uint32_t slot) const
{
	const D3D12_CPU_DESCRIPTOR_HANDLE srv = m_Material ? m_Material->TextureSRV(slot) : D3D12_CPU_DESCRIPTOR_HANDLE{ 0 };
	if (srv.ptr != 0)
		return srv;
	
	return (slot == ParticleMaterial::kBaseTexture) ?
		s_SoftCircle.GetSRV() : Graphics::GetDefaultTexture(Graphics::kWhiteOpaque2D);
}
