#include "pch.h"
#include "ParticleRenderer.h"

#include "CommandListManager.h"
#include "EmitterRuntime.h"
#include "GraphicsCore.h"

const char* ParticleRenderer::ModeName(ParticleRenderMode mode)
{
	switch (mode)
	{
	case ParticleRenderMode::SharedList:  return "SharedList";
	case ParticleRenderMode::EmitterList: return "EmitterList";
	default:                              return "Sprite";
	}
}

void ParticleRenderer::Reserve(uint32_t capacityTotal)
{
	const uint32_t want = (m_Mode == ParticleRenderMode::SharedList) ? capacityTotal : 0u;
	if (want == m_Reserved)
		return;

	Graphics::g_CommandManager.IdleGPU();   // 지난 프레임이 아직 이 버퍼를 쓰고 있을 수 있다
	if (want == 0)
	{
		m_TranslucentList.Shutdown();
		m_AdditiveList.Shutdown();
	}
	else
	{
		m_TranslucentList.Reserve(want);
		m_AdditiveList.Reserve(want);
	}
	m_Reserved = want;
}

void ParticleRenderer::Shutdown()
{
	if (m_Reserved == 0)
		return;

	Graphics::g_CommandManager.IdleGPU();
	m_TranslucentList.Shutdown();
	m_AdditiveList.Shutdown();
	m_Reserved = 0;
}

void ParticleRenderer::Begin(GraphicsContext& gfx, const Math::Camera& camera)
{
	m_Submitted = 0;
	m_ListActive = (m_Mode == ParticleRenderMode::SharedList) && m_Reserved > 0;
	
	if (m_ListActive)
	{
		m_TranslucentList.SetCulling(m_Culling);
		m_TranslucentList.Begin(gfx, camera);

		m_AdditiveList.SetCulling(m_Culling);
		m_AdditiveList.Begin(gfx, camera);
	}

	// Sprite 는 Add 한 순서 그대로, 그 밖에는 정렬 키 -> 거리
	m_EmitterList.SetSorted(m_Mode != ParticleRenderMode::Sprite);
	m_EmitterList.Begin(camera);
}

void ParticleRenderer::Add(GraphicsContext& gfx, EmitterRuntime* runtime, const float3& worldPosition, int sortKey)
{
	if (!runtime)
		return;

	// 공용 목록: 잡아 둔 크기 안에 들어갈 때만 (Reserve 뒤에 이미터가 늘었으면 그 프레임은 이미터 단위로 그린다)
	if (m_ListActive && m_Submitted + runtime->Capacity() <= m_Reserved)
	{
		m_Submitted += runtime->Capacity();
		runtime->SubmitTo(gfx, runtime->IsAdditive() ? m_AdditiveList : m_TranslucentList);
	}
	else
		m_EmitterList.Add(runtime, worldPosition, sortKey);
}

void ParticleRenderer::Render(GraphicsContext& gfx)
{
	if (m_ListActive)
	{
		// Additive 끼리는 순서가 결과에 영향이 없어서 정렬하지 않고 반투명 뒤에 그린다
		m_TranslucentList.Render(gfx, true);
		m_AdditiveList.Render(gfx, false);
		m_ListActive = false;
	}
	m_EmitterList.Render(gfx);
}
