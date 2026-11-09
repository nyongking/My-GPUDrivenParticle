#pragma once

#include "GpuBuffer.h"
#include "ModuleDesc.h"
#include "ParticleSorter.h"

#include <cstdint>
#include <string>

class GraphicsContext;
class EmitterInstance;
namespace Math { class Camera; }

class SpriteRenderer
{
public:
	static RendererDesc Desc();

    bool Initialize(const EmitterInstance& emitter);
    void Shutdown();

    void Render(GraphicsContext& gfx, const Math::Camera& camera, const EmitterInstance& emitter);

    void SetSortMode(SortMode mode) { m_SortMode = mode; }
    SortMode GetSortMode() const { return m_SortMode; }


#ifndef RELEASE
    const std::string& Log() const { return m_Log; }
#endif

private:
    void UploadSortedIndices(GraphicsContext& gfx, const Math::Camera& camera, const EmitterInstance& emitter);


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

