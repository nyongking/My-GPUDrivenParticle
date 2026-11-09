#include "pch.h"
#include "ParticleSorter.h"

#include <algorithm>

void SortBackToFront(const float3* positions, uint32_t count, const float3& cameraPosition, const float3& cameraForward, std::vector<uint32_t>& outIndices, SortScratch& scratch)
{
	scratch.items.resize(count);

	// 거리 계산
	for (uint32_t i = 0; i < count; ++i)
	{
		const float dx = positions[i].x - cameraPosition.x;
		const float dy = positions[i].y - cameraPosition.y;
		const float dz = positions[i].z - cameraPosition.z;

		scratch.items[i].key = dx * cameraPosition.x + dy * cameraPosition.y + dz * cameraPosition.z;
		scratch.items[i].index = i;
	}

	// key에 따라 소팅, 실제 파티클은 index에 따라서 렌더링된다.
	std::sort(scratch.items.begin(), scratch.items.end(),
		[](const SortScratch::KeyIndex& a, const SortScratch::KeyIndex& b)
		{
			return a.key > b.key;
		});

	for (uint32_t i = 0; i < count; ++i)
		outIndices[i] = scratch.items[i].index;
}
