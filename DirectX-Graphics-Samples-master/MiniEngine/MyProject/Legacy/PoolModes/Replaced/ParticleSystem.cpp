#include "pch.h"
#include "ParticleSystem.h"

#include "GraphicsCore.h"
#include "GraphicsCommon.h"
#include "BufferManager.h"
#include "CommandContext.h"
#include "RootSignature.h"
#include "PipelineState.h"
#include "EngineTuning.h"
#include "EngineProfiling.h"
#include "Camera.h"

#include "ParticleUtility.h"
#include "ParticlePool.h"
#include "SlotReusePool.h"
#include "AliveListPool.h"
#include "FullScanPool.h"
#include "CompactionPool.h"

#include <algorithm>

using namespace Graphics;
using namespace Math;
using namespace Particles;


void ParticleSystem::Initialize(const ParticleSettings& settings, const Math::Vector3& emitterPosition)
{
	m_Settings = Sanitize(settings);
	CreatePool(m_Settings.mode, m_Settings.maxParticles);

	m_ParticleEmitter.position = emitterPosition;
	m_ParticleEmitter.emitParams = BuildEmitterParams(m_Settings);
	m_ParticleEmitter.ResetBurst();
}


void ParticleSystem::Shutdown()
{
	DestroyPool();
}

void ParticleSystem::Update(float deltaT)
{
	// if has PendingChange
	ApplyPending();

	EmitRequest request = m_ParticleEmitter.RequestEmit(deltaT);
	request.emitCount = min(request.emitCount, m_ParticlePool->GetMaxParticles());

	m_ParticlePool->Update(deltaT, request);
}

void ParticleSystem::Render(GraphicsContext& gfx, const Math::Camera& cam)
{
	m_ParticlePool->Render(gfx, cam);
}

void ParticleSystem::ApplySettings(const ParticleSettings& settings, bool forceRecreate)
{
	m_PendingSettings = Sanitize(settings);
	m_HasPending = true;
	m_ForceRecreate = m_ForceRecreate || forceRecreate;
}

Particles::ParticleStat ParticleSystem::GetParticleStat()
{
	Particles::ParticleStat stat = {};
	stat.mode = m_ParticlePool->GetMode();
	stat.maxParticles = m_ParticlePool->GetMaxParticles();

	if (IsDefault(stat.mode))
	{
		stat.counters = static_cast<FullScanPool*>(m_ParticlePool.get())->GetCounterSnapshot();
	}

	else if (IsSlotFamily(stat.mode))
	{
		stat.counters = static_cast<SlotReusePool*>(m_ParticlePool.get())->GetCounterSnapshot();
	}

	else if (IsAliveFamily(stat.mode))
	{
		stat.counters = static_cast<AliveListPool*>(m_ParticlePool.get())->GetCounterSnapshot();
	}

	return stat;
}

void ParticleSystem::ApplyPending()
{
	if (!m_HasPending)
		return;

	const ParticleSettings& cur = m_Settings;
	const ParticleSettings& next = m_PendingSettings;

	// 1) Pool : [reset 요청, MAX 변경, 계열 변경] 중 하나라면 새로 만든다
	const bool recreate = m_ForceRecreate
		|| next.maxParticles != cur.maxParticles
		|| !IsSameFamily(next.mode, cur.mode);

	if (recreate)
	{
		DestroyPool();
		CreatePool(next.mode, next.maxParticles);
	}
	else if (next.mode != cur.mode)
	{
		m_ParticlePool->SetMode(next.mode);	// 같은 계열: 버퍼는 그대로, 모드만
	}

	// 2) Emitter : 비율 -> 개수 변환 MAX에 따라 달라지므로 항상 다시 계산
	m_ParticleEmitter.emitParams = BuildEmitterParams(next);
	m_ParticleEmitter.ResetBurst();

	m_Settings = next;
	m_HasPending = false;
	m_ForceRecreate = false;
}

void ParticleSystem::CreatePool(Particles::Mode mode, uint32_t maxParticles)
{
	if (IsDefault(mode))
		m_ParticlePool = std::make_unique<FullScanPool>();
	else if (IsSlotFamily(mode))
		m_ParticlePool = std::make_unique<SlotReusePool>(mode);
	else
		m_ParticlePool = std::make_unique<AliveListPool>(mode);

	m_ParticlePool->Initialize(maxParticles);
}

void ParticleSystem::DestroyPool()
{
	if (!m_ParticlePool)
		return;

	Graphics::g_CommandManager.IdleGPU();	// 이전 프레임들이 아직 버퍼를 쓰고 있을 수 있음
	m_ParticlePool->Shutdown();
	m_ParticlePool.reset();					// 새 풀을 만들기 전에 먼저 지움 (VRAM 두 배 방지)
}
