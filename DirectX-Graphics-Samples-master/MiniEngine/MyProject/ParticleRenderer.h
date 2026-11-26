#pragma once
// 파티클 전체의 렌더 방식 (전역 설정)
//
//   이미터를 가진 쪽 (EffectSystem, EmitterTestScene) 은 그리는 방식을 모른다. Add 로 이미터를 넘기기만 한다.
//   어떤 방식으로 그릴지는 여기서 정한다 (Main 이 하나 갖고, UI 에서 고른다).
//
//     SharedList  : 공용 목록.  모든 이미터의 파티클을 한 목록에 모아 파티클 단위로 깊이 정렬, Draw 1번 (ParticleRenderList)
//                   반투명 목록 (정렬) -> Additive 목록 (정렬 없음) 순서. 머티리얼은 VM 인터프리터
//     EmitterList : 준 List.   이미터마다 자기 셰이더로 그리되 이미터 사이의 순서를 정한다 (EmitterRenderList)
//                   정렬 키 -> 카메라 거리. 머티리얼은 Preset / Custom 셰이더
//     Sprite      : 이미터마다 자기 셰이더로, Add 한 순서 그대로 그린다 (이미터 사이의 순서를 정하지 않음)
//
//   프레임마다: Reserve (Tick 전) -> Begin -> Add x 이미터 수 -> Render
//
//   같은 종류의 전역 설정: 시뮬레이션 = UberGpuProgram::Mode (GpuSimMode), 머티리얼 = MaterialGpuProgram::Mode

#include "EmitterRenderList.h"
#include "ParticleRenderList.h"

#include <cstdint>

class EmitterRuntime;
class GraphicsContext;
namespace Math { class Camera; }

enum class ParticleRenderMode { SharedList, EmitterList, Sprite };

class ParticleRenderer
{
public:
	void               SetMode(ParticleRenderMode mode) { m_Mode = mode; }
	ParticleRenderMode Mode() const { return m_Mode; }
	static const char* ModeName(ParticleRenderMode mode);

	// SharedList 전용: Gather 에서 화면 밖 / 투명 / 크기 0 파티클을 빼고 정렬
	void SetCulling(bool enable) { m_Culling = enable; }
	bool Culling() const { return m_Culling; }

	// 이번 프레임에 그릴 이미터 capacity 의 합. SharedList 일 때만 그만큼 버퍼를 잡고, 다른 방식이면 놓는다.
	// 크기가 바뀌면 GPU 를 기다리므로 커맨드를 기록하기 전 (Tick 전) 에 부른다
	void Reserve(uint32_t capacityTotal);
	void Shutdown();

	void Begin(GraphicsContext& gfx, const Math::Camera& camera);
	void Add(GraphicsContext& gfx, EmitterRuntime* runtime, const float3& worldPosition, int sortKey = 0);
	void Render(GraphicsContext& gfx);

private:
	ParticleRenderMode m_Mode = ParticleRenderMode::EmitterList;
	bool               m_Culling = true;

	// SharedList
	ParticleRenderList m_TranslucentList;   // 반투명: 이미터 구분 없이 깊이 정렬
	ParticleRenderList m_AdditiveList;      // Additive: 정렬 없이 반투명 다음에. 블렌드 모드가 실행 중에 바뀌므로 같은 크기
	uint32_t           m_Reserved = 0;      // 두 목록의 크기 (파티클 수)
	uint32_t           m_Submitted = 0;     // 이번 프레임에 목록에 넣은 이미터 capacity 의 합
	bool               m_ListActive = false;

	// EmitterList / Sprite (SharedList 에서 목록에 못 들어간 이미터도 여기로)
	EmitterRenderList  m_EmitterList;
};
