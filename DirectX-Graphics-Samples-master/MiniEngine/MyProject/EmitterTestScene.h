#pragma once
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
};
