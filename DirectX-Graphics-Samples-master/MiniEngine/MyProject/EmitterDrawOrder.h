#pragma once
// 이미터 단위의 그리는 순서 ("준 List" 방식)
//
//   공용 렌더 목록 (ParticleRenderList) 처럼 모든 파티클을 한데 섞어 정렬하지 않는다.
//   이미터는 각자 자기 셰이더로 그리고 (Preset / Custom 머티리얼 그대로), 이미터 "사이" 의 순서만 CPU 에서 정한다.
//
//     1. 정렬 키 (sortKey) 가 작은 이미터부터.  키가 클수록 나중에 = 더 위에 그려진다 (기본 0, 사용자가 정함)
//     2. 키가 같으면 카메라에서 먼 이미터부터 (이미터 위치 기준, 파티클 위치가 아님)
//     3. 그것도 같으면 넣은 순서 (이미터 목록 순서)
//
//   반투명 / Additive 를 구분하지 않는다: 연기 뒤의 불꽃이 먼저 그려져야 연기에 가려지기 때문.
//   이미터 "안" 의 파티클 정렬은 SpriteRenderer 가 한다 (반투명 = 깊이 정렬, Additive = 정렬 없음)

#include "AttributeType.h"

#include <vector>

class EmitterRuntime;
namespace Math { class Camera; }

class EmitterDrawOrder
{
public:
	struct Item
	{
		EmitterRuntime* runtime = nullptr;
		int             sortKey = 0;
		float           depth = 0.f;    // 카메라 앞 방향으로의 거리 (클수록 멀다)
	};

	// 프레임마다: Begin -> Add x 이미터 수 -> Sorted
	void Begin(const Math::Camera& camera);
	void Add(EmitterRuntime* runtime, const float3& worldPosition, int sortKey);
	const std::vector<Item>& Sorted();      // 그릴 순서대로

private:
	float3            m_CamPosition = { 0.f, 0.f, 0.f };
	float3            m_CamForward = { 0.f, 0.f, 1.f };
	std::vector<Item> m_Items;
};
