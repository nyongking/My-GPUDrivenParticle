#pragma once

#include "GpuBuffer.h"
#include "ModuleDesc.h"
#include "ParticleRenderList.h"
#include "ParticleSorter.h"
#include "TextureManager.h"

#include <cstdint>
#include <string>
#include <map>

class GraphicsContext;
class EmitterInstance;
class GpuEmitterInstance;
class Block;
class ParticleMaterial;
namespace Math { class Camera; }

enum class SpriteBlendMode : uint32_t
{
    Transculent,
    Additive
};

enum class SpriteTextureAlpha : uint32_t
{
    Straight,       // 일반 PNG (RGB는 원래 색상, 알파는 따로)
    Premultiplied,  // RGB 에 알파가 이미 곱해진 방식
    FromLuminance,  // 알파 없음, 검은 배경 = 투명
    Count
};

// 사각형 앞면이 어디를 보는가 (HLSL SpriteFacing.hlsli 의 FACING_ 와 같은 번호)
enum class SpriteFacingMode : uint32_t
{
    FaceCameraPlane,        // 카메라 평면과 나란히 (기존 방식)
    FaceCameraPosition,     // 카메라 위치를 바라봄
    CustomFacingVector,     // SpriteFacing 바인딩 벡터를 바라봄 (예: (0,1,0) = 바닥에 눕힘)
    FaceCameraDistanceBlend,// 가까우면 CameraPosition, 멀면 CameraPlane
    Count
};

// 사각형 위쪽 축을 어디에 맞추는가 (HLSL ALIGN_)
enum class SpriteAlignment : uint32_t
{
    Unaligned,              // 카메라 위쪽
    VelocityAligned,        // Velocity 바인딩 방향 (+ velocityStretch)
    CustomAlignment,        // SpriteAlignment 바인딩 벡터 (예: (0,1,0) = 세로로 선 빌보드)
    Count
};

const char* SpriteFacingName(SpriteFacingMode mode);
const char* SpriteAlignmentName(SpriteAlignment mode);

struct SpriteSettings
{
    uint32_t    subImageCols = 1;
    uint32_t    subImageRows = 1;
    bool        subUVBlend = false;
    SpriteBlendMode blend = SpriteBlendMode::Transculent;
    SpriteTextureAlpha textureAlpha = SpriteTextureAlpha::Straight;

    // ---- 빌보드 ----
    SpriteFacingMode facing = SpriteFacingMode::FaceCameraPlane;
    SpriteAlignment  alignment = SpriteAlignment::Unaligned;
    float2      pivot = { 0.5f, 0.5f };     // uv 기준 (0.5, 0.5 = 가운데 / 0.5, 1 = 아래 가운데가 파티클 위치)
    float       cameraOffset = 0.f;         // 카메라 쪽으로 당기는 거리 (m)
    float       velocityStretch = 0.f;      // VelocityAligned: 세로 길이 += 속력 * 이 값 (초)
    float       blendNear = 1.f;            // FaceCameraDistanceBlend: 이 거리까지 CameraPosition
    float       blendFar = 10.f;            //                          이 거리부터 CameraPlane
};


class SpriteRenderer
{
public:
    // 칸 순서 = Desc() 의 bindings 순서
    //   SpriteSize 가 연결되어 있지 않으면 (Size * 2, Size * 2) 로 그린다 (Size = 반지름, 기존 크기 그대로)
    enum BindingSlot
    {
        kPosition, kColor, kSize, kSubImageIndex,
        kSpriteSize,        // float2 가로 / 세로 전체 길이
        kSpriteRotation,    // float  도
        kVelocity,          // float3 VelocityAligned 용
        kSpriteFacing,      // float3 CustomFacingVector 용 (연결이 없으면 기본값이 상수 벡터)
        kSpriteAlignment,   // float3 CustomAlignment 용
        kNumSlots
    };

    // SpriteRenderer의 기본값에 해당하는 Desc, 이미터마다 원하는 바인딩이 다를 경우 직접 RendererDesc를 지정해야 한다.
    static RendererDesc Desc();

    bool Initialize(const EmitterInstance& emitter, const RendererDesc& desc);
    void Shutdown();

    void Render(GraphicsContext& gfx, const Math::Camera& camera, const EmitterInstance& emitter);

    // GPU 이미터용: 버퍼는 GpuEmitterInstance 것을 그대로 읽고, 개수는 DrawIndirect 로 받는다
    bool InitializeGpu(const GpuEmitterInstance& emitter, const RendererDesc& desc);

    void SetDefaults(const RendererDesc& desc);
    void RenderGpu(GraphicsContext& gfx, const Math::Camera& camera, GpuEmitterInstance& emitter);

    // 공용 렌더 목록에 넣기 (여러 이미터를 한 번에 정렬해서 그릴 때). 그리기는 ParticleRenderList::Render 가 한다
    void SubmitToList(GraphicsContext& gfx, ParticleRenderList& list, const EmitterInstance& emitter);
    void SubmitToListGpu(GraphicsContext& gfx, ParticleRenderList& list, GpuEmitterInstance& emitter);

    void SetSortMode(SortMode mode) { m_SortMode = mode; }
    SortMode GetSortMode() const { return m_SortMode; }

    void SetMaterial(const ParticleMaterial* material) { m_Material = material; }

    void SetSettings(const SpriteSettings& settings);
    const SpriteSettings& Settings() const { return m_Settings; }


#ifndef RELEASE
    const std::string& Log() const { return m_Log; }
#endif

private:
    void UploadSortedIndices(GraphicsContext& gfx, const Math::Camera& camera, const EmitterInstance& emitter);

    // 레이아웃에서 Position / Color / Size offset 을 꺼낸다 (CPU / GPU 공통)
    bool ReadLayout(const Block& layout, uint32_t capacity, const RendererDesc& desc);
    // 상수 버퍼 / 루트 시그니처 / PSO / SRV 묶기 (CPU / GPU 공통). 그리기 호출만 따로
    //   sortedIndices = t1 (ByteAddressBuffer), sortStride = 정렬 목록 한 칸의 바이트 (CPU 4 = uint, GPU 8 = uint2(index, key))
    void BindForDraw(GraphicsContext& gfx, const Math::Camera& camera, ByteAddressBuffer& particles,
        ByteAddressBuffer& sortedIndices, bool sorted, uint32_t sortStride);
    // GPU 정렬: 깊이 키 생성(ParticleSortKeyCS) -> BitonicSort 내림차순 (먼 것부터 = back-to-front)
    void SortOnGpu(GraphicsContext& gfx, const Math::Camera& camera, GpuEmitterInstance& emitter);
    // 공용 렌더 목록용 공통 정보 (레이아웃 / 기본값 / 설정 / 텍스처). 버퍼와 개수는 호출한 쪽이 채운다
    ParticleRenderList::Source MakeListSource() const;

    static const uint32_t kNoAttribute = 0xFFFFFFFFu;   // 셰이더의 NO_ATTRIBUTE 와 같음

    D3D12_CPU_DESCRIPTOR_HANDLE TextureSRV(uint32_t slot) const;

    const ParticleMaterial* m_Material = nullptr;
    

    ByteAddressBuffer   m_ParticleBuffer;
    ByteAddressBuffer   m_SortedIndexBuffer;    // CPU: 정렬된 인덱스 (uint)
    ByteAddressBuffer   m_GpuSortBuffer;        // GPU: 정렬 목록 uint2(index, key) x capacity (BitonicSort 가 RW raw 로 씀)

    uint32_t            m_BufferBytes;
    uint32_t            m_Offsets[kNumSlots] = {};
    float3              m_DefaultPosition = { 0.f, 0.f, 0.f };
    float4              m_DefaultColor = { 1.f, 1.f, 1.f, 1.f };
    float               m_DefaultSize = 0.1f;
    float               m_DefaultSubImage = 0.f;
    float               m_DefaultSpriteRotation = 0.f;
    float3              m_DefaultVelocity = { 0.f, 0.f, 0.f };
    float3              m_DefaultSpriteFacing = { 0.f, 1.f, 0.f };
    float3              m_DefaultSpriteAlignment = { 0.f, 1.f, 0.f };

    SpriteSettings      m_Settings;

    uint32_t            m_Capacity;

    bool                m_Ready;

    SortMode            m_SortMode = SortMode::ViewDepth;
    std::vector<uint32_t>  m_SortedIndices;
    SortScratch         m_SortScratch;

#ifndef RELEASE
    std::string m_Log;
#endif
};

