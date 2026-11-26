#pragma once
// 여러 이미터의 파티클을 한 목록에 모아서 이미터 구분 없이 깊이 정렬 후 한 번에 그린다
//
//   Begin()                     : 카운터 0, 카메라 저장
//   Add() x 이미터 수            : ParticleGatherCS - 살아 있는 파티클 -> RenderParticles 에 append (ListParticle)
//                                 + SortList 에 uint2(목록 번호, 깊이 키)
//   Render()                    : BitonicSort (전체 목록, 내림차순 = 먼 것부터)
//                                 -> DrawArgs.InstanceCount = 카운터 -> DrawIndirect 1번 (ParticleListVS/PS)
//   Render(gfx, false)          : 정렬 없이 append 순서로 그림 (Additive 처럼 순서가 결과에 영향 없을 때)
//
//   SetCulling(true) 면 Gather 가 프러스텀 밖 / 투명 / 크기 0 인 파티클을 목록에 넣지 않는다
//
//   입력은 이미터의 고정 렌더 구조체 (RenderParticle.h). 모든 이미터가 같은 형식이라 속성 offset 이 필요 없다.
//   Gather 가 빌보드 축까지 계산해서 목록용 형식 (HLSL ListParticle: 중심 + 색 + 축 2개) 으로 바꿔 넣는다.
//   그리기에 필요한 이미터별 설정 (SubUV / 블렌드 / 텍스처) 은 EmitterInfo 로 넘긴다.

#include "GpuBuffer.h"
#include "ModuleDesc.h"
#include "ParticleMaterial.h"

#include <cstdint>
#include <string>
#include <vector>

class GraphicsContext;
namespace Math { class Camera; }

class ParticleRenderList
{
public:
	static const uint32_t kNoAttribute = 0xFFFFFFFFu;
	static const uint32_t kMaxTextures = 32;          // 한 목록에서 쓸 수 있는 서로 다른 텍스처 수 (넘으면 0 번 텍스처)
	static const uint32_t kRenderParticleBytes = 64;  // HLSL ListParticle 과 같음 (중심 + 색 + 빌보드 축 2개)

	// 이미터 하나를 목록에 넣을 때 필요한 정보 (SpriteRenderer 가 채운다)
	struct Source
	{
		StructuredBuffer*  render = nullptr;        // 고정 렌더 구조체 (RenderParticle x capacity)
		ByteAddressBuffer* particles = nullptr;     // 가변 속성 SoA. SpriteFacing / SpriteAlignment 를 읽을 때만 쓰지만 항상 넘긴다
		ByteAddressBuffer* countBuffer = nullptr;   // nullptr 이면 constCount 사용 (CPU 이미터)
		uint32_t           countOffset = 0;         // countBuffer 안의 개수 위치 (GPU: DrawArgs.InstanceCount = 4)
		uint32_t           constCount = 0;
		uint32_t           capacity = 0;            // 버퍼 길이

		// 가변 속성 버퍼에 남아 있는 것 (Custom Facing / Custom Alignment 모드에서만 쓰인다)
		uint32_t spriteFacingOffset = kNoAttribute;
		uint32_t spriteAlignmentOffset = kNoAttribute;
		float3   defaultSpriteFacing = { 0.f, 1.f, 0.f };
		float3   defaultSpriteAlignment = { 0.f, 1.f, 0.f };

		// 빌보드 (SpriteSettings 와 같은 뜻, 번호 = SpriteFacingMode / SpriteAlignment)
		uint32_t facingMode = 0;
		uint32_t alignmentMode = 0;
		float2   pivot = { 0.5f, 0.5f };
		float    cameraOffset = 0.f;
		float    velocityStretch = 0.f;
		float    blendNear = 1.f;
		float    blendFar = 10.f;

		uint32_t subImageCols = 1;
		uint32_t subImageRows = 1;
		bool     subUVBlend = false;
		uint32_t blendMode = 0;                    // 0 = Translucent, 1 = Additive (SpriteBlendMode)
		uint32_t textureAlpha = 0;				// 0, 1, 2
		D3D12_CPU_DESCRIPTOR_HANDLE texture = {};   // 이미터 텍스처 (기본 원형 포함)

		const MaterialProgram* program = nullptr;
		const float4*          materialParams = nullptr;
		D3D12_CPU_DESCRIPTOR_HANDLE effectTextures[ParticleMaterial::kNumEffectTextures] = {};
	};

	// 모든 이미터 capacity 의 합만큼 버퍼를 잡는다. 크기가 바뀌면 GPU idle 상태에서 불러야 한다
	bool Reserve(uint32_t capacity);
	void Shutdown();
	uint32_t ReservedCapacity() const { return m_Capacity; }

	void Begin(GraphicsContext& gfx, const Math::Camera& camera);
	void Add(GraphicsContext& gfx, const Source& source);
	void Render(GraphicsContext& gfx, bool sorted = true);   // sorted == false: BitonicSort 생략

	// Gather 컬링 on / off (다음 Add 부터 적용)
	void SetCulling(bool enable) { m_Culling = enable; }
	bool Culling() const { return m_Culling; }

	uint32_t EmitterCount() const { return uint32_t(m_Emitters.size()); }

private:
	// HLSL StructuredBuffer<EmitterInfo> 와 같은 배치
	struct EmitterInfo
	{
		uint32_t subImageCols;
		uint32_t subImageRows;
		uint32_t subUVBlend;
		uint32_t blendMode;
		uint32_t textureIndex;
		uint32_t textureAlpha;
		uint32_t paramBase;
		uint32_t instrBase;
		uint32_t instrCount;
		uint32_t outputReg;
		uint32_t _pad[2];
		uint32_t effectTextures[4];
	};
	static_assert(sizeof(EmitterInfo) == 64, "EmitterInfo 64 B (HLSL ParticleListCommon.hlsli)");
	static_assert(ParticleMaterial::kNumEffectTextures == 4, "EmitterInfo.effectTextures 는 4 칸");

	uint32_t TextureIndex(D3D12_CPU_DESCRIPTOR_HANDLE srv);

	ByteAddressBuffer  m_RenderParticles;   // RenderParticle x capacity
	ByteAddressBuffer  m_SortList;          // uint2(목록 번호, 깊이 키) x capacity
	ByteAddressBuffer  m_Counter;           // [0] = 목록에 들어간 파티클 수
	IndirectArgsBuffer m_DrawArgs;          // (6, count, 0, 0)

	uint32_t m_Capacity = 0;
	bool     m_Active = false;              // Begin ~ Render 사이

	float    m_CamPos[3] = {};
	float    m_CamForward[3] = {};
	DirectX::XMFLOAT4X4 m_ViewProj = {};
	DirectX::XMFLOAT4   m_CamRight = {};
	DirectX::XMFLOAT4   m_CamUp = {};
	DirectX::XMFLOAT4   m_FrustumPlanes[6] = {};   // 월드 공간, 안쪽이 + (Begin 에서 카메라로 계산)
	bool                m_Culling = false;

	std::vector<EmitterInfo>                 m_Emitters;
	std::vector<D3D12_CPU_DESCRIPTOR_HANDLE> m_Textures;
	std::vector<float4>                      m_MaterialParams;
	std::vector<MaterialInstr>               m_Instrs;
	bool                                     m_TextureOverflowLogged = false;
};
