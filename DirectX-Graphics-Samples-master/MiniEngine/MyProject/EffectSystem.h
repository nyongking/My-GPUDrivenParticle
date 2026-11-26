#pragma once

#include "EmitterAsset.h"
#include "OwnerTransform.h"
#include "ParticleRenderList.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

class ComputeContext;
class EmitterRenderList;
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
	bool                            globalSort = true;
	std::vector<SystemEmitterAsset> emitters;
};

std::string SystemAssetToJson(const SystemAsset& asset);
bool        SystemAssetFromJson(const std::string& text, SystemAsset& out, std::string& error, std::vector<std::string>& warnings);
bool        SaveSystemAsset(const std::string& path, const SystemAsset& asset, std::string& error);
bool        LoadSystemAsset(const std::string& path, SystemAsset& out, std::string& error, std::vector<std::string>& warnings);

const char*              SystemAssetDirectory();
std::string              SystemAssetPath(const std::string& name);
std::vector<std::string> ListSystemAssets();

class EffectSystem
{
public:
	struct Emitter
	{
		std::string                     name;
		bool                            enabled = true;
		// EmitterRenderList 에 넘기는 정렬 키: 클수록 나중에 = 더 위에 그려진다 (EmitterRenderList.h)
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
	// 준 List (GlobalSort 가 꺼져 있거나 이미터 1개): 그리지 않고 emitterList 에 넣기만 한다 -> 그리는 것은 목록의 주인이 한다
	// 공용 목록 (GlobalSort)                        : 여기서 바로 그린다
	void Render(GraphicsContext& gfx, const Math::Camera& camera, EmitterRenderList& emitterList);
	void Shutdown();

	void SetGlobalSort(bool enable) { m_GlobalSort = enable; }
	bool GlobalSort() const { return m_GlobalSort; }

	uint32_t AliveTotal() const;
	uint32_t CapacityTotal() const;
	const std::string& LastError() const { return m_LastError; }

	SystemAsset Capture(const std::string& name) const;
	bool        Load(const SystemAsset& asset, std::vector<std::string>* failures);

private:
	std::vector<Emitter>                          m_Emitters;
	std::vector<std::unique_ptr<EmitterRuntime>>  m_Retired;
	std::vector<EmitterRuntime*>                  m_Ticked;

	bool               m_GlobalSort = true;
	ParticleRenderList m_RenderList;       // 반투명: 이미터 구분 없이 깊이 정렬
	ParticleRenderList m_AdditiveList;     // Additive: 정렬 없이 반투명 다음에 그린다
	uint32_t           m_ListCapacity = 0;
	std::string        m_LastError;

	void Retire(std::unique_ptr<EmitterRuntime> runtime);
};
