#pragma once

#include "GpuBuffer.h"
#include "ReadbackBuffer.h"
#include "VectorMath.h"
#include "ParticleTypes.h"
#include "ParticlePool.h"
#include "ParticleEmitter.h"
#include "ParticleSettings.h"

class GraphicsContext;
namespace Math { class Camera; }

class ParticleSystem
{
public:
	void Initialize(const ParticleSettings& settings, const Math::Vector3& emitterPosition);
	void Shutdown();

	void Update(float deltaT);
	void Render(GraphicsContext& gfx, const Math::Camera& cam);

	void ApplySettings(const ParticleSettings& settings, bool forceRecreate = false);

	const ParticleSettings& GetSettings() const { return m_Settings; }
	Particles::ParticleStat	GetParticleStat();

private:
	void ApplyPending();
	void CreatePool(Particles::Mode mode, uint32_t maxParticles);
	void DestroyPool();

	// -----------------------
	//		Particle Setting
	// -----------------------
	std::unique_ptr<ParticlePool>	m_ParticlePool;
	ParticleEmitter					m_ParticleEmitter;

	// -----------------------
	//		Settings
	// -----------------------
	ParticleSettings	m_Settings;
	ParticleSettings	m_PendingSettings;
	bool				m_HasPending = false;
	bool				m_ForceRecreate = false;
};

