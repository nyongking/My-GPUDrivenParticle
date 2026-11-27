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
#include "EngineProfiling.h"

#include "TagLibrary.h"
#include "BitonicSort.h"

#include "GlobalData.h"


#include "CompiledShaders/ParticleSpritePS.h"
#include "CompiledShaders/SpritePresetNonePS.h"
#include "CompiledShaders/SpritePresetPatternPS.h"
#include "CompiledShaders/SpritePresetTintPS.h"
#include "CompiledShaders/SpritePresetDissolvePS.h"
#include "CompiledShaders/SpritePresetDistortionPS.h"
#include "CompiledShaders/SpritePresetGlowPS.h"
#include "CompiledShaders/ParticleSpriteVS.h"
#include "CompiledShaders/ParticleSortKeyCS.h"

#include <DirectXMath.h>
#include <numeric>
#include <algorithm>

using namespace Graphics;

namespace
{
	// HLSL cbuffer SpriteConstants 와 같은 배치 (ParticleSpriteVS.hlsl)
	//   파티클 값은 고정 렌더 구조체에서 읽으므로 속성 offset / 기본값이 없다 (Facing / Alignment 벡터만 예외)
	struct alignas(16) SpriteConstants
	{
		DirectX::XMFLOAT4X4 viewProj;        // 0
		DirectX::XMFLOAT4   camRight;        // 64
		DirectX::XMFLOAT4   camUp;           // 80
		DirectX::XMFLOAT4   camPosition;     // 96
		DirectX::XMFLOAT4   camForward;      // 112

		uint32_t            sortEnabled;     // 128
		uint32_t            sortStride;      // 132  정렬 목록 한 칸 바이트 (CPU 4 / GPU 8)
		uint32_t            subUVBlend;      // 136
		uint32_t            facingMode;      // 140

		float               subImageSize[2]; // 144  (가로 칸, 세로 칸)
		float2              pivot;           // 152

		uint32_t            alignmentMode;   // 160
		float               cameraOffset;    // 164
		float               velocityStretch; // 168
		float               blendNear;       // 172

		float               blendFar;        // 176
		uint32_t            capacity;        // 180  가변 속성 버퍼의 SoA 열 길이
		uint32_t            spriteFacingOffset;     // 184  (0xFFFFFFFF = 속성 없음 -> 기본값)
		uint32_t            spriteAlignmentOffset;  // 188

		float3              defaultSpriteFacing;    // 192
		float               _pad0;                  // 204
		float3              defaultSpriteAlignment; // 208
		float               _pad1;                  // 220
	}; // 224
	static_assert(sizeof(SpriteConstants) == 224, "HLSL cbuffer SpriteConstants(224 B)와 일치해야 함");
	static_assert(offsetof(SpriteConstants, sortEnabled) == 128, "SortEnabled offset 128");
	static_assert(offsetof(SpriteConstants, subImageSize) == 144, "SubImageSize offset 144");
	static_assert(offsetof(SpriteConstants, alignmentMode) == 160, "AlignmentMode offset 160");
	static_assert(offsetof(SpriteConstants, blendFar) == 176, "BlendFar offset 176");
	static_assert(offsetof(SpriteConstants, defaultSpriteFacing) == 192, "DefaultSpriteFacing offset 192");
	static_assert(offsetof(SpriteConstants, defaultSpriteAlignment) == 208, "DefaultSpriteAlignment offset 208");

	// GPU 정렬 키 생성 (ParticleSortKeyCS.hlsl 의 cbuffer SortKeyConstants 와 같은 배치)
	struct SortKeyConstants
	{
		uint32_t capacity;        // 0
		uint32_t countOffset;     // 4   개수 버퍼(DrawArgs) 안의 alive 위치
		uint32_t _pad0[2];        // 8
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

	struct MaterialProgramConstants
	{
		uint32_t      useVM;
		uint32_t      instrCount;
		uint32_t      outputReg;
		uint32_t      _pad;
	};
	static_assert(sizeof(MaterialProgramConstants) == 16, "HLSL MaterialProgramCB");

	enum RootParams { kRootCBV, kRootSRVs, kRootTexture, kRootPSConstants, kRootMaterialCB, kRootMaterialProgram, kRootMaterialInstrs, kNumRootParams };
	alignas(16) const MaterialInstr kEmptyInstr = {};
	static const uint32_t kNumSRVs = 3;     // VS: t0 렌더 구조체, t1 정렬 목록, t2 가변 속성 (SoA)

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
		
	// Preset 모드의 효과별 픽셀 셰이더 (ParticleMaterial::Effect 순서 그대로).
	//   효과마다 셰이더가 따로라서 그릴 때 EffectMode 분기가 없다.
	//   Preset 을 늘릴 때: Effect 에 항목 추가 -> Shaders/SpritePreset<이름>PS.hlsl 작성 -> 여기에 한 줄
	struct PresetShader
	{
		ParticleMaterial::Effect effect;
		const char*              file;      // UI 표시용
		const void*              ps;
		size_t                   size;
	};
	const PresetShader kPresetShaders[] =
	{
		{ ParticleMaterial::Effect::None,       "SpritePresetNonePS.hlsl",       g_pSpritePresetNonePS,       sizeof(g_pSpritePresetNonePS) },
		{ ParticleMaterial::Effect::Pattern,    "SpritePresetPatternPS.hlsl",    g_pSpritePresetPatternPS,    sizeof(g_pSpritePresetPatternPS) },
		{ ParticleMaterial::Effect::Tint,       "SpritePresetTintPS.hlsl",       g_pSpritePresetTintPS,       sizeof(g_pSpritePresetTintPS) },
		{ ParticleMaterial::Effect::Dissolve,   "SpritePresetDissolvePS.hlsl",   g_pSpritePresetDissolvePS,   sizeof(g_pSpritePresetDissolvePS) },
		{ ParticleMaterial::Effect::Distortion, "SpritePresetDistortionPS.hlsl", g_pSpritePresetDistortionPS, sizeof(g_pSpritePresetDistortionPS) },
		{ ParticleMaterial::Effect::Glow,       "SpritePresetGlowPS.hlsl",       g_pSpritePresetGlowPS,       sizeof(g_pSpritePresetGlowPS) },
	};
	static_assert(sizeof(kPresetShaders) / sizeof(kPresetShaders[0]) == size_t(ParticleMaterial::Effect::Count),
		"kPresetShaders 는 Effect 마다 한 줄 (순서도 같게)");

	GraphicsPSO s_PresetPSO[size_t(ParticleMaterial::Effect::Count)];

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
		s_RootSig[kRootMaterialProgram].InitAsConstantBuffer(3, D3D12_SHADER_VISIBILITY_PIXEL);
		s_RootSig[kRootMaterialInstrs].InitAsBufferSRV(7, D3D12_SHADER_VISIBILITY_PIXEL);

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

		// Preset: 기본 PSO 와 같은 설정에 픽셀 셰이더만 효과별 것으로
		for (size_t i = 0; i < size_t(ParticleMaterial::Effect::Count); ++i)
		{
			ASSERT(size_t(kPresetShaders[i].effect) == i, "kPresetShaders 순서가 Effect 와 다름");
			s_PresetPSO[i] = s_PSO;
			s_PresetPSO[i].SetPixelShader(kPresetShaders[i].ps, kPresetShaders[i].size);
			s_PresetPSO[i].Finalize();
		}

		// b0 = SortKeyConstants, t0 = 렌더 구조체, t1 = 개수 버퍼(DrawArgs), u0 = 정렬 목록
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

void SpriteRenderer::CreateMaterialPSO(GraphicsPSO& pso, const void* ps, size_t size)
{
	InitializePipeline();

	// 루트 시그니처 / 블렌드 / 깊이 / VS 는 기본 PSO 그대로, 픽셀 셰이더만 바꾼다
	pso = s_PSO;
	pso.SetPixelShader(ps, size);
	pso.Finalize();
}

const GraphicsPSO& SpriteRenderer::PresetPSO(ParticleMaterial::Effect effect)
{
	InitializePipeline();
	const size_t i = (effect < ParticleMaterial::Effect::Count) ? size_t(effect) : 0;
	return s_PresetPSO[i];
}

const char* SpriteRenderer::PresetShaderName(ParticleMaterial::Effect effect)
{
	const size_t i = (effect < ParticleMaterial::Effect::Count) ? size_t(effect) : 0;
	return kPresetShaders[i].file;
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
		MakeRenderBinding<float>("SpriteRotation",   Particles::SpriteRotation,  0.f),
		MakeRenderBinding<float3>("Velocity",        Particles::Velocity,        float3{ 0.f, 0.f, 0.f }),
		// 아래 둘은 가변 속성 버퍼에서 읽는다 (EmitterCompiler 가 Trim 하지 않게 여기서 "읽는다" 고 알린다)
		MakeRenderBinding<float3>("SpriteFacing",    Particles::SpriteFacing,    float3{ 0.f, 1.f, 0.f }),
		MakeRenderBinding<float3>("SpriteAlignment", Particles::SpriteAlignment, float3{ 0.f, 1.f, 0.f })
	};

	return desc;
}

RenderParticleData SpriteRenderer::RenderDefaults(const RendererDesc& desc)
{
	RenderParticleData d = RenderParticle::Defaults();
	if (desc.bindings.size() != kNumSlots)
		return d;

	std::memcpy(&d.position, desc.bindings[kPosition].defaultValue.data(), sizeof(float3));
	std::memcpy(&d.color, desc.bindings[kColor].defaultValue.data(), sizeof(float4));
	std::memcpy(&d.size, desc.bindings[kSize].defaultValue.data(), sizeof(float));
	std::memcpy(&d.subImageIndex, desc.bindings[kSubImageIndex].defaultValue.data(), sizeof(float));
	std::memcpy(&d.spriteRotation, desc.bindings[kSpriteRotation].defaultValue.data(), sizeof(float));
	std::memcpy(&d.velocity, desc.bindings[kVelocity].defaultValue.data(), sizeof(float3));
	// spriteSize (-1 = 없음) / normalizedAge (0) 는 RenderParticle::Defaults() 그대로
	return d;
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
	m_ParticleBuffer.Create(L"Sprite Particle Buffer (SoA)", m_BufferBytes / 4, 4);
	m_RenderBuffer.Create(L"Sprite Render Particles", (std::max)(m_Capacity, 1u), RenderParticle::kBytes);

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
	m_RenderBuffer.Destroy();

	m_SortedIndices.clear();

	m_Log.clear();
	m_Ready = false;
}

void SpriteRenderer::Render(GraphicsContext& gfx, const Math::Camera& camera, const EmitterInstance& emitter)
{
	if (!m_Ready || emitter.Count() == 0)
		return;

	// 1) 업로드 버퍼
	UploadCpu(gfx, emitter);

	// 2) 정렬 여부에 따라 정렬 + 업로드 버퍼
	const bool sorted = ShouldSort();
	if (sorted)
		UploadSortedIndices(gfx, camera, emitter);
	else
		gfx.TransitionResource(m_SortedIndexBuffer, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, true);

	// 3) 상수 버퍼 / 파이프라인 / SRV
	BindForDraw(gfx, camera, m_RenderBuffer, m_ParticleBuffer, m_SortedIndexBuffer, sorted, sizeof(uint32_t));

	// 4) 그리기: 파티클 하나 = 사각형(정점 6개), 인스턴스 = 파티클 수
	gfx.DrawInstanced(6, emitter.Count());
}


void SpriteRenderer::UploadCpu(GraphicsContext& gfx, const EmitterInstance& emitter)
{
	// 렌더 구조체: 살아 있는 만큼만
	gfx.TransitionResource(m_RenderBuffer, D3D12_RESOURCE_STATE_COPY_DEST, true);
	gfx.WriteBuffer(m_RenderBuffer, 0, emitter.RenderData(), size_t(emitter.Count()) * RenderParticle::kBytes);
	gfx.TransitionResource(m_RenderBuffer, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, true);

	// 가변 속성 (SoA): 셰이더가 읽는 것은 SpriteFacing / SpriteAlignment 뿐이라, 둘 다 없으면 올리지 않는다
	if (m_FacingOffset != kNoAttribute || m_AlignmentOffset != kNoAttribute)
	{
		gfx.TransitionResource(m_ParticleBuffer, D3D12_RESOURCE_STATE_COPY_DEST, true);
		gfx.WriteBuffer(m_ParticleBuffer, 0, emitter.ParticleData(), m_BufferBytes);
	}
	gfx.TransitionResource(m_ParticleBuffer, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, true);
}

bool SpriteRenderer::ReadLayout(const Block& layout, uint32_t capacity, const RendererDesc& desc)
{
	// Desc() 와 같은 칸 구성인지
	static const TypeInfo* kSlotTypes[kNumSlots] =
	{
		&Types::Float3, &Types::Float4, &Types::Float, &Types::Float,
		&Types::Float, &Types::Float3, &Types::Float3, &Types::Float3
	};
	if (desc.bindings.size() != kNumSlots)
	{
		m_Log = "[SpriteRenderer] 바인딩 칸 수가 다름 (SpriteRenderer::Desc() 로 만든 desc 를 넘겨야 함)\n";
		return false;
	}
	for (uint32_t i = 0; i < kNumSlots; ++i)
	{
		if (desc.bindings[i].type != kSlotTypes[i])
		{
			m_Log = "[SpriteRenderer] '" + desc.bindings[i].slot + "' 칸 타입이 다름\n";
			return false;
		}
	}

	// 렌더 구조체의 값은 위치가 고정이라 찾을 것이 없다.
	// 가변 속성 버퍼에서 읽는 두 벡터만: 레이아웃에 있으면 그 offset, 없으면 (아무도 안 씀) 기본값
	auto find = [&](const char* name) -> uint32_t
		{
			const Slot* s = layout.Find(name);
			return (s && s->type == &Types::Float3) ? s->offset : kNoAttribute;
		};
	m_FacingOffset = find(Particles::SpriteFacing);
	m_AlignmentOffset = find(Particles::SpriteAlignment);

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
	// 렌더 구조체의 기본값은 시뮬레이션이 Spawn 때 넣는다 (RenderDefaults). 여기서는 VS 가 직접 쓰는 두 벡터만
	std::memcpy(&m_DefaultSpriteFacing, desc.bindings[kSpriteFacing].defaultValue.data(), sizeof(float3));
	std::memcpy(&m_DefaultSpriteAlignment, desc.bindings[kSpriteAlignment].defaultValue.data(), sizeof(float3));
}


void SpriteRenderer::RenderGpu(GraphicsContext& gfx, const Math::Camera& camera, GpuEmitterInstance& emitter)
{
	// 기존 코드
	//if (!m_Ready || !emitter.Ready())
	//	return;

	//	gfx.TransitionResource(emitter.RenderBuffer(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
	//	gfx.TransitionResource(emitter.ParticleBuffer(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

	//	// 정렬 (CPU 경로와 같은 조건). 개수를 CPU 로 읽지 않고 DrawArgs 의 alive 를 GPU 에서 바로 쓴다
	//	const bool sorted = ShouldSort();
	//	if (sorted)
	//		SortOnGpu(gfx, camera, emitter);


	//	gfx.TransitionResource(emitter.DrawArgs(), D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
	//	gfx.TransitionResource(m_GpuSortBuffer, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, true);

	//	BindForDraw(gfx, camera, emitter.RenderBuffer(), emitter.ParticleBuffer(), m_GpuSortBuffer, sorted, sizeof(uint32_t) * 2);
	//{

	//	ScopedTimer t(L"Render Sprite (Single)", gfx);
	//	// 개수는 Finalize 가 Draw 인자 (6, alive, 0, 0) 로 써둔 것. CPU 는 개수를 몰라도 된다
	//	gfx.DrawIndirect(emitter.DrawArgs());
	//}

	PrepareGPU(gfx, camera, emitter);
	DrawGPU(gfx, camera, emitter);

}

void SpriteRenderer::PrepareGPU(GraphicsContext& gfx, const Math::Camera& camera, GpuEmitterInstance& emitter)
{
	if (!m_Ready || !emitter.Ready())
		return;

	gfx.TransitionResource(emitter.RenderBuffer(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
	gfx.TransitionResource(emitter.ParticleBuffer(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

	if (ShouldSort())
		SortOnGpu(gfx, camera, emitter);

	gfx.TransitionResource(emitter.DrawArgs(), D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
	gfx.TransitionResource(m_GpuSortBuffer, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
}

void SpriteRenderer::DrawGPU(GraphicsContext& gfx, const Math::Camera& camera, GpuEmitterInstance& emitter)
{
	if (!m_Ready || !emitter.Ready())
		return;

	BindForDraw(gfx, camera, emitter.RenderBuffer(), emitter.ParticleBuffer(), m_GpuSortBuffer, ShouldSort(), sizeof(uint32_t) * 2);
	
	
	gfx.DrawIndirect(emitter.DrawArgs());
}

void SpriteRenderer::SortOnGpu(GraphicsContext& gfx, const Math::Camera& camera, GpuEmitterInstance& emitter)
{
	ComputeContext& cc = gfx.GetComputeContext();
	IndirectArgsBuffer& countBuffer = emitter.DrawArgs();   // [4] = alive (Finalize 가 씀)

	// 1) 키 생성: 파티클 i -> uint2(i, 깊이 키). alive 이후 칸은 셰이더가 건너뛴다
	SortKeyConstants kc = {};
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
	const D3D12_CPU_DESCRIPTOR_HANDLE srvs[2] = { emitter.RenderBuffer().GetSRV(), countBuffer.GetSRV() };
	cc.SetDynamicDescriptors(kSortKeySRVs, 0, 2, srvs);
	cc.SetDynamicDescriptor(kSortKeyUAV, 0, m_GpuSortBuffer.GetUAV());

	// 개수를 CPU 가 모르므로 capacity 기준으로 띄우고, alive 를 넘는 스레드는 바로 끝난다
	{
		ScopedTimer t(L"Make SortKey (Single)", gfx);
		cc.Dispatch((m_Capacity + kSortKeyGroupSize - 1) / kSortKeyGroupSize, 1, 1);
	}

	// 2) 정렬: 키 내림차순 = 카메라에서 먼 것부터 (CPU SortBackToFront 와 같은 순서)
	//    BitonicSort 가 키 목록 UAV 배리어 / 개수 버퍼 SRV 전환을 직접 한다
	{
		ScopedTimer t(L"Sort Particle (Single)", gfx);
		BitonicSort::Sort(cc, m_GpuSortBuffer, countBuffer, kDrawArgsInstanceCountOffset, false, false);
	}
}

void SpriteRenderer::SubmitToList(GraphicsContext& gfx, ParticleRenderList& list, const EmitterInstance& emitter)
{
	if (!m_Ready || emitter.Count() == 0)
		return;

	// CPU 시뮬레이션 결과를 올린다 (Render 의 1 단계와 같음). 개수는 CPU 가 알고 있으므로 상수로 넘긴다
	UploadCpu(gfx, emitter);

	ParticleRenderList::Source src = MakeListSource();
	src.render = &m_RenderBuffer;
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
	src.render = &emitter.RenderBuffer();
	src.particles = &emitter.ParticleBuffer();
	src.countBuffer = &emitter.DrawArgs();
	src.countOffset = kDrawArgsInstanceCountOffset;
	list.Add(gfx, src);
}

ParticleRenderList::Source SpriteRenderer::MakeListSource() const
{
	ParticleRenderList::Source src;
	src.capacity = m_Capacity;
	src.spriteFacingOffset = m_FacingOffset;
	src.spriteAlignmentOffset = m_AlignmentOffset;
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

	if (m_Material)
	{
		src.program = &m_Material->Program();
		src.materialParams = m_Material->Params();
	}
	for (uint32_t k = 0; k < ParticleMaterial::kNumEffectTextures; ++k)
		src.effectTextures[k] = TextureSRV(ParticleMaterial::kTexture0 + k);

	return src;
}

void SpriteRenderer::BindForDraw(GraphicsContext& gfx, const Math::Camera& camera, StructuredBuffer& render, ByteAddressBuffer& particles,
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
	sc.sortEnabled = sorted ? 1u : 0u;
	sc.sortStride = sortStride;
	sc.subUVBlend = m_Settings.subUVBlend ? 1u : 0u;
	sc.subImageSize[0] = static_cast<float>(m_Settings.subImageCols);
	sc.subImageSize[1] = static_cast<float>(m_Settings.subImageRows);

	sc.facingMode = static_cast<uint32_t>(m_Settings.facing);
	sc.alignmentMode = static_cast<uint32_t>(m_Settings.alignment);
	sc.pivot = m_Settings.pivot;
	sc.cameraOffset = m_Settings.cameraOffset;
	sc.velocityStretch = m_Settings.velocityStretch;
	sc.blendNear = m_Settings.blendNear;
	sc.blendFar = m_Settings.blendFar;

	// 가변 속성 버퍼에서 읽는 것 (Custom Facing / Custom Alignment)
	sc.capacity = m_Capacity;
	sc.spriteFacingOffset = m_FacingOffset;
	sc.spriteAlignmentOffset = m_AlignmentOffset;
	sc.defaultSpriteFacing = m_DefaultSpriteFacing;
	sc.defaultSpriteAlignment = m_DefaultSpriteAlignment;

	gfx.SetRootSignature(s_RootSig);
	gfx.SetPipelineState(m_MaterialPSO ? *m_MaterialPSO : s_PSO);
	gfx.SetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	gfx.SetDynamicConstantBufferView(kRootCBV, sizeof(sc), &sc);

	const D3D12_CPU_DESCRIPTOR_HANDLE srvs[kNumSRVs] = { render.GetSRV(), sortedIndices.GetSRV(), particles.GetSRV() };
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
		alignas(16) float4 defaults[ParticleMaterial::kMaxParams];
		for (uint32_t k = 0; k < ParticleMaterial::kMaxParams; ++k)
			defaults[k] = ParticleMaterial::DefaultParam(k);
		gfx.SetDynamicConstantBufferView(kRootMaterialCB, sizeof(defaults), defaults);
	}

	{
		alignas(16) MaterialProgramConstants mp = {};	// SetDynamicConstantBufferView 는 16 B 정렬된 주소만 받는다

		const void* instrData = &kEmptyInstr;
		size_t      instrBytes = sizeof(MaterialInstr);
		// Preset / 생성된 PS 는 명령어를 읽지 않는다 (b3 / t7 은 빈 값만 묶어 둔다)
		if (m_Material && !m_MaterialPSO)
		{
			const MaterialProgram& program = m_Material->Program();
			const uint32_t count = (std::min)(uint32_t(program.instrs.size()), uint32_t(MaterialProgram::kMaxInstrs));
			
			mp.useVM = m_Material->NeedsVM() ? 1u : 0u;
			mp.instrCount = count;
			mp.outputReg = program.outputReg;

			if (count > 0)
			{
				instrData = program.instrs.data();
				instrBytes = sizeof(MaterialInstr) * count;
			}
		}
		gfx.SetDynamicConstantBufferView(kRootMaterialProgram, sizeof(mp), &mp);
		gfx.SetDynamicSRV(kRootMaterialInstrs, instrBytes, instrData);
	}

}

void SpriteRenderer::UploadSortedIndices(GraphicsContext& gfx, const Math::Camera& camera, const EmitterInstance& emitter)
{
	uint32_t count = emitter.Count();

	// 위치는 렌더 구조체의 첫 필드: 구조체 크기만큼 건너뛰며 읽는다
	const float3* pos = &emitter.RenderData()->position;

	SortBackToFront(pos, count,
		ToFloat3(camera.GetPosition()), ToFloat3(camera.GetForwardVec()),
		m_SortedIndices, m_SortScratch, RenderParticle::kBytes);

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
