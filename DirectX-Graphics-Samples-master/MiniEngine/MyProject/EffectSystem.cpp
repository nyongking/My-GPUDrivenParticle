#include "pch.h"
#include "EffectSystem.h"

#include "CommandListManager.h"
#include "EmitterRenderList.h"
#include "GraphicsCore.h"
#include "Json.h"

#include <algorithm>

int EffectSystem::AddEmitter(const std::string& name, const std::vector<ModuleDesc>& modules, uint32_t capacity, EmitterBackend backend)
{
	Emitter e;
	e.name = name;
	e.runtime = std::make_unique<EmitterRuntime>();
	if (!e.runtime->Initialize(modules, capacity, backend))
	{
		m_LastError = "[System] '" + name + "': " + e.runtime->LastError();
		e.runtime->Shutdown();
		return -1;
	}
	m_Emitters.push_back(std::move(e));
	return static_cast<int>(m_Emitters.size()) - 1;
}

int EffectSystem::AddEmitter(const std::string& name, const EmitterAsset& asset, const OwnerTransform& local)
{
	Emitter e;
	e.name = name;
	e.local = local;
	e.runtime = std::make_unique<EmitterRuntime>();
	if (!InitializeEmitterFromAsset(*e.runtime, asset, 0))
	{
		m_LastError = "[System] '" + name + "': " + e.runtime->LastError();
		e.runtime->Shutdown();
		return -1;
	}
	m_Emitters.push_back(std::move(e));
	return static_cast<int>(m_Emitters.size()) - 1;
}

int EffectSystem::DuplicateEmitter(size_t index)
{
	if (index >= m_Emitters.size())
		return -1;
	const Emitter& src = m_Emitters[index];
	const EmitterAsset asset = CaptureEmitterAsset(*src.runtime, src.name);
	const int added = AddEmitter(src.name + " copy", asset, src.local);
	if (added < 0)
		return -1;
	m_Emitters[added].enabled = src.enabled;
	m_Emitters[added].sortKey = src.sortKey;
	Emitter moved = std::move(m_Emitters[added]);
	m_Emitters.erase(m_Emitters.begin() + added);
	m_Emitters.insert(m_Emitters.begin() + index + 1, std::move(moved));
	return static_cast<int>(index) + 1;
}

void EffectSystem::RemoveEmitter(size_t index)
{
	if (index >= m_Emitters.size())
		return;
	Retire(std::move(m_Emitters[index].runtime));
	m_Emitters.erase(m_Emitters.begin() + index);
}

void EffectSystem::MoveEmitter(size_t index, int direction)
{
	const int to = static_cast<int>(index) + (direction < 0 ? -1 : 1);
	if (index >= m_Emitters.size() || to < 0 || to >= static_cast<int>(m_Emitters.size()))
		return;
	std::swap(m_Emitters[index], m_Emitters[to]);
}

void EffectSystem::Clear()
{
	for (Emitter& e : m_Emitters)
		Retire(std::move(e.runtime));
	m_Emitters.clear();
}

void EffectSystem::Retire(std::unique_ptr<EmitterRuntime> runtime)
{
	if (runtime)
		m_Retired.push_back(std::move(runtime));
}

void EffectSystem::ApplyPending()
{
	if (!m_Retired.empty())
	{
		Graphics::g_CommandManager.IdleGPU();
		for (std::unique_ptr<EmitterRuntime>& r : m_Retired)
			r->Shutdown();
		m_Retired.clear();
	}

	// 블렌드 모드는 실행 중에 바뀔 수 있어서 두 목록 모두 전체 capacity 만큼 잡는다
	const uint32_t want = (m_Emitters.size() > 1) ? CapacityTotal() : 0u;
	if (want != m_ListCapacity)
	{
		Graphics::g_CommandManager.IdleGPU();
		if (want == 0)
		{
			m_RenderList.Shutdown();
			m_AdditiveList.Shutdown();
		}
		else
		{
			m_RenderList.Reserve(want);
			m_AdditiveList.Reserve(want);
		}
		m_ListCapacity = want;
	}

	for (Emitter& e : m_Emitters)
		e.runtime->ApplyPending();
}

void EffectSystem::Tick(ComputeContext& ctx, float deltaTime, const OwnerTransform& systemTransform)
{
	m_Ticked.clear();
	for (Emitter& e : m_Emitters)
	{
		if (!e.enabled)
			continue;
		const OwnerTransform world = ComposeOwnerTransform(systemTransform, e.local);
		e.worldPosition = world.position;       // 그릴 때 이미터 사이의 순서를 정하는 기준
		e.runtime->Tick(ctx, deltaTime, world);
		m_Ticked.push_back(e.runtime.get());
	}
}

void EffectSystem::OnSubmitted(uint64_t fence)
{
	for (EmitterRuntime* r : m_Ticked)
		r->OnSubmitted(fence);
}

void EffectSystem::Render(GraphicsContext& gfx, const Math::Camera& camera, EmitterRenderList& emitterList)
{
	// 두 가지 방식 (m_GlobalSort)
	//   공용 목록 : 모든 이미터의 파티클을 한 목록에 섞어 깊이 정렬 (머티리얼은 VM 인터프리터)
	//   준 List   : 이미터마다 따로 그리고 이미터 사이의 순서만 정한다 (머티리얼은 Preset / Custom 셰이더)
	const bool listReady = m_GlobalSort && m_Emitters.size() > 1 && m_ListCapacity == CapacityTotal() && m_ListCapacity > 0;
	if (!listReady)
	{
		// 준 List: 여기서는 그리지 않는다. 이미터를 목록에 넣기만 하고, 순서 정하기와 그리기는 EmitterRenderList 가 한다
		for (Emitter& e : m_Emitters)
		{
			if (e.enabled)
				emitterList.Add(e.runtime.get(), e.worldPosition, e.sortKey);
		}
		return;
	}

	// 공용 목록: 반투명은 이미터 구분 없이 깊이 정렬, Additive 는 모으기만 해서 그 뒤에 그린다
	//   (Additive 끼리는 순서가 결과에 영향이 없어서 정렬하지 않는다)
	m_RenderList.Begin(gfx, camera);
	m_AdditiveList.Begin(gfx, camera);
	for (Emitter& e : m_Emitters)
	{
		if (e.enabled)
			e.runtime->SubmitTo(gfx, e.runtime->IsAdditive() ? m_AdditiveList : m_RenderList);
	}
	m_RenderList.Render(gfx, true);
	m_AdditiveList.Render(gfx, false);
}

void EffectSystem::Shutdown()
{
	if (m_Emitters.empty() && m_Retired.empty() && m_ListCapacity == 0)
		return;
	Graphics::g_CommandManager.IdleGPU();
	for (Emitter& e : m_Emitters)
		e.runtime->Shutdown();
	m_Emitters.clear();
	for (std::unique_ptr<EmitterRuntime>& r : m_Retired)
		r->Shutdown();
	m_Retired.clear();
	m_Ticked.clear();
	m_RenderList.Shutdown();
	m_AdditiveList.Shutdown();
	m_ListCapacity = 0;
}

uint32_t EffectSystem::AliveTotal() const
{
	uint32_t total = 0;
	for (const Emitter& e : m_Emitters)
		total += e.runtime->AliveCount();
	return total;
}

uint32_t EffectSystem::CapacityTotal() const
{
	uint32_t total = 0;
	for (const Emitter& e : m_Emitters)
		total += e.runtime->Capacity();
	return total;
}

SystemAsset EffectSystem::Capture(const std::string& name) const
{
	SystemAsset a;
	a.name = name;
	a.globalSort = m_GlobalSort;
	for (const Emitter& e : m_Emitters)
	{
		SystemEmitterAsset se;
		se.name = e.name;
		se.enabled = e.enabled;
		se.sortKey = e.sortKey;
		se.local = e.local;
		se.emitter = CaptureEmitterAsset(*e.runtime, e.name);
		a.emitters.push_back(std::move(se));
	}
	return a;
}

bool EffectSystem::Load(const SystemAsset& asset, std::vector<std::string>* failures)
{
	Clear();
	m_GlobalSort = asset.globalSort;
	bool ok = true;
	for (const SystemEmitterAsset& se : asset.emitters)
	{
		const int added = AddEmitter(se.name, se.emitter, se.local);
		if (added < 0)
		{
			ok = false;
			if (failures)
				failures->push_back(m_LastError);
			continue;
		}
		m_Emitters[added].enabled = se.enabled;
		m_Emitters[added].sortKey = se.sortKey;
	}
	return ok;
}

namespace
{
	const char* const kSystemFormat = "MyProject.System";
	const char* const kSystemDirectory = "Assets/Systems/";
	const char* const kSystemExtension = ".system.json";

	JsonValue Floats(const float* v, uint32_t count)
	{
		JsonValue a = JsonValue::MakeArray();
		for (uint32_t i = 0; i < count; ++i)
			a.Push(JsonValue(v[i]));
		return a;
	}

	void ReadFloats(const JsonValue* v, float* out, uint32_t count, const std::string& what, std::vector<std::string>& warnings)
	{
		if (v == nullptr)
			return;
		if (!v->IsArray() || v->Size() != count)
		{
			warnings.push_back(what + ": expected " + std::to_string(count) + " numbers -> kept default");
			return;
		}
		for (uint32_t i = 0; i < count; ++i)
			out[i] = static_cast<float>(v->At(i).AsNumber(out[i]));
	}
}

std::string SystemAssetToJson(const SystemAsset& a)
{
	JsonValue root = JsonValue::MakeObject();
	root.Set("format", JsonValue(kSystemFormat));
	root.Set("version", JsonValue(SystemAsset::kVersion));
	root.Set("name", JsonValue(a.name));
	root.Set("globalSort", JsonValue(a.globalSort));

	JsonValue emitters = JsonValue::MakeArray();
	for (const SystemEmitterAsset& se : a.emitters)
	{
		JsonValue je = JsonValue::MakeObject();
		je.Set("name", JsonValue(se.name));
		je.Set("enabled", JsonValue(se.enabled));
		je.Set("sortKey", JsonValue(se.sortKey));
		je.Set("position", Floats(&se.local.position.x, 3));
		je.Set("rotation", Floats(&se.local.rotation.x, 4));
		je.Set("scale", Floats(&se.local.scale.x, 3));
		je.Set("emitter", EmitterAssetToJsonValue(se.emitter));
		emitters.Push(std::move(je));
	}
	root.Set("emitters", std::move(emitters));
	return WriteJson(root);
}

bool SystemAssetFromJson(const std::string& text, SystemAsset& out, std::string& error, std::vector<std::string>& warnings)
{
	JsonValue root;
	if (!ParseJson(text, root, error))
		return false;
	if (!root.IsObject())
	{
		error = "root is not an object";
		return false;
	}
	const JsonValue* format = root.Find("format");
	if (format == nullptr || format->AsString() != kSystemFormat)
	{
		error = std::string("not an effect system (format must be \"") + kSystemFormat + "\")";
		return false;
	}
	const JsonValue* version = root.Find("version");
	if (version != nullptr && version->AsNumber() > double(SystemAsset::kVersion))
	{
		error = "file version " + std::to_string(int(version->AsNumber())) + " is newer than this build ("
			+ std::to_string(SystemAsset::kVersion) + ")";
		return false;
	}

	SystemAsset a;
	if (const JsonValue* v = root.Find("name"))
		a.name = v->AsString();
	if (const JsonValue* v = root.Find("globalSort"))
		a.globalSort = v->AsBool(true);

	const JsonValue* emitters = root.Find("emitters");
	if (emitters == nullptr || !emitters->IsArray())
	{
		error = "\"emitters\" must be an array";
		return false;
	}
	for (size_t i = 0; i < emitters->Size(); ++i)
	{
		const JsonValue& je = emitters->At(i);
		const std::string where = "emitters[" + std::to_string(i) + "]";
		SystemEmitterAsset se;
		if (const JsonValue* v = je.Find("name"))
			se.name = v->AsString();
		if (se.name.empty())
			se.name = "Emitter " + std::to_string(i);
		if (const JsonValue* v = je.Find("enabled"))
			se.enabled = v->AsBool(true);
		if (const JsonValue* v = je.Find("sortKey"))
			se.sortKey = static_cast<int>(v->AsNumber(0.0));
		ReadFloats(je.Find("position"), &se.local.position.x, 3, where + ".position", warnings);
		ReadFloats(je.Find("rotation"), &se.local.rotation.x, 4, where + ".rotation", warnings);
		ReadFloats(je.Find("scale"), &se.local.scale.x, 3, where + ".scale", warnings);

		const JsonValue* emitter = je.Find("emitter");
		std::string emitterError;
		std::vector<std::string> emitterWarnings;
		if (emitter == nullptr || !EmitterAssetFromJsonValue(*emitter, se.emitter, emitterError, emitterWarnings))
		{
			warnings.push_back(where + " '" + se.name + "': " + (emitter ? emitterError : std::string("missing \"emitter\"")) + " -> skipped");
			continue;
		}
		for (const std::string& w : emitterWarnings)
			warnings.push_back(where + " '" + se.name + "': " + w);
		a.emitters.push_back(std::move(se));
	}

	out = std::move(a);
	return true;
}

bool SaveSystemAsset(const std::string& path, const SystemAsset& asset, std::string& error)
{
	return WriteAssetTextFile(path, SystemAssetToJson(asset), error);
}

bool LoadSystemAsset(const std::string& path, SystemAsset& out, std::string& error, std::vector<std::string>& warnings)
{
	std::string text;
	if (!ReadAssetTextFile(path, text, error))
		return false;
	return SystemAssetFromJson(text, out, error, warnings);
}

const char* SystemAssetDirectory() { return kSystemDirectory; }

std::string SystemAssetPath(const std::string& name)
{
	return std::string(kSystemDirectory) + name + kSystemExtension;
}

std::vector<std::string> ListSystemAssets()
{
	return ListAssetFiles(kSystemDirectory, kSystemExtension);
}
