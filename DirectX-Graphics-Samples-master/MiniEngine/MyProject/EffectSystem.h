#pragma once

#include "EmitterAsset.h"
#include "OwnerTransform.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

class ComputeContext;
class ParticleRenderer;
class GraphicsContext;
namespace Math { class Camera; }

struct SystemEmitterAsset
{
	std::string    name;
	bool           enabled = true;
	int            sortKey = 0;     // 준 List: 클수록 나중에 = 위에 그려진다
	OwnerTransform local;
	EmitterAsset   emitter;
};

struct SystemAsset
{
	static const uint32_t kVersion = 1;

	std::string                     name;
	std::vector<SystemEmitterAsset> emitters;
};

std::string SystemAssetToJson(const SystemAsset& asset);
bool        SystemAssetFromJson(const std::string& text, SystemAsset& out, std::string& error, std::vector<std::string>& warnings);
bool        SaveSystemAsset(const std::string& path, const SystemAsset& asset, std::string& error);
bool        LoadSystemAsset(const std::string& path, SystemAsset& out, std::string& error, std::vector<std::string>& warnings);

const char*              SystemAssetDirectory();
std::string              SystemAssetPath(const std::string& name);
std::vector<std::string> ListSystemAssets();

// 이미터 여러 개를 모아 둔 것 (JSON 으로 저장 / 로드). 시뮬레이션은 이미터마다 돌리고, 그리기는 ParticleRenderer 에 맡긴다
class EffectSystem
{
public:
	struct Emitter
	{
		std::string                     name;
		bool                            enabled = true;
		// 렌더러에 넘기는 정렬 키 (EmitterList 방식에서 쓰인다): 클수록 나중에 = 더 위에 그려진다
		int                             sortKey = 0;
		OwnerTransform                  local;
		float3                          worldPosition = { 0.f, 0.f, 0.f };   // 마지막 Tick 의 월드 위치 (순서 계산용)
		std::unique_ptr<EmitterRuntime> runtime;
	};

	EffectSystem() = default;
	~EffectSystem() { Shutdown(); }
	EffectSystem(const EffectSystem&) = delete;
	EffectSystem& operator=(const EffectSystem&) = delete;

	int  AddEmitter(const std::string& name, const std::vector<ModuleDesc>& modules, uint32_t capacity, EmitterBackend backend);
	int  AddEmitter(const std::string& name, const EmitterAsset& asset, const OwnerTransform& local = OwnerTransform());
	int  DuplicateEmitter(size_t index);
	void RemoveEmitter(size_t index);
	void MoveEmitter(size_t index, int direction);
	void Clear();

	size_t         Count() const { return m_Emitters.size(); }
	Emitter&       At(size_t index) { return m_Emitters[index]; }
	const Emitter& At(size_t index) const { return m_Emitters[index]; }

	void ApplyPending();
	void Tick(ComputeContext& ctx, float deltaTime, const OwnerTransform& systemTransform);
	void OnSubmitted(uint64_t fence);
	// 그리지 않는다. 켜져 있는 이미터를 렌더러에 넘기기만 한다 (어떤 방식으로 그릴지는 ParticleRenderer 가 정한다)
	void Submit(GraphicsContext& gfx, ParticleRenderer& renderer);
	void Shutdown();

	uint32_t AliveTotal() const;
	uint32_t CapacityTotal() const;
	const std::string& LastError() const { return m_LastError; }

	SystemAsset Capture(const std::string& name) const;
	bool        Load(const SystemAsset& asset, std::vector<std::string>* failures);

private:
	std::vector<Emitter>                          m_Emitters;
	std::vector<std::unique_ptr<EmitterRuntime>>  m_Retired;
	std::vector<EmitterRuntime*>                  m_Ticked;

	std::string        m_LastError;

	void Retire(std::unique_ptr<EmitterRuntime> runtime);
};
