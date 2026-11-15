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
namespace Math { class Camera; }

enum class SpriteBlendMode : uint32_t
{
    Transculent,
    Additive
};

struct SpriteSettings
{
    uint32_t    subImageCols = 1;
    uint32_t    subImageRows = 1;
    bool        subUVBlend = false;
    SpriteBlendMode blend = SpriteBlendMode::Transculent;
};


class SpriteRenderer
{
public:
    enum BindingSlot { kPosition, kColor, kSize, kSubImageIndex, kNumSlots };

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

    bool SetTexture(const std::wstring& path);
    void ReleaseTextures();

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

    D3D12_CPU_DESCRIPTOR_HANDLE TextureSRV() const;

    // 한 번 불러온 텍스처는 바꿔도 지우지 않는다 (이전 프레임이 아직 읽고 있을 수 있어서)
    std::map<std::wstring, TextureRef> m_Textures;
    const TextureRef* m_Texture = nullptr;       // nullptr = 기본 원형

    ByteAddressBuffer   m_ParticleBuffer;
    ByteAddressBuffer   m_SortedIndexBuffer;    // CPU: 정렬된 인덱스 (uint)
    ByteAddressBuffer   m_GpuSortBuffer;        // GPU: 정렬 목록 uint2(index, key) x capacity (BitonicSort 가 RW raw 로 씀)

    uint32_t            m_BufferBytes;
    uint32_t            m_Offsets[kNumSlots] = {};
    float3              m_DefaultPosition = { 0.f, 0.f, 0.f };
    float4              m_DefaultColor = { 1.f, 1.f, 1.f, 1.f };
    float               m_DefaultSize = 0.1f;
    float               m_DefaultSubImage = 0.f;

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

