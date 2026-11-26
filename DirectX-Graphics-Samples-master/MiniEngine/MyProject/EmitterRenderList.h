#pragma once
// 이미터 렌더 목록 ("준 List" 방식)
//
//   그릴 이미터를 배열에 모았다가, 이미터 "사이" 의 순서를 CPU 에서 정해서 하나씩 그린다.
//   ParticleRenderList 와 쓰는 법이 같다 (Begin -> Add x 이미터 수 -> Render). 차이는 목록의 단위:
//     ParticleRenderList : 파티클 단위. 모든 파티클을 한데 섞어 GPU 에서 정렬, Draw 1번 (머티리얼은 VM)
//     EmitterRenderList  : 이미터 단위. 이미터는 각자 자기 셰이더로 그린다 (Preset / Custom), Draw 는 이미터 수만큼
//
//   그리는 순서
//     1. 정렬 키 (sortKey) 가 작은 이미터부터.  키가 클수록 나중에 = 더 위에 그려진다 (기본 0, 사용자가 정함)
//     2. 키가 같으면 카메라에서 먼 이미터부터 (이미터 위치 기준, 파티클 위치가 아님)
//     3. 그것도 같으면 Add 한 순서
//
//   반투명 / Additive 를 구분하지 않는다: 연기 뒤의 불꽃이 먼저 그려져야 연기에 가려지기 때문.
//   이미터 "안" 의 파티클 정렬은 SpriteRenderer 가 한다 (반투명 = 깊이 정렬, Additive = 정렬 없음)
//
//   ParticleRenderer 가 갖고 쓴다 (렌더 방식이 EmitterList / Sprite 일 때). Sprite 는 SetSorted(false) 로 정렬만 끈 것

#include "AttributeType.h"

#include <vector>

class EmitterRuntime;
class GraphicsContext;
namespace Math { class Camera; }

class EmitterRenderList
{
public:
	// 프레임마다: Begin -> Add x 이미터 수 -> Render
	void Begin(const Math::Camera& camera);
	void Add(EmitterRuntime* runtime, const float3& worldPosition, int sortKey = 0);
	void Render(GraphicsContext& gfx);      // 정렬 후 순서대로 EmitterRuntime::Render. 목록은 비워진다

	// false 면 순서를 정하지 않고 Add 한 순서대로 그린다 (Sprite 방식)
	void SetSorted(bool sorted) { m_Sorted = sorted; }
	bool Sorted() const { return m_Sorted; }

	uint32_t EmitterCount() const { return uint32_t(m_Items.size()); }

private:
	struct Item
	{
		EmitterRuntime* runtime = nullptr;
		int             sortKey = 0;
		float           depth = 0.f;    // 카메라 앞 방향으로의 거리 (클수록 멀다)
	};

	const Math::Camera* m_Camera = nullptr;     // Begin ~ Render 사이에만 유효
	float3              m_CamPosition = { 0.f, 0.f, 0.f };
	float3              m_CamForward = { 0.f, 0.f, 1.f };
	std::vector<Item>   m_Items;
	bool                m_Sorted = true;
};
