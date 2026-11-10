#pragma once

#include "GpuBuffer.h"
#include "ModuleDesc.h"
#include "ParticleSorter.h"

#include <cstdint>
#include <string>

class GraphicsContext;
class EmitterInstance;
class GpuEmitterInstance;
class Block;
namespace Math { class Camera; }

class SpriteRenderer
{
public:
    enum BindingSlot { kPosition, kColor, kSize, kNumSlots };

    static RendererDesc Desc();

    bool Initialize(const EmitterInstance& emitter, const RendererDesc& desc);
    void Shutdown();

    void Render(GraphicsContext& gfx, const Math::Camera& camera, const EmitterInstance& emitter);

    // GPU 이미터용: 버퍼는 GpuEmitterInstance 것을 그대로 읽고, 개수는 DrawIndirect 로 받는다
    bool InitializeGpu(const GpuEmitterInstance& emitter, const RendererDesc& desc);
   
    void SetDefaults(const RendererDesc& desc);
    void RenderGpu(GraphicsContext& gfx, const Math::Camera& camera, GpuEmitterInstance& emitter);

    void SetSortMode(SortMode mode) { m_SortMode = mode; }
    SortMode GetSortMode() const { return m_SortMode; }


#ifndef RELEASE
    const std::string& Log() const { return m_Log; }
#endif

private:
    void UploadSortedIndices(GraphicsContext& gfx, const Math::Camera& camera, const EmitterInstance& emitter);

    // 레이아웃에서 Position / Color / Size offset 을 꺼낸다 (CPU / GPU 공통)
    bool ReadLayout(const Block& layout, uint32_t capacity, const RendererDesc& desc);
    // 상수 버퍼 / 루트 시그니처 / PSO / SRV 묶기 (CPU / GPU 공통). 그리기 호출만 따로
    void BindForDraw(GraphicsContext& gfx, const Math::Camera& camera, ByteAddressBuffer& particles, bool sorted);

    static const uint32_t kNoAttribute = 0xFFFFFFFFu;   // 셰이더의 NO_ATTRIBUTE 와 같음

    ByteAddressBuffer   m_ParticleBuffer;
    StructuredBuffer    m_SortedIndexBuffer;

    uint32_t            m_BufferBytes;
    uint32_t            m_Offsets[kNumSlots] = {};
    float3              m_DefaultPosition = { 0.f, 0.f, 0.f };
    float4              m_DefaultColor = { 1.f, 1.f, 1.f, 1.f };
    float               m_DefaultSize = 0.1f;

    uint32_t            m_Capacity;

    bool                m_Ready;

    SortMode            m_SortMode = SortMode::ViewDepth;
    std::vector<uint32_t>  m_SortedIndices;
    SortScratch         m_SortScratch;

#ifndef RELEASE
    std::string m_Log;
#endif
};

