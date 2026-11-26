#pragma once

#include "GpuBuffer.h"
#include "ModuleDesc.h"
#include "ParticleRenderList.h"
#include "ParticleSorter.h"
#include "RenderParticle.h"
#include "TextureManager.h"

#include <cstdint>
#include <string>
#include <map>

class GraphicsContext;
class EmitterInstance;
class GpuEmitterInstance;
class Block;
class ParticleMaterial;
class GraphicsPSO;
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
    //   파티클 값은 고정 렌더 구조체 (RenderParticle.h) 에서 읽는다. 어느 속성을 읽을지는 고정이고,
    //   여기서 정하는 것은 "아무 모듈도 그 속성을 쓰지 않을 때의 값" (기본값) 뿐이다.
    //   kSpriteFacing / kSpriteAlignment 만 예외: 일부 모드에서만 쓰여서 가변 속성 버퍼(SoA) 에서 offset 으로 읽는다
    enum BindingSlot
    {
        kPosition, kColor, kSize, kSubImageIndex,
        kSpriteRotation,    // float  도
        kVelocity,          // float3 VelocityAligned 용
        kSpriteFacing,      // float3 CustomFacingVector 용 (속성이 없으면 기본값이 상수 벡터)
        kSpriteAlignment,   // float3 CustomAlignment 용
        kNumSlots,
        kNumRenderSlots = kSpriteFacing     // 이 앞까지가 렌더 구조체의 필드
    };

    // SpriteRenderer 의 칸 목록 + 기본값. 연결되는 속성은 고정이다 (attribute 는 바꿔도 반영되지 않는다)
    static RendererDesc Desc();

    // desc 의 기본값 -> 렌더 구조체 한 개. 시뮬레이션이 새 파티클을 이 값에서 시작한다 (모듈이 덮어쓴다)
    //   SpriteSize 는 항상 "없음" (-1): SpriteSize 를 쓰는 모듈이 없으면 Size * 2 로 그린다
    static RenderParticleData RenderDefaults(const RendererDesc& desc);

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
    // 실제로 정렬해서 그리는가. Additive 는 더하기라 그리는 순서가 결과에 영향이 없으므로 정렬하지 않는다
    bool ShouldSort() const { return m_SortMode == SortMode::ViewDepth && m_Settings.blend != SpriteBlendMode::Additive; }
    SortMode GetSortMode() const { return m_SortMode; }

    void SetMaterial(const ParticleMaterial* material) { m_Material = material; }

    // 머티리얼 전용 픽셀 셰이더의 PSO 로 그린다 (nullptr = 기본 PSO: EffectMode 분기 / VM 인터프리터)
    //   Preset : PresetPSO(effect)                (효과마다 고정된 셰이더)
    //   Custom : MaterialGpuProgram 이 가진 PSO   (그래프마다 만든 셰이더)
    //   공용 렌더 목록에는 영향이 없다
    void SetMaterialPSO(const GraphicsPSO* pso) { m_MaterialPSO = pso; }

    // Preset 모드: 효과마다 고정된 픽셀 셰이더 (Shaders/SpritePreset*PS.hlsl) 의 PSO. 프로그램이 끝날 때까지 살아 있다
    //   Preset 을 늘릴 때는 SpriteRenderer.cpp 의 kPresetShaders 표에 추가한다
    static const GraphicsPSO& PresetPSO(ParticleMaterial::Effect effect);
    static const char* PresetShaderName(ParticleMaterial::Effect effect);   // UI 표시용 (셰이더 파일 이름)
    // 기본 PSO 와 같은 설정에 픽셀 셰이더만 바꾼 PSO 를 만든다. ps 는 PSO 를 쓰는 동안 살아 있어야 한다
    static void CreateMaterialPSO(GraphicsPSO& pso, const void* ps, size_t size);

    void SetSettings(const SpriteSettings& settings);
    const SpriteSettings& Settings() const { return m_Settings; }


#ifndef RELEASE
    const std::string& Log() const { return m_Log; }
#endif

private:
    void UploadSortedIndices(GraphicsContext& gfx, const Math::Camera& camera, const EmitterInstance& emitter);

    // CPU 이미터의 결과를 GPU 버퍼로 올린다 (렌더 구조체는 항상, 가변 속성은 Facing / Alignment 를 읽을 때만)
    void UploadCpu(GraphicsContext& gfx, const EmitterInstance& emitter);

    // 가변 속성 레이아웃에서 SpriteFacing / SpriteAlignment 의 offset 을 꺼낸다 (CPU / GPU 공통)
    bool ReadLayout(const Block& layout, uint32_t capacity, const RendererDesc& desc);
    // 상수 버퍼 / 루트 시그니처 / PSO / SRV 묶기 (CPU / GPU 공통). 그리기 호출만 따로
    //   render = t0 (렌더 구조체), sortedIndices = t1, particles = t2 (가변 속성 SoA)
    //   sortStride = 정렬 목록 한 칸의 바이트 (CPU 4 = uint, GPU 8 = uint2(index, key))
    void BindForDraw(GraphicsContext& gfx, const Math::Camera& camera, StructuredBuffer& render, ByteAddressBuffer& particles,
        ByteAddressBuffer& sortedIndices, bool sorted, uint32_t sortStride);
    // GPU 정렬: 깊이 키 생성(ParticleSortKeyCS) -> BitonicSort 내림차순 (먼 것부터 = back-to-front)
    void SortOnGpu(GraphicsContext& gfx, const Math::Camera& camera, GpuEmitterInstance& emitter);
    // 공용 렌더 목록용 공통 정보 (레이아웃 / 기본값 / 설정 / 텍스처). 버퍼와 개수는 호출한 쪽이 채운다
    ParticleRenderList::Source MakeListSource() const;

    static const uint32_t kNoAttribute = 0xFFFFFFFFu;   // 셰이더의 NO_ATTRIBUTE 와 같음

    D3D12_CPU_DESCRIPTOR_HANDLE TextureSRV(uint32_t slot) const;

    const ParticleMaterial* m_Material = nullptr;
    const GraphicsPSO*      m_MaterialPSO = nullptr;
    

    StructuredBuffer    m_RenderBuffer;         // CPU: 렌더 구조체 업로드용 (GPU 이미터는 이미터의 버퍼를 그대로 읽는다)
    ByteAddressBuffer   m_ParticleBuffer;       // CPU: 가변 속성 (SoA) 업로드용
    ByteAddressBuffer   m_SortedIndexBuffer;    // CPU: 정렬된 인덱스 (uint)
    ByteAddressBuffer   m_GpuSortBuffer;        // GPU: 정렬 목록 uint2(index, key) x capacity (BitonicSort 가 RW raw 로 씀)

    uint32_t            m_BufferBytes;
    uint32_t            m_FacingOffset = kNoAttribute;      // 가변 속성 버퍼의 Particles.SpriteFacing
    uint32_t            m_AlignmentOffset = kNoAttribute;   // 가변 속성 버퍼의 Particles.SpriteAlignment
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

