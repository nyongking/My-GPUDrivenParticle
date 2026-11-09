#pragma once

#include "VectorMath.h"
#include "ParticleTypes.h"

class GraphicsContext;
struct EmitRequest;

namespace Math { class Camera; }

class ParticlePool
{
public:
	ParticlePool(Particles::Mode mode) : m_Mode(mode) {}
	virtual ~ParticlePool() {}

public:
	virtual void Initialize(uint32_t maxParticles = 0) = 0;
	virtual void CreateBuffers(uint32_t maxParticles = 0) = 0;
	virtual void Shutdown() = 0;

	virtual void Update(float deltaT, const EmitRequest& emitRequest) = 0;
	virtual void Render(GraphicsContext& gfx, const Math::Camera& cam) = 0;
	
	void				SetMode(Particles::Mode mode) { m_Mode = mode; }
	Particles::Mode		GetMode() const { return m_Mode; }
	uint32_t			GetMaxParticles() const { return m_MaxParticles; }


protected:
	static constexpr uint32_t sReadbackLatency = 3;

	uint32_t			m_MaxParticles = 0;
	Particles::Mode		m_Mode = Particles::Mode::SlotReuse;

	uint32_t			m_FrameIndex = 0;
};

