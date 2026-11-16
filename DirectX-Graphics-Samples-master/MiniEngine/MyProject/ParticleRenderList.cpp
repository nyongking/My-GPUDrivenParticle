#include "pch.h"
#include "ParticleRenderList.h"

#include "BitonicSort.h"
#include "BufferManager.h"
#include "Camera.h"
#include "CommandContext.h"
#include "GraphicsCommon.h"
#include "PipelineState.h"
#include "RootSignature.h"

#include "CompiledShaders/ParticleGatherCS.h"
#include "CompiledShaders/ParticleListVS.h"
#include "CompiledShaders/ParticleListPS.h"

#include <DirectXMath.h>

using namespace Graphics;

namespace
{
	// ParticleGatherCS.hlsl 의 cbuffer GatherConstants 와 같은 배치
	struct alignas(16) GatherConstants
	{
		uint32_t positionOffset;   // 0
		uint32_t colorOffset;      // 4
		uint32_t sizeOffset;       // 8
		uint32_t subImageOffset;   // 12
		uint32_t capacity;         // 16
		uint32_t countOffset;      // 20
		uint32_t constCount;       // 24
		uint32_t useCountBuffer;   // 28
		uint32_t emitterIndex;     // 32
		uint32_t listCapacity;     // 36
		uint32_t spriteSizeOffset;      // 40
		uint32_t spriteRotationOffset;  // 44
		float4   defaultColor;     // 48
		float3   defaultPosition;  // 64
		float    defaultSize;      // 76
		float    camPos[3];        // 80
		float    defaultSubImage;  // 92
		float    camForward[3];    // 96
		float    defaultSpriteRotation; // 108
		float    camRight[3];      // 112
		uint32_t velocityOffset;   // 124
		float    camUp[3];         // 128
		uint32_t spriteFacingOffset;    // 140
		float3   defaultVelocity;       // 144
		uint32_t spriteAlignmentOffset; // 156
		float3   defaultSpriteFacing;   // 160
		uint32_t facingMode;            // 172
		float3   defaultSpriteAlignment;// 176
		uint32_t alignmentMode;         // 188
		float2   pivot;            // 192
		float    cameraOffset;     // 200
		float    velocityStretch;  // 204
		float    blendNear;        // 208
		float    blendFar;         // 212
		uint32_t cullEnable;       // 216
		float    _pad0;            // 220
		float4   frustumPlanes[6]; // 224  월드 공간, 안쪽이 +
	};
	static_assert(sizeof(GatherConstants) == 320, "GatherConstants 320 B");
	static_assert(offsetof(GatherConstants, defaultColor) == 48, "DefaultColor offset 48");
	static_assert(offsetof(GatherConstants, camPos) == 80, "CamPos offset 80");
	static_assert(offsetof(GatherConstants, camForward) == 96, "CamForward offset 96");
	static_assert(offsetof(GatherConstants, camRight) == 112, "CamRight offset 112");
	static_assert(offsetof(GatherConstants, camUp) == 128, "CamUp offset 128");
	static_assert(offsetof(GatherConstants, defaultVelocity) == 144, "DefaultVelocity offset 144");
	static_assert(offsetof(GatherConstants, defaultSpriteFacing) == 160, "DefaultSpriteFacing offset 160");
	static_assert(offsetof(GatherConstants, defaultSpriteAlignment) == 176, "DefaultSpriteAlignment offset 176");
	static_assert(offsetof(GatherConstants, pivot) == 192, "Pivot offset 192");
	static_assert(offsetof(GatherConstants, cullEnable) == 216, "CullEnable offset 216");
	static_assert(offsetof(GatherConstants, frustumPlanes) == 224, "FrustumPlanes offset 224");

	// ParticleListVS.hlsl 의 cbuffer ListDrawConstants 와 같은 배치 (빌보드 축은 Gather 가 계산)
	struct alignas(16) DrawConstants
	{
		DirectX::XMFLOAT4X4 viewProj;   // 0
	};
	static_assert(sizeof(DrawConstants) == 64, "DrawConstants 64 B");

	enum GatherRoot { kGatherCB, kGatherSRVs, kGatherUAVs, kNumGatherRoot };
	enum DrawRoot { kDrawCB, kDrawSRVs, kDrawEmitters, kDrawTextures, kNumDrawRoot };

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

		// b0 = GatherConstants, t0 = 이미터 파티클, t1 = 개수 버퍼, u0 = RenderParticles, u1 = SortList, u2 = Counter
		s_GatherRootSig.Reset(kNumGatherRoot, 0);
		s_GatherRootSig[kGatherCB].InitAsConstantBuffer(0);
		s_GatherRootSig[kGatherSRVs].InitAsDescriptorRange(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 0, 2);
		s_GatherRootSig[kGatherUAVs].InitAsDescriptorRange(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 0, 3);
		s_GatherRootSig.Finalize(L"Particle Gather RootSig");

		s_GatherPSO.SetRootSignature(s_GatherRootSig);
		s_GatherPSO.SetComputeShader(g_pParticleGatherCS, sizeof(g_pParticleGatherCS));
		s_GatherPSO.Finalize();

		// VS: b0 = DrawConstants, t0 = RenderParticles, t1 = SortList, t2 = EmitterInfo (루트 SRV)
		// PS: t0~t15 (space1) = 텍스처 표
		s_DrawRootSig.Reset(kNumDrawRoot, 1);
		s_DrawRootSig[kDrawCB].InitAsConstantBuffer(0, D3D12_SHADER_VISIBILITY_VERTEX);
		s_DrawRootSig[kDrawSRVs].InitAsDescriptorRange(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 0, 2, D3D12_SHADER_VISIBILITY_VERTEX);
		s_DrawRootSig[kDrawEmitters].InitAsBufferSRV(2, D3D12_SHADER_VISIBILITY_VERTEX);
		s_DrawRootSig[kDrawTextures].InitAsDescriptorRange(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 0, ParticleRenderList::kMaxTextures,
			D3D12_SHADER_VISIBILITY_PIXEL, 1);
		s_DrawRootSig.InitStaticSampler(0, SamplerLinearClampDesc, D3D12_SHADER_VISIBILITY_PIXEL);
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
}

void ParticleRenderList::Begin(GraphicsContext& gfx, const Math::Camera& camera)
{
	m_Active = (m_Capacity > 0);
	m_Emitters.clear();
	m_Textures.clear();
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
	if (!m_Active || src.particles == nullptr || src.capacity == 0 || src.texture.ptr == 0)
		return;
	if (src.countBuffer == nullptr && src.constCount == 0)
		return;

	const uint32_t emitterIndex = uint32_t(m_Emitters.size());

	EmitterInfo info = {};
	info.subImageCols = (std::max)(src.subImageCols, 1u);
	info.subImageRows = (std::max)(src.subImageRows, 1u);
	info.subUVBlend = src.subUVBlend ? 1u : 0u;
	info.blendMode = src.blendMode;
	info.textureIndex = TextureIndex(src.texture);
	m_Emitters.push_back(info);

	GatherConstants gc = {};
	gc.positionOffset = src.positionOffset;
	gc.colorOffset = src.colorOffset;
	gc.sizeOffset = src.sizeOffset;
	gc.subImageOffset = src.subImageOffset;
	gc.capacity = src.capacity;
	gc.countOffset = src.countOffset;
	gc.constCount = src.constCount;
	gc.useCountBuffer = src.countBuffer ? 1u : 0u;
	gc.emitterIndex = emitterIndex;
	gc.listCapacity = m_Capacity;
	gc.defaultColor = src.defaultColor;
	gc.defaultPosition = src.defaultPosition;
	gc.defaultSize = src.defaultSize;
	gc.defaultSubImage = src.defaultSubImage;
	gc.spriteSizeOffset = src.spriteSizeOffset;
	gc.spriteRotationOffset = src.spriteRotationOffset;
	gc.defaultSpriteRotation = src.defaultSpriteRotation;
	gc.velocityOffset = src.velocityOffset;
	gc.spriteFacingOffset = src.spriteFacingOffset;
	gc.spriteAlignmentOffset = src.spriteAlignmentOffset;
	gc.defaultVelocity = src.defaultVelocity;
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
	cc.TransitionResource(*src.particles, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
	if (src.countBuffer)
		cc.TransitionResource(*src.countBuffer, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

	cc.SetRootSignature(s_GatherRootSig);
	cc.SetPipelineState(s_GatherPSO);
	cc.SetDynamicConstantBufferView(kGatherCB, sizeof(gc), &gc);

	// t1 은 개수 버퍼가 없을 때(CPU) 읽지 않지만 자리를 채워야 한다
	ByteAddressBuffer& countSrv = src.countBuffer ? *src.countBuffer : *src.particles;
	const D3D12_CPU_DESCRIPTOR_HANDLE srvs[2] = { src.particles->GetSRV(), countSrv.GetSRV() };
	const D3D12_CPU_DESCRIPTOR_HANDLE uavs[3] = { m_RenderParticles.GetUAV(), m_SortList.GetUAV(), m_Counter.GetUAV() };
	cc.SetDynamicDescriptors(kGatherSRVs, 0, 2, srvs);
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

	gfx.SetRootSignature(s_DrawRootSig);
	gfx.SetPipelineState(s_DrawPSO);
	gfx.SetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	gfx.SetDynamicConstantBufferView(kDrawCB, sizeof(dc), &dc);

	const D3D12_CPU_DESCRIPTOR_HANDLE srvs[2] = { m_RenderParticles.GetSRV(), m_SortList.GetSRV() };
	gfx.SetDynamicDescriptors(kDrawSRVs, 0, 2, srvs);
	gfx.SetDynamicSRV(kDrawEmitters, m_Emitters.size() * sizeof(EmitterInfo), m_Emitters.data());

	// 텍스처 표: 빈 칸은 0 번으로 채운다 (테이블 전체가 유효해야 함)
	D3D12_CPU_DESCRIPTOR_HANDLE textures[kMaxTextures];
	for (uint32_t i = 0; i < kMaxTextures; ++i)
		textures[i] = (i < m_Textures.size()) ? m_Textures[i] : m_Textures[0];
	gfx.SetDynamicDescriptors(kDrawTextures, 0, kMaxTextures, textures);

	gfx.DrawIndirect(m_DrawArgs);
}
