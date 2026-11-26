#pragma once
#include "EmitterDrawOrder.h"
#include "EmitterRuntime.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

class ComputeContext;
class GraphicsContext;
namespace Math { class Camera; }

enum class EmitterPreset : uint32_t
{
	Basic,      // 지금 Main 의 기본 스택
	Fountain,   // AddVelocityInCone + GravityForce
	Minimal,    // SpawnRate + Init + Sphere + State 만 (Velocity 없음, Mass 는 trim 후보)
	Random,     // RandomRange / perComponent 입력값
	Count
};

const char* PresetName(EmitterPreset preset);
std::vector<ModuleDesc> MakePreset(EmitterPreset preset, float spawnRate);

constexpr uint32_t kMaxModuleCombos = 32;
uint32_t                ComboModuleBits(uint32_t combo);      // bit0 Sphere, 1 Cone, 2 Gravity, 3 Solve, 4 SubUV
std::string             ComboName(uint32_t combo);
std::vector<ModuleDesc> MakeComboStack(uint32_t combo, float spawnRate);

class EmitterTestScene
{
public:
	struct Settings
	{
		uint32_t count = 10;
		uint32_t capacityPerEmitter = 4096;   // 이미터에서 할당되는 파티클 개수, (capacity * N * 2)
		float    spawnRate = 400.f;           // 이미터마다 초당 생성 수. 살아 있는 수 ~= rate x 수명
		float    spacing = 2.f;               // 격자 간격 (m)
		uint32_t presetMask = (1u << uint32_t(EmitterPreset::Count)) - 1;   // 돌아가며 쓸 프리셋
		uint32_t comboCount = 0;

		// 비어 있지 않으면 프리셋 / 콤보 대신 이 에셋들을 돌아가며 배치 (Assets/Emitters/<이름>.emitter.json)
		//   spawnRate / presetMask / comboCount 는 쓰지 않는다 (에셋의 모듈 값 그대로)
		std::vector<std::string> assets;
		bool     assetCapacity = false;       // true = 에셋에 저장된 capacity, false = capacityPerEmitter (측정 조건을 맞출 때)
	};

	struct BuildResult
	{
		uint32_t requested = 0;
		uint32_t created = 0;
		double   setupMs = 0.0;      // N 개를 만드는 데 걸린 전체 시간 (버퍼 + IdleGPU 포함)
		double   shaderBuildMs = 0.0; // 그 중 셰이더 생성 + 컴파일 + PSO 생성 시간
		uint32_t shaderBuilds = 0;
	};

	EmitterTestScene() = default;
	EmitterTestScene(const EmitterTestScene&) = delete;
	EmitterTestScene& operator=(const EmitterTestScene&) = delete;

	bool Build(const Settings& settings);
	void Shutdown();

	void Tick(ComputeContext& ctx, float deltaTime);
	void OnSubmitted(uint64_t fence);
	void Render(GraphicsContext& gfx, const Math::Camera& camera);

	void SetStageMajor(bool stageMajor) { m_StageMajor = stageMajor; }
	bool StageMajor() const { return m_StageMajor; }

	// true = 모든 이미터의 파티클을 한 목록에서 정렬해서 한 번에 그림 / false = 이미터마다 따로 정렬, 따로 그림
	void SetGlobalSort(bool globalSort) { m_GlobalSort = globalSort; }
	bool GlobalSort() const { return m_GlobalSort; }

	// 전체 정렬 경로 전용. Gather 에서 화면 밖 / 투명 / 크기 0 파티클을 빼고 정렬
	void SetGatherCulling(bool enable) { m_GatherCulling = enable; }
	bool GatherCulling() const { return m_GatherCulling; }


	bool                Empty() const { return m_Items.empty(); }
	uint32_t            Count() const { return uint32_t(m_Items.size()); }
	uint32_t            AliveTotal() const;
	uint32_t            CapacityTotal() const;
	const Settings& AppliedSettings() const { return m_Settings; }
	const BuildResult& LastBuild() const { return m_LastBuild; }
	const std::string& LastError() const { return m_LastError; }

private:
	struct Item
	{
		std::unique_ptr<EmitterRuntime> runtime;
		OwnerTransform                  owner;
		EmitterPreset                   preset = EmitterPreset::Basic;
	};

	std::vector<Item> m_Items;
	Settings          m_Settings;
	BuildResult       m_LastBuild;
	std::string       m_LastError;

	bool			  m_StageMajor = false;
	std::vector<GpuEmitterInstance*> m_StagedScratch;

	bool               m_GlobalSort = true;
	bool               m_GatherCulling = true;
	EmitterDrawOrder   m_DrawOrder;       // 준 List: 이미터 사이의 그리는 순서
	ParticleRenderList m_RenderList;      // 반투명 (정렬). 크기 = 이미터 capacity 합 (Build 에서 잡음)
	ParticleRenderList m_AdditiveList;    // Additive (정렬 안 함). 블렌드 모드가 실행 중에 바뀌므로 같은 크기
};
