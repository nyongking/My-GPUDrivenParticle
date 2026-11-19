#include "pch.h"
#include "EmitterTestScene.h"

#include "EmitterAsset.h"
#include "ModuleLibrary.h"

#include "CommandListManager.h"
#include "GraphicsCore.h"
#include "SystemTime.h"

#include <algorithm>
#include <cmath>

const char* PresetName(EmitterPreset preset)
{
	switch (preset)
	{
	case EmitterPreset::Basic:    return "Basic";
	case EmitterPreset::Fountain: return "Fountain";
	case EmitterPreset::Minimal:  return "Minimal";
	case EmitterPreset::Random:   return "Random";
	default:                      return "?";
	}
}

std::vector<ModuleDesc> MakePreset(EmitterPreset preset, float spawnRate)
{
	switch (preset)
	{
	case EmitterPreset::Fountain:
		return
		{
			SpawnRate(spawnRate),
			InitializeParticle(0.05f, 1.f, 2.f, float3{ 0.f, 0.f, 0.f }, float4{ 0.4f, 0.7f, 1.f, 1.f }),
			AddVelocityInCone(5.f, 20.f, float3{ 0.f, 1.f, 0.f }),
			ParticleState(true),
			GravityForce(float3{ 0.f, -9.8f, 0.f }),
			SolveForceAndVelocity(20.f, 100.f)
		};

	case EmitterPreset::Minimal:
		return
		{
			SpawnRate(spawnRate),
			InitializeParticle(0.08f, 1.f, 1.f, float3{ 0.f, 0.f, 0.f }, float4{ 1.f, 0.5f, 0.2f, 1.f }),
			SphereLocation(0.5f, false),
			ParticleState(true)
		};

	case EmitterPreset::Random:
		return
		{
			SpawnRate(spawnRate),
			InitializeParticle(
				RandomRange<float>(0.05f, 0.2f),
				1.f,
				RandomRange<float>(0.5f, 2.f),
				float3{ 0.f, 0.f, 0.f },
				RandomRange<float4>(float4{ 1.f, 0.2f, 0.2f, 1.f }, float4{ 0.2f, 0.2f, 1.f, 1.f }, true)),
			SphereLocation(RandomRange<float>(0.1f, 0.6f), true),
			ParticleState(true),
			SolveForceAndVelocity(10.f, 100.f)
		};

	case EmitterPreset::Basic:
	default:
		return
		{
			SpawnRate(spawnRate),
			InitializeParticle(0.1f, 1.f, 1.f, RandomRange<float3>(float3{ 0.3f, 0.1f, 0.f }, float3{ -0.3f, -0.1f, 0.f }, true), float4{ 1.f, 1.f, 1.f, 1.f }),
			SphereLocation(0.1f, true),
			ParticleState(true),
			SolveForceAndVelocity(10.f, 100.f),
			SubUVAnimation(0.f, 63.f)
		};
	}
}

uint32_t ComboModuleBits(uint32_t combo)
{
	// 13 은 32 와 서로소라 0~31 을 한 번씩 다 지나는 순서가 된다. 0 번이 "전부 켬"이 되도록 31 을 더함
	return (combo % kMaxModuleCombos * 13u + 31u) % kMaxModuleCombos;
}

std::string ComboName(uint32_t combo)
{
	const uint32_t bits = ComboModuleBits(combo);
	std::string name = "Combo" + std::to_string(combo) + " [";
	const char* const kNames[] = { "Sphere", "Cone", "Gravity", "Solve", "SubUV" };
	bool first = true;
	for (uint32_t b = 0; b < 5; ++b)
		if (bits & (1u << b)) { name += (first ? "" : "+"); name += kNames[b]; first = false; }
	return name + (first ? "none]" : "]");
}

std::vector<ModuleDesc> MakeComboStack(uint32_t combo, float spawnRate)
{
	const uint32_t bits = ComboModuleBits(combo);

	// 조합마다 다른 색 (화면에서 어느 조합인지 구분용)
	const float h = float(bits) / float(kMaxModuleCombos);
	const float kTwoPi = 6.2831853f;
	const float4 color = { 0.5f + 0.5f * std::cos(kTwoPi * h), 0.5f + 0.5f * std::cos(kTwoPi * (h + 0.333f)),
		0.5f + 0.5f * std::cos(kTwoPi * (h + 0.667f)), 1.f };

	// 모듈은 공유 셰이더의 고정 순서대로 넣는다 (순서 경고가 나지 않게)
	std::vector<ModuleDesc> s;
	s.push_back(SpawnRate(spawnRate));
	s.push_back(InitializeParticle(0.08f, 1.f, 1.5f, float3{ 0.f, 0.f, 0.f }, color));
	if (bits & (1u << 0)) s.push_back(SphereLocation(0.4f, false));
	if (bits & (1u << 1)) s.push_back(AddVelocityInCone(3.f, 25.f, float3{ 0.f, 1.f, 0.f }));
	s.push_back(ParticleState(true));
	if (bits & (1u << 2)) s.push_back(GravityForce(float3{ 0.f, -4.9f, 0.f }));
	if (bits & (1u << 3)) s.push_back(SolveForceAndVelocity(10.f, 100.f));
	if (bits & (1u << 4)) s.push_back(SubUVAnimation(0.f, 63.f));
	return s;
}

bool EmitterTestScene::Build(const Settings& settings)
{
	Shutdown();
	m_Settings = settings;
	m_LastBuild = BuildResult();
	m_LastBuild.requested = settings.count;
	m_LastError.clear();

	// 쓸 프리셋 목록 (마스크가 비어 있으면 Basic 하나)
	std::vector<EmitterPreset> presets;
	for (uint32_t i = 0; i < uint32_t(EmitterPreset::Count); ++i)
		if (settings.presetMask & (1u << i))
			presets.push_back(EmitterPreset(i));
	if (presets.empty())
		presets.push_back(EmitterPreset::Basic);

	// 격자 배치: cols x rows, 원점 중심
	const uint32_t cols = (std::max)(1u, uint32_t(std::ceil(std::sqrt(float(settings.count)))));
	const uint32_t rows = (settings.count + cols - 1) / cols;
	const float    x0 = -0.5f * float(cols - 1) * settings.spacing;
	const float    z0 = -0.5f * float(rows - 1) * settings.spacing;

	EmitterGpuProgram::ResetBuildStats();
	const int64_t startTick = SystemTime::GetCurrentTick();

	// 에셋: 파일은 한 번씩만 읽어 두고 돌아가며 쓴다 (읽기 실패한 에셋만 빼고 계속)
	const bool useAssets = !settings.assets.empty();
	std::vector<EmitterAsset> assets;
	for (const std::string& name : settings.assets)
	{
		EmitterAsset asset;
		std::string error;
		std::vector<std::string> warnings;
		if (!LoadEmitterAsset(EmitterAssetPath(name), asset, error, warnings))
		{
			m_LastError = "[TestScene] asset " + name + " 실패: " + error + "\n";
			Utility::Print(m_LastError.c_str());
			continue;
		}
		for (const std::string& w : warnings)
			Utility::Printf("[TestScene] asset %s: %s\n", name.c_str(), w.c_str());
		asset.name = name;   // 라벨 = 파일 이름
		assets.push_back(std::move(asset));
	}
	const uint32_t buildCount = (useAssets && assets.empty()) ? 0u : settings.count;

	m_Items.reserve(buildCount);
	for (uint32_t i = 0; i < buildCount; ++i)
	{
		Item item;
		item.preset = presets[i % presets.size()];
		const bool useCombo = settings.comboCount > 0;
		const uint32_t combo = useCombo ? i % (std::min)(settings.comboCount, kMaxModuleCombos) : 0;
		const EmitterAsset* asset = useAssets ? &assets[i % assets.size()] : nullptr;
		const std::string label = asset ? asset->name : useCombo ? ComboName(combo) : std::string(PresetName(item.preset));
		item.owner.position = { x0 + float(i % cols) * settings.spacing, 0.f, z0 + float(i / cols) * settings.spacing };
		item.runtime = std::make_unique<EmitterRuntime>();

		bool ok = false;
		if (asset)
		{
			const uint32_t capacity = (settings.assetCapacity && asset->capacity > 0) ? asset->capacity : settings.capacityPerEmitter;
			ok = InitializeEmitterFromAsset(*item.runtime, *asset, capacity);
		}
		else
		{
			const std::vector<ModuleDesc> stack = useCombo ? MakeComboStack(combo, settings.spawnRate) : MakePreset(item.preset, settings.spawnRate);
			ok = item.runtime->Initialize(stack, settings.capacityPerEmitter, EmitterBackend::Gpu);
		}
		if (!ok)
		{
			m_LastError = std::string("[TestScene] ") + label + " #" + std::to_string(i) + " 실패\n" + item.runtime->LastError();
			Utility::Print(m_LastError.c_str());
			item.runtime->Shutdown();
			continue;   // 실패한 것만 빼고 계속 (어떤 프리셋이 문제인지 로그로 확인)
		}
		m_Items.push_back(std::move(item));
	}

	// 공용 렌더 목록: 모든 이미터가 꽉 차도 들어가는 크기 (Shutdown 에서 GPU idle 을 거쳤으므로 다시 만들어도 안전)
	uint32_t listCapacity = 0;
	for (const Item& item : m_Items)
		listCapacity += item.runtime->Capacity();
	if (!m_Items.empty())
	{
		m_RenderList.Reserve(listCapacity);
		m_AdditiveList.Reserve(listCapacity);
	}

	m_LastBuild.created = uint32_t(m_Items.size());
	m_LastBuild.setupMs = SystemTime::TimeBetweenTicks(startTick, SystemTime::GetCurrentTick()) * 1000.0;
	m_LastBuild.shaderBuildMs = EmitterGpuProgram::GetStats().buildMs;
	m_LastBuild.shaderBuilds = EmitterGpuProgram::GetStats().builds;

	Utility::Printf("[TestScene] %u / %u emitters, setup %.1f ms (shader build %.1f ms, %u builds)\n",
		m_LastBuild.created, m_LastBuild.requested, m_LastBuild.setupMs, m_LastBuild.shaderBuildMs, m_LastBuild.shaderBuilds);

	return m_LastBuild.created == m_LastBuild.requested;
}

void EmitterTestScene::Shutdown()
{
	if (m_Items.empty())
		return;

	Graphics::g_CommandManager.IdleGPU();   // 한 번만 기다리고 전부 내린다
	for (Item& item : m_Items)
		item.runtime->Shutdown();
	m_Items.clear();
	m_RenderList.Shutdown();
	m_AdditiveList.Shutdown();
}

void EmitterTestScene::Tick(ComputeContext& ctx, float deltaTime)
{
	// Emitter order: 이미터 하나의 모든 단계를 끝낸 뒤 다음 이미터 (단계마다 배리어 -> 디스패치가 차례로 실행)
	//   Stage order 와 같은 단계 함수(GpuEmitterInstance::Tick 안의 BeginTick / StageBarriers / StageWork / EndTick)를 쓰고 루프 순서만 다르다
	if (!m_StageMajor)
	{
		for (Item& item : m_Items)
		{
			item.runtime->ApplyPending();
			item.runtime->Tick(ctx, deltaTime, item.owner);
		}
		return;
	}

	// Stage order: 단계마다 모든 이미터 (같은 단계끼리는 독립이라 배리어를 모아 걸고 디스패치를 연달아 넣는다)
	std::vector<GpuEmitterInstance*>& gpus = m_StagedScratch;
	gpus.clear();
	for (Item& item : m_Items)
	{
		item.runtime->ApplyPending();
		if (GpuEmitterInstance* gpu = item.runtime->StagedGpu())
		{
			if (gpu->BeginTick(deltaTime, item.owner))
				gpus.push_back(gpu);
		}
		else
		{
			item.runtime->Tick(ctx, deltaTime, item.owner);
		}
	}

	for (uint32_t s = 0; s < uint32_t(GpuSimStage::End); ++s)
	{
		const GpuSimStage stage = GpuSimStage(s);
		for (GpuEmitterInstance* gpu : gpus)
			gpu->StageBarriers(ctx, stage);
		for (GpuEmitterInstance* gpu : gpus)
			gpu->StageWork(ctx, stage);
	}

	for (GpuEmitterInstance* gpu : gpus)
		gpu->EndTick();


}

void EmitterTestScene::OnSubmitted(uint64_t fence)
{
	for (Item& item : m_Items)
		item.runtime->OnSubmitted(fence);
}

void EmitterTestScene::Render(GraphicsContext& gfx, const Math::Camera& camera)
{
	if (m_Items.empty())
		return;

	// 전체 정렬: 모든 이미터 파티클을 한 목록에 모아 정렬 1번 + Draw 1번 (이미터 사이 앞뒤도 맞음)
	if (m_GlobalSort && m_RenderList.ReservedCapacity() > 0)
	{
		// Additive 끼리는 그리는 순서가 결과에 영향이 없으므로 정렬 목록에서 뺀다
		const bool splitAdditive = m_ExcludeAdditive && m_AdditiveList.ReservedCapacity() > 0;

		m_RenderList.SetCulling(m_GatherCulling);
		m_RenderList.Begin(gfx, camera);
		if (splitAdditive)
		{
			m_AdditiveList.SetCulling(m_GatherCulling);
			m_AdditiveList.Begin(gfx, camera);
		}

		for (Item& item : m_Items)
		{
			const bool additive = item.runtime->GetSpriteSettings().blend == SpriteBlendMode::Additive;
			item.runtime->SubmitTo(gfx, (splitAdditive && additive) ? m_AdditiveList : m_RenderList);
		}

		m_RenderList.Render(gfx, true);
		if (splitAdditive)
			m_AdditiveList.Render(gfx, false);
		return;
	}

	// 이미터별: 각자 정렬해서 각자 그림 (이미터 사이 순서 = 그리는 순서)
	for (Item& item : m_Items)
		item.runtime->Render(gfx, camera);
}

uint32_t EmitterTestScene::AliveTotal() const
{
	uint32_t sum = 0;
	for (const Item& item : m_Items)
		sum += item.runtime->AliveCount();
	return sum;
}

uint32_t EmitterTestScene::CapacityTotal() const
{
	return uint32_t(m_Items.size()) * m_Settings.capacityPerEmitter;
}
