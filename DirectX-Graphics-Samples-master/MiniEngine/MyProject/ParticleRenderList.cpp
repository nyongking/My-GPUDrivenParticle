#include "pch.h"
#include "ParticleRenderList.h"

#include "BitonicSort.h"
#include "BufferManager.h"
#include "Camera.h"
#include "CommandContext.h"
#include "GraphicsCommon.h"
#include "PipelineState.h"
#include "RootSignature.h"
#include "GlobalData.h"

#include "CompiledShaders/ParticleGatherCS.h"
#include "CompiledShaders/ParticleListVS.h"
#include "CompiledShaders/ParticleListPS.h"

#include <DirectXMath.h>

using namespace Graphics;

namespace
{
	// ParticleGatherCS.hlsl 의 cbuffer GatherConstants 와 같은 배치
	//   파티클 값은 고정 렌더 구조체에서 읽으므로 속성 offset / 기본값이 없다 (Facing / Alignment 벡터만 예외)
	struct alignas(16) GatherConstants
	{
		uint32_t capacity;         // 0
		uint32_t countOffset;      // 4
		uint32_t constCount;       // 8
		uint32_t useCountBuffer;   // 12
		uint32_t emitterIndex;     // 16
		uint32_t listCapacity;     // 20
		uint32_t spriteFacingOffset;    // 24
		uint32_t spriteAlignmentOffset; // 28
		float    camPos[3];        // 32
		uint32_t facingMode;       // 44
		float    camForward[3];    // 48
		uint32_t alignmentMode;    // 60
		float    camRight[3];      // 64
		float    cameraOffset;     // 76
		float    camUp[3];         // 80
		float    velocityStretch;  // 92
		float3   defaultSpriteFacing;    // 96
		float    blendNear;        // 108
		float3   defaultSpriteAlignment; // 112
		float    blendFar;         // 124
		float2   pivot;            // 128
		uint32_t cullEnable;       // 136
		uint32_t _pad0;            // 140
		float4   frustumPlanes[6]; // 144  월드 공간, 안쪽이 +
	};
	static_assert(sizeof(GatherConstants) == 240, "GatherConstants 240 B");
	
	// ParticleListVS.hlsl 의 cbuffer ListDrawConstants 와 같은 배치 (빌보드 축은 Gather 가 계산)
	struct alignas(16) DrawConstants
	{
		DirectX::XMFLOAT4X4 viewProj;   // 0
		float               time;
		float               _pad[3];
	};
	static_assert(sizeof(DrawConstants) == 80, "DrawConstants 80 B");

	enum GatherRoot { kGatherCB, kGatherSRVs, kGatherUAVs, kNumGatherRoot };
	enum DrawRoot { kDrawCB, kDrawSRVs, kDrawEmitters, kDrawMaterialParams, kDrawInstrs, kDrawTextures, kNumDrawRoot };

	const uint32_t kGatherGroupSize = 256;            // ParticleGatherCS 의 numthreads

	bool          s_PipelineReady = false;
	RootSignature s_GatherRootSig;
	ComputePSO    s_GatherPSO(L"Particle Gather CS");
	RootSignature s_DrawRootSig;
	GraphicsPSO   s_DrawPSO(L"Particle List PSO");

	void InitializePipeline()
	{
		if (s_PipelineReady)
			return;

		// b0 = GatherConstants, t0 = 이미터의 렌더 구조체, t1 = 가변 속성(SoA), t2 = 개수 버퍼
		// u0 = RenderParticles, u1 = SortList, u2 = Counter
		s_GatherRootSig.Reset(kNumGatherRoot, 0);
		s_GatherRootSig[kGatherCB].InitAsConstantBuffer(0);
		s_GatherRootSig[kGatherSRVs].InitAsDescriptorRange(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 0, 3);
		s_GatherRootSig[kGatherUAVs].InitAsDescriptorRange(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 0, 3);
		s_GatherRootSig.Finalize(L"Particle Gather RootSig");

		s_GatherPSO.SetRootSignature(s_GatherRootSig);
		s_GatherPSO.SetComputeShader(g_pParticleGatherCS, sizeof(g_pParticleGatherCS));
		s_GatherPSO.Finalize();

		// VS: b0 = DrawConstants, t0 = RenderParticles, t1 = SortList, t2 = EmitterInfo (루트 SRV)
		// PS: t0~t15 (space1) = 텍스처 표
		s_DrawRootSig.Reset(kNumDrawRoot, 2);
		s_DrawRootSig[kDrawCB].InitAsConstantBuffer(0, D3D12_SHADER_VISIBILITY_ALL);
		s_DrawRootSig[kDrawSRVs].InitAsDescriptorRange(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 0, 2, D3D12_SHADER_VISIBILITY_VERTEX);
		s_DrawRootSig[kDrawEmitters].InitAsBufferSRV(2, D3D12_SHADER_VISIBILITY_ALL);
		s_DrawRootSig[kDrawMaterialParams].InitAsBufferSRV(3, D3D12_SHADER_VISIBILITY_PIXEL);
		s_DrawRootSig[kDrawInstrs].InitAsBufferSRV(4, D3D12_SHADER_VISIBILITY_PIXEL);
		s_DrawRootSig[kDrawTextures].InitAsDescriptorRange(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 0, ParticleRenderList::kMaxTextures,
			D3D12_SHADER_VISIBILITY_PIXEL, 1);

		// SpriteRenderer 와 같은 샘플러 (밉 1 까지만)
		SamplerDesc particleSampler = SamplerLinearClampDesc;
		particleSampler.MaxLOD = 1.f;
		s_DrawRootSig.InitStaticSampler(0, particleSampler, D3D12_SHADER_VISIBILITY_PIXEL);
		s_DrawRootSig.InitStaticSampler(1, SamplerLinearWrapDesc, D3D12_SHADER_VISIBILITY_PIXEL);

		s_DrawRootSig.Finalize(L"Particle List RootSig");

		// SpriteRenderer 의 PSO 와 같은 상태 (premultiplied: Additive 는 PS 가 alpha 0 을 내보낸다)
		s_DrawPSO.SetRootSignature(s_DrawRootSig);
		s_DrawPSO.SetRasterizerState(RasterizerTwoSided);
		s_DrawPSO.SetBlendState(BlendPreMultiplied);
		s_DrawPSO.SetDepthStencilState(DepthStateReadOnly);
		s_DrawPSO.SetRenderTargetFormat(g_SceneColorBuffer.GetFormat(), g_SceneDepthBuffer.GetFormat());
		s_DrawPSO.SetPrimitiveTopologyType(D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE);
		s_DrawPSO.SetVertexShader(g_pParticleListVS, sizeof(g_pParticleListVS));
		s_DrawPSO.SetPixelShader(g_pParticleListPS, sizeof(g_pParticleListPS));
		s_DrawPSO.Finalize();

		s_PipelineReady = true;
	}
}

bool ParticleRenderList::Reserve(uint32_t capacity)
{
	InitializePipeline();

	capacity = (std::max)(capacity, 1u);
	if (capacity == m_Capacity)
		return true;

	m_RenderParticles.Create(L"Particle List: RenderParticles", capacity * (kRenderParticleBytes / 4), 4);
	m_SortList.Create(L"Particle List: Sort Key-Index", capacity, sizeof(uint32_t) * 2);

	alignas(16) const uint32_t zeros[4] = { 0, 0, 0, 0 };
	m_Counter.Create(L"Particle List: Counter", 4, sizeof(uint32_t), zeros);

	// InstanceCount (offset 4) 만 매 프레임 카운터에서 복사한다
	alignas(16) const uint32_t drawArgs[4] = { 6, 0, 0, 0 };
	m_DrawArgs.Create(L"Particle List: Draw Args", 4, sizeof(uint32_t), drawArgs);

	m_Capacity = capacity;
	return true;
}

void ParticleRenderList::Shutdown()
{
	m_RenderParticles.Destroy();
	m_SortList.Destroy();
	m_Counter.Destroy();
	m_DrawArgs.Destroy();
	m_Capacity = 0;
	m_Active = false;
	m_Emitters.clear();
	m_Textures.clear();
	m_MaterialParams.clear();
	m_Instrs.clear();
}

void ParticleRenderList::Begin(GraphicsContext& gfx, const Math::Camera& camera)
{
	m_Active = (m_Capacity > 0);
	m_Emitters.clear();
	m_Textures.clear();
	m_MaterialParams.clear();
	m_Instrs.clear();
	if (!m_Active)
		return;

	const Math::Vector3 pos = camera.GetPosition();
	const Math::Vector3 fwd = camera.GetForwardVec();
	m_CamPos[0] = pos.GetX(); m_CamPos[1] = pos.GetY(); m_CamPos[2] = pos.GetZ();
	m_CamForward[0] = fwd.GetX(); m_CamForward[1] = fwd.GetY(); m_CamForward[2] = fwd.GetZ();
	DirectX::XMStoreFloat4x4(&m_ViewProj, camera.GetViewProjMatrix());
	DirectX::XMStoreFloat4(&m_CamRight, camera.GetRightVec());
	DirectX::XMStoreFloat4(&m_CamUp, camera.GetUpVec());
	m_CamRight.w = 0.f;
	m_CamUp.w = 0.f;

	// 컬링용 월드 프러스텀 평면. Math::Frustum 이 reversed-Z 를 처리하고, 법선은 안쪽 + 정규화 (거리 = m)
	const Math::Frustum& frustum = camera.GetWorldSpaceFrustum();
	for (int k = 0; k < 6; ++k)
		DirectX::XMStoreFloat4(&m_FrustumPlanes[k], Math::Vector4(frustum.GetFrustumPlane(Math::Frustum::PlaneID(k))));

	// 카운터 0 (이전 프레임의 Draw / Sort 가 읽은 뒤에 같은 커맨드 리스트 순서로 실행된다)
	alignas(16) const uint32_t zeros[4] = { 0, 0, 0, 0 };
	gfx.TransitionResource(m_Counter, D3D12_RESOURCE_STATE_COPY_DEST, true);
	gfx.WriteBuffer(m_Counter, 0, zeros, sizeof(zeros));
	gfx.TransitionResource(m_Counter, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
	gfx.TransitionResource(m_RenderParticles, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
	gfx.TransitionResource(m_SortList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, true);
}

uint32_t ParticleRenderList::TextureIndex(D3D12_CPU_DESCRIPTOR_HANDLE srv)
{
	for (uint32_t i = 0; i < uint32_t(m_Textures.size()); ++i)
		if (m_Textures[i].ptr == srv.ptr)
			return i;

	if (m_Textures.size() >= kMaxTextures)
	{
		if (!m_TextureOverflowLogged)
		{
			Utility::Printf("[ParticleRenderList] 텍스처가 %u 개를 넘음 -> 넘는 이미터는 0 번 텍스처로 그림\n", kMaxTextures);
			m_TextureOverflowLogged = true;
		}
		return 0;
	}
	m_Textures.push_back(srv);
	return uint32_t(m_Textures.size() - 1);
}

void ParticleRenderList::Add(GraphicsContext& gfx, const Source& src)
{
	if (!m_Active || src.render == nullptr || src.particles == nullptr || src.capacity == 0 || src.texture.ptr == 0)
		return;
	if (src.countBuffer == nullptr && src.constCount == 0)
		return;

	const uint32_t emitterIndex = uint32_t(m_Emitters.size());

	EmitterInfo info = {};
	info.subImageCols = (std::max)(src.subImageCols, 1u);
	info.subImageRows = (std::max)(src.subImageRows, 1u);
	info.subUVBlend = src.subUVBlend ? 1u : 0u;
	info.blendMode = src.blendMode;
	info.textureAlpha = src.textureAlpha;
	info.textureIndex = TextureIndex(src.texture);

	info.paramBase = uint32_t(m_MaterialParams.size());
	for (uint32_t k = 0; k < ParticleMaterial::kMaxParams; ++k)
		m_MaterialParams.push_back(src.materialParams ? src.materialParams[k] : ParticleMaterial::DefaultParam(k));

	info.instrBase = uint32_t(m_Instrs.size());
	info.instrCount = 0;
	info.outputReg = 0;
	if (src.program)
	{
		info.instrCount = (std::min)(uint32_t(src.program->instrs.size()), uint32_t(MaterialProgram::kMaxInstrs));
		info.outputReg = src.program->outputReg;
		m_Instrs.insert(m_Instrs.end(), src.program->instrs.begin(), src.program->instrs.begin() + info.instrCount);
	}

	for (uint32_t k = 0; k < ParticleMaterial::kNumEffectTextures; ++k)
		info.effectTextures[k] = TextureIndex(src.effectTextures[k].ptr != 0 ? src.effectTextures[k] : src.texture);

	m_Emitters.push_back(info);

	// 이미터마다 다른 것은 개수 / 빌보드 설정뿐이다 (파티클 값의 배치는 모든 이미터가 같다)
	GatherConstants gc = {};
	gc.capacity = src.capacity;
	gc.countOffset = src.countOffset;
	gc.constCount = src.constCount;
	gc.useCountBuffer = src.countBuffer ? 1u : 0u;
	gc.emitterIndex = emitterIndex;
	gc.listCapacity = m_Capacity;
	gc.spriteFacingOffset = src.spriteFacingOffset;
	gc.spriteAlignmentOffset = src.spriteAlignmentOffset;
	gc.defaultSpriteFacing = src.defaultSpriteFacing;
	gc.defaultSpriteAlignment = src.defaultSpriteAlignment;
	gc.facingMode = src.facingMode;
	gc.alignmentMode = src.alignmentMode;
	gc.pivot = src.pivot;
	gc.cameraOffset = src.cameraOffset;
	gc.velocityStretch = src.velocityStretch;
	gc.blendNear = src.blendNear;
	gc.blendFar = src.blendFar;
	gc.cullEnable = m_Culling ? 1u : 0u;
	for (int k = 0; k < 6; ++k)
		gc.frustumPlanes[k] = { m_FrustumPlanes[k].x, m_FrustumPlanes[k].y, m_FrustumPlanes[k].z, m_FrustumPlanes[k].w };
	const float camRight[3] = { m_CamRight.x, m_CamRight.y, m_CamRight.z };
	const float camUp[3] = { m_CamUp.x, m_CamUp.y, m_CamUp.z };
	for (int k = 0; k < 3; ++k)
	{
		gc.camPos[k] = m_CamPos[k];
		gc.camForward[k] = m_CamForward[k];
		gc.camRight[k] = camRight[k];
		gc.camUp[k] = camUp[k];
	}

	ComputeContext& cc = gfx.GetComputeContext();
	cc.TransitionResource(*src.render, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
	cc.TransitionResource(*src.particles, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
	if (src.countBuffer)
		cc.TransitionResource(*src.countBuffer, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

	cc.SetRootSignature(s_GatherRootSig);
	cc.SetPipelineState(s_GatherPSO);
	cc.SetDynamicConstantBufferView(kGatherCB, sizeof(gc), &gc);

	// t2 는 개수 버퍼가 없을 때(CPU) 읽지 않지만 자리를 채워야 한다
	ByteAddressBuffer& countSrv = src.countBuffer ? *src.countBuffer : *src.particles;
	const D3D12_CPU_DESCRIPTOR_HANDLE srvs[3] = { src.render->GetSRV(), src.particles->GetSRV(), countSrv.GetSRV() };
	const D3D12_CPU_DESCRIPTOR_HANDLE uavs[3] = { m_RenderParticles.GetUAV(), m_SortList.GetUAV(), m_Counter.GetUAV() };
	cc.SetDynamicDescriptors(kGatherSRVs, 0, 3, srvs);
	cc.SetDynamicDescriptors(kGatherUAVs, 0, 3, uavs);

	// 개수를 모르는 GPU 이미터도 있어서 capacity 기준으로 띄운다 (넘는 스레드 / 컬링된 파티클은 append 하지 않음)
	cc.Dispatch((src.capacity + kGatherGroupSize - 1) / kGatherGroupSize, 1, 1);
	// 이미터끼리는 카운터 원자 연산으로 자리만 나눠 쓰므로 UAV 배리어가 필요 없다 (정렬 전에 한 번)
}

void ParticleRenderList::Render(GraphicsContext& gfx, bool sorted)
{
	if (!m_Active)
		return;
	m_Active = false;
	if (m_Emitters.empty())
		return;

	ComputeContext& cc = gfx.GetComputeContext();
	cc.InsertUAVBarrier(m_RenderParticles);
	cc.InsertUAVBarrier(m_SortList);
	cc.InsertUAVBarrier(m_Counter, true);

	// 1) 전체 정렬: 키 내림차순 = 카메라에서 먼 것부터. 개수 = 카운터 [0]
	//    정렬 안 함: Gather 가 SortList[slot] = (slot, 키) 로 썼으므로 그대로 append 순서
	if (sorted)
		BitonicSort::Sort(cc, m_SortList, m_Counter, 0, false, false);

	// 2) Draw 인자의 InstanceCount = 카운터
	gfx.TransitionResource(m_Counter, D3D12_RESOURCE_STATE_COPY_SOURCE);
	gfx.TransitionResource(m_DrawArgs, D3D12_RESOURCE_STATE_COPY_DEST, true);
	gfx.CopyBufferRegion(m_DrawArgs, 4, m_Counter, 0, 4);

	gfx.TransitionResource(m_DrawArgs, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
	gfx.TransitionResource(m_RenderParticles, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
	gfx.TransitionResource(m_SortList, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, true);

	// 3) 그리기
	DrawConstants dc = {};
	dc.viewProj = m_ViewProj;
	dc.time = sGlobal.exeTime;

	gfx.SetRootSignature(s_DrawRootSig);
	gfx.SetPipelineState(s_DrawPSO);
	gfx.SetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	gfx.SetDynamicConstantBufferView(kDrawCB, sizeof(dc), &dc);

	const D3D12_CPU_DESCRIPTOR_HANDLE srvs[2] = { m_RenderParticles.GetSRV(), m_SortList.GetSRV() };
	gfx.SetDynamicDescriptors(kDrawSRVs, 0, 2, srvs);
	gfx.SetDynamicSRV(kDrawEmitters, m_Emitters.size() * sizeof(EmitterInfo), m_Emitters.data());

	if (m_Instrs.empty())
		m_Instrs.push_back(MaterialInstr{});
	gfx.SetDynamicSRV(kDrawMaterialParams, m_MaterialParams.size() * sizeof(float4), m_MaterialParams.data());
	gfx.SetDynamicSRV(kDrawInstrs, m_Instrs.size() * sizeof(MaterialInstr), m_Instrs.data());

	// 텍스처 표: 빈 칸은 0 번으로 채운다 (테이블 전체가 유효해야 함)
	D3D12_CPU_DESCRIPTOR_HANDLE textures[kMaxTextures];
	for (uint32_t i = 0; i < kMaxTextures; ++i)
		textures[i] = (i < m_Textures.size()) ? m_Textures[i] : m_Textures[0];
	static_assert(kMaxTextures % 16 == 0, "텍스처 표는 16 칸 단위"); // 16칸 단위를 2번으로 나누어서
	for (uint32_t i = 0; i < kMaxTextures; i += 16)
		gfx.SetDynamicDescriptors(kDrawTextures, i, 16, textures + i);

	gfx.DrawIndirect(m_DrawArgs);
}
