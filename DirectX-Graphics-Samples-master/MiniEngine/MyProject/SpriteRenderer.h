#pragma once

#include "GpuBuffer.h"
#include "ModuleDesc.h"
#include "ParticleSorter.h"

#include <cstdint>
#include <string>

class GraphicsContext;
class EmitterInstance;
class GpuEmitterInstance;
struct Block;
namespace Math { class Camera; }

class SpriteRenderer
{
public:
    static RendererDesc Desc();

    bool Initialize(const EmitterInstance& emitter);
    void Shutdown();

    void Render(GraphicsContext& gfx, const Math::Camera& camera, const EmitterInstance& emitter);

    // GPU 이미터용 (H5): 버퍼는 GpuEmitterInstance 것을 그대로 읽고, 개수는 DrawIndirect 로 받는다
    //   정렬은 아직 없음 (G3 에서 GPU 정렬 결과를 t1 에 넣는다)
    bool InitializeGpu(const GpuEmitterInstance& emitter);
    void RenderGpu(GraphicsContext& gfx, const Math::Camera& camera, GpuEmitterInstance& emitter);

    void SetSortMode(SortMode mode) { m_SortMode = mode; }
    SortMode GetSortMode() const { return m_SortMode; }


#ifndef RELEASE
    const std::string& Log() const { return m_Log; }
#endif

private:
    void UploadSortedIndices(GraphicsContext& gfx, const Math::Camera& camera, const EmitterInstance& emitter);

    // 레이아웃에서 Position / Color / Size offset 을 꺼낸다 (CPU / GPU 공통)
    bool ReadLayout(const Block& layout, uint32_t capacity);
    // 상수 버퍼 / 루트 시그니처 / PSO / SRV 묶기 (CPU / GPU 공통). 그리기 호출만 따로
    void BindForDraw(GraphicsContext& gfx, const Math::Camera& camera, ByteAddressBuffer& particles, bool sorted);


    ByteAddressBuffer   m_ParticleBuffer;
    StructuredBuffer    m_SortedIndexBuffer;

    uint32_t            m_BufferBytes;
    uint32_t            m_PositionOffset;
    uint32_t            m_ColorOffset;
    uint32_t            m_SizeOffset;
    uint32_t            m_Capacity;

    bool                m_Ready;

    SortMode            m_SortMode = SortMode::ViewDepth;
    std::vector<uint32_t>  m_SortedIndices;
    SortScratch         m_SortScratch;

#ifndef RELEASE
    std::string m_Log;
#endif
};

