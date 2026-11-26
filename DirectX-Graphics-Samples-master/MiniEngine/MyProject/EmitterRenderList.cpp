#include "pch.h"
#include "EmitterRenderList.h"

#include "Camera.h"
#include "EmitterRuntime.h"

#include <algorithm>

void EmitterRenderList::Begin(const Math::Camera& camera)
{
	const Math::Vector3 p = camera.GetPosition();
	const Math::Vector3 f = camera.GetForwardVec();
	m_Camera = &camera;
	m_CamPosition = { float(p.GetX()), float(p.GetY()), float(p.GetZ()) };
	m_CamForward = { float(f.GetX()), float(f.GetY()), float(f.GetZ()) };
	m_Items.clear();
}

void EmitterRenderList::Add(EmitterRuntime* runtime, const float3& worldPosition, int sortKey)
{
	if (!runtime || !m_Camera)
		return;

	Item item;
	item.runtime = runtime;
	item.sortKey = sortKey;
	// 파티클 정렬 (SortBackToFront / ParticleSortKeyCS) 과 같은 기준: 카메라 앞 방향으로의 거리
	item.depth = (worldPosition.x - m_CamPosition.x) * m_CamForward.x
		+ (worldPosition.y - m_CamPosition.y) * m_CamForward.y
		+ (worldPosition.z - m_CamPosition.z) * m_CamForward.z;
	m_Items.push_back(item);
}

void EmitterRenderList::Render(GraphicsContext& gfx)
{
	if (!m_Camera)
		return;

	if (m_Sorted)
	{
		// stable: 키와 거리가 모두 같으면 Add 한 순서를 지킨다 -> 프레임마다 순서가 뒤바뀌지 않는다
		std::stable_sort(m_Items.begin(), m_Items.end(), [](const Item& a, const Item& b)
			{
				if (a.sortKey != b.sortKey)
					return a.sortKey < b.sortKey;   // 키가 큰 것이 나중 = 위
				return a.depth > b.depth;           // 먼 것부터
			});
	}

	for (const Item& item : m_Items)
		item.runtime->Render(gfx, *m_Camera);

	m_Items.clear();
	m_Camera = nullptr;
}
