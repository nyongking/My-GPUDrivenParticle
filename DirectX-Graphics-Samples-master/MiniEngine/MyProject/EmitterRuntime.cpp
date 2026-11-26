#include "pch.h"
#include "EmitterRuntime.h"

#include "EmitterCompiler.h"

#include "CommandListManager.h"
#include "GraphicsCore.h"

#include "UberGpuProgram.h"

#include <algorithm>
#include <unordered_map>
#include <utility>

namespace
{
	std::unordered_map<std::string, std::weak_ptr<EmitterGpuProgram>> s_ProgramCache;
	bool		s_ProgramCacheEnabled = true;
	uint32_t	s_ProgramCacheHits = 0;
}

void EmitterRuntime::SetProgramCacheEnabled(bool enabled) { s_ProgramCacheEnabled = enabled; }
bool EmitterRuntime::ProgramCacheEnabled() { return s_ProgramCacheEnabled; }
uint32_t EmitterRuntime::ProgramCacheHits() { return s_ProgramCacheHits; }


namespace
{
	CompiledEmitter CompileStack(const std::vector<ModuleDesc>& modules, const RendererDesc& sprite)
	{
		EmitterCompiler compiler;
		for (const ModuleDesc& m : modules)
			compiler.Add(m);
		compiler.Add(sprite);
		return compiler.Compile();
	}

	std::string JoinLines(const std::vector<std::string>& lines)
	{
		std::string out;
		for (const std::string& l : lines)
			out += l + "\n";
		return out;
	}

	bool SameBlockLayout(const Block& a, const Block& b)
	{
		if (a.Stride() != b.Stride() || a.Slots().size() != b.Slots().size())
			return false;

		for (size_t i = 0; i < a.Slots().size(); ++i)
		{
			const Slot& x = a.Slots()[i];
			const Slot& y = b.Slots()[i];
			if (x.offset != y.offset || x.type != y.type || x.key.Full() != y.key.Full())
				return false;
		}
		return true;
	}

	// 살아 있는 파티클을 그대로 쓸 수 있는지. 모듈 입력값(ModuleParams)은 달라도 된다
	bool SameParticleLayout(const CompiledEmitter& a, const CompiledEmitter& b)
	{
		return SameBlockLayout(a.particles, b.particles) && SameBlockLayout(a.emitterParams, b.emitterParams);
	}
}

bool EmitterRuntime::Initialize(const std::vector<ModuleDesc>& modules, uint32_t capacity, EmitterBackend backend)
{
	m_CpuRenderer.SetMaterial(&m_Material);
	m_GpuRenderer.SetMaterial(&m_Material);

	m_Modules = modules;
	m_AppliedModules = modules;
	m_Capacity = capacity;
	m_AppliedCapacity = capacity;
	m_Backend = backend;
	m_ActiveBackend = backend;
	m_Ready = false;

	m_Pending = Pending::Restart;
	ApplyPending();
	return m_Ready;
}

bool EmitterRuntime::Initialize(const std::vector<ModuleDesc>& modules, const RendererDesc& sprite, uint32_t capacity, EmitterBackend backend)
{
	if (sprite.bindings.size() == m_SpriteDesc.bindings.size())
	{
		m_SpriteDesc = sprite;
		m_AppliedSpriteDesc = sprite;
	}
	return Initialize(modules, capacity, backend);
}

void EmitterRuntime::Shutdown()
{
	Graphics::g_CommandManager.IdleGPU();     // GPU 가 버퍼를 다 쓴 뒤 지운다
	ShutdownActive();

	m_Material.ReleaseTextures();

	SetMaterialProgram(nullptr);
	m_MaterialProgramTried = false;
	m_MaterialProgramError.clear();

	m_ShaderCompiler.Shutdown();
	m_Ready = false;
}

void EmitterRuntime::Tick(ComputeContext& ctx, float deltaTime, const OwnerTransform& owner)
{
	if (!m_Ready)
		return;

	if (m_ActiveBackend == EmitterBackend::Cpu)
		m_Cpu.Tick(deltaTime, owner);
	else
		m_Gpu.Tick(ctx, deltaTime, owner);
}

void EmitterRuntime::Tick(ComputeContext& ctx, float deltaTime, const float3& ownerPosition)
{
	OwnerTransform owner;
	owner.position = ownerPosition;
	Tick(ctx, deltaTime, owner);
}

void EmitterRuntime::Render(GraphicsContext& gfx, const Math::Camera& camera)
{
	if (!m_Ready)
		return;

	if (m_ActiveBackend == EmitterBackend::Cpu)
		m_CpuRenderer.Render(gfx, camera, m_Cpu);
	else
		m_GpuRenderer.RenderGpu(gfx, camera, m_Gpu);
}

void EmitterRuntime::SubmitTo(GraphicsContext& gfx, ParticleRenderList& list)
{
	if (!m_Ready)
		return;

	if (m_ActiveBackend == EmitterBackend::Cpu)
		m_CpuRenderer.SubmitToList(gfx, list, m_Cpu);
	else
		m_GpuRenderer.SubmitToListGpu(gfx, list, m_Gpu);
}

void EmitterRuntime::OnSubmitted(uint64_t fence)
{
	if (m_Ready && m_ActiveBackend == EmitterBackend::Gpu)
		m_Gpu.OnSubmitted(fence);
}

void EmitterRuntime::ShutdownActive()
{
	m_CpuRenderer.Shutdown();
	m_Cpu.Shutdown();

	m_GpuRenderer.Shutdown();
	m_Gpu.Shutdown();
	m_GpuProgram.reset();
}

void EmitterRuntime::Request(Pending pending)
{
	if (pending > m_Pending)
		m_Pending = pending;
}

void EmitterRuntime::NotifyValuesChanged()
{
	Request(Pending::Values);
}

void EmitterRuntime::SetParamKind(size_t moduleIndex, size_t paramIndex, ParamKind kind)
{
	ParamDesc& p = m_Modules[moduleIndex].params[paramIndex];
	if (p.kind == kind)
		return;

	const size_t size = p.type->size;
	p.kind = kind;
	if (kind == ParamKind::Constant)
	{
		p.value.resize(size);       // Min 을 상수로 유지
		p.perComponent = false;
	}
	else if (kind == ParamKind::Curve)
	{
		p.value.resize(size);       // 상수 값은 남겨 둔다 (상수로 되돌릴 때 사용)
		p.perComponent = false;
		if (!p.curve)
		{
			// 처음 커브로 바꿀 때: 현재 상수 값을 양 끝 키로 (바꾼 직후에는 결과가 그대로)
			CurveKey key;
			key.value = float4{ 0.f, 0.f, 0.f, 0.f };
			std::memcpy(&key.value, p.value.data(), (std::min)(size, sizeof(float4)));
			key.time = 0.f;
			std::vector<CurveKey> keys = { key };
			key.time = 1.f;
			keys.push_back(key);
			p.curve = std::make_shared<CurveAsset>(keys);
		}
	}
	else
	{
		p.value.resize(size * 2);   // 현재 값을 Min, Max 양쪽에 복사
		std::memcpy(p.value.data() + size, p.value.data(), size);
	}
	// Curve 는 가로축 속성(NormalizedAge) 읽기가 늘어 레이아웃이 바뀔 수 있다 -> RebuildPreserve 가 재시작으로 넘긴다
	Request(Pending::Preserve);
}

void EmitterRuntime::SetPerComponent(size_t moduleIndex, size_t paramIndex, bool perComponent)
{
	ParamDesc& p = m_Modules[moduleIndex].params[paramIndex];
	if (p.perComponent == perComponent)
		return;

	p.perComponent = perComponent;
	Request(Pending::Preserve);
}

int EmitterRuntime::FindNeighbor(size_t moduleIndex, int direction) const
{
	const Stage stage = m_Modules[moduleIndex].stage;
	for (int i = static_cast<int>(moduleIndex) + direction; i >= 0 && i < static_cast<int>(m_Modules.size()); i += direction)
		if (m_Modules[i].stage == stage)
			return i;
	return -1;
}

bool EmitterRuntime::CanMoveModule(size_t moduleIndex, int direction) const
{
	return FindNeighbor(moduleIndex, direction) >= 0;
}

void EmitterRuntime::MoveModule(size_t moduleIndex, int direction)
{
	const int neighbor = FindNeighbor(moduleIndex, direction);
	if (neighbor < 0)
		return;

	std::swap(m_Modules[moduleIndex], m_Modules[neighbor]);
	Request(Pending::Restart);
}

void EmitterRuntime::AddModule(const ModuleDesc& module)
{
	m_Modules.push_back(module);
	DetachCurves(m_Modules.back());     // 모듈 목록의 원본과 커브를 같이 쓰지 않게
	Request(Pending::Restart);
}

void EmitterRuntime::RemoveModule(size_t moduleIndex)
{
	if (moduleIndex >= m_Modules.size())
		return;
	m_Modules.erase(m_Modules.begin() + moduleIndex);
	Request(Pending::Restart);
}

void EmitterRuntime::ReplaceDescription(const std::vector<ModuleDesc>& modules, const RendererDesc& sprite, uint32_t capacity, EmitterBackend backend)
{
	m_Modules = modules;
	// 바인딩 칸은 SpriteRenderer::Desc() 순서로 고정 (렌더러가 번호로 읽는다). 모양이 다르면 지금 것을 유지
	if (sprite.bindings.size() == m_SpriteDesc.bindings.size())
		m_SpriteDesc = sprite;
	if (capacity > 0)
		m_Capacity = capacity;
	m_Backend = backend;
	Request(Pending::Restart);
}

void EmitterRuntime::SetBackend(EmitterBackend backend)
{
	if (m_Backend == backend)
		return;

	m_Backend = backend;
	Request(Pending::Restart);
}

void EmitterRuntime::Restart()
{
	Request(Pending::Restart);
}

void EmitterRuntime::SetBindingDefault(size_t bindingIndex, const std::vector<uint8_t>& value)
{
	RenderBinding& b = m_SpriteDesc.bindings[bindingIndex];
	if (value.size() != b.type->size)
		return;

	b.defaultValue = value;
	m_AppliedSpriteDesc.bindings[bindingIndex].defaultValue = value;   // 재컴파일과 무관한 값

	ApplyRenderDefaults();
}

void EmitterRuntime::ApplyRenderDefaults()
{
	// Facing / Alignment 벡터: VS 의 상수 버퍼 값이라 바로 반영 (실행 중이 아닌 쪽도 같이 맞춰 둔다)
	m_CpuRenderer.SetDefaults(m_SpriteDesc);
	m_GpuRenderer.SetDefaults(m_SpriteDesc);

	// 렌더 구조체의 기본값: 시뮬레이션이 새 파티클을 이 값에서 시작한다 (이미 살아 있는 파티클은 그대로)
	const RenderParticleData defaults = SpriteRenderer::RenderDefaults(m_SpriteDesc);
	m_Cpu.SetRenderDefaults(defaults);
	m_Gpu.SetRenderDefaults(defaults);
}

bool EmitterRuntime::SetMaterialTexture(uint32_t slot, const MaterialTexture& texture)
{
	return m_Material.SetTexture(slot, texture);
}

void EmitterRuntime::SetMaterialParam(uint32_t k, const float4& value)
{
	return m_Material.SetParam(k, value);
}

void EmitterRuntime::SetMaterialEffect(ParticleMaterial::Effect effect)
{
	m_Material.SetEffect(effect);
}

bool EmitterRuntime::AddMaterialLayer(ParticleMaterial::Effect effect)
{
	return m_Material.AddLayer(effect);
}

bool EmitterRuntime::RemoveMaterialLayer(uint32_t index)
{
	return m_Material.RemoveLayer(index);
}

bool EmitterRuntime::MoveMaterialLayer(uint32_t index, int direction)
{
	return m_Material.MoveLayer(index, direction);
}

bool EmitterRuntime::SetMaterialLayers(const std::vector<ParticleMaterial::Effect>& layers)
{
	return m_Material.SetLayers(layers);
}

bool EmitterRuntime::SetMaterialLayerEffect(uint32_t index, ParticleMaterial::Effect effect)
{
	return m_Material.SetLayerEffect(index, effect);
}

void EmitterRuntime::ConvertMaterialLayersToGraph()
{
	if (!m_Material.HasCustomGraph() && m_Material.Layers().size() > 1)
		m_Material.ConvertLayersToCustomGraph();
}

void EmitterRuntime::SetMaterialGraph(const MaterialGraph& graph, bool liveEdit)
{
	m_Material.SetCustomGraph(graph);
	if (liveEdit)
		m_MaterialLiveEdit = true;
	else
		EndMaterialLiveEdit();
}

void EmitterRuntime::EndMaterialLiveEdit()
{
	if (!m_MaterialLiveEdit)
		return;
	m_MaterialLiveEdit = false;
	m_MaterialProgramTried = false;		// 다음 SyncMaterialProgram 에서 지금 값으로 셰이더를 만든다
}

void EmitterRuntime::SetMaterialGraphLayout(const MaterialGraph& graph)
{
	m_Material.SetCustomGraphLayout(graph);
}

void EmitterRuntime::ClearMaterialGraph()
{
	m_Material.ClearCustomGraph();
}

void EmitterRuntime::SetMaterialUseVM(bool use)
{
	m_Material.SetUseVM(use);
}

void EmitterRuntime::SetMaterialProgramSource(ParticleMaterial::ProgramSource source)
{
	m_Material.SetProgramSource(source);
}


void EmitterRuntime::SetSpriteSettings(const SpriteSettings& settings)
{
	// 상수 버퍼 / 루트 상수 값이라 바로 반영 (실행 중이 아닌 쪽도 맞춰 둔다)
	m_CpuRenderer.SetSettings(settings);
	m_GpuRenderer.SetSettings(settings);
}


void EmitterRuntime::ApplyPending()
{
	SyncProgramWithMode();
	SyncMaterialProgram();

	const Pending pending = m_Pending;
	m_Pending = Pending::None;

	if (pending == Pending::None)
		return;

	if (pending == Pending::Values)
	{
		FillModuleParamData(m_Modules, m_Compiled);
		FillUberParams(m_Modules, m_Uber.params, nullptr);
		m_AppliedModules = m_Modules;

		if (m_Ready)
		{
			if (m_ActiveBackend == EmitterBackend::Cpu)
				m_Cpu.SetModuleParamData(m_Compiled.moduleParamData);
			else
			{
				m_Gpu.SetModuleParamData(m_Compiled.moduleParamData);
				m_Gpu.SetUberData(m_Uber);
			}
		}
		return;
	}

	const bool ok = (pending == Pending::Preserve) ? RebuildPreserve() : RebuildRestart();
	if (ok)
	{
		m_AppliedModules = m_Modules;
		m_AppliedSpriteDesc = m_SpriteDesc;
		m_AppliedCapacity = m_Capacity;
		m_LastError.clear();
	}
	else
	{
		Utility::Print(m_LastError.c_str());
		m_Modules = m_AppliedModules;
		m_SpriteDesc = m_AppliedSpriteDesc;
		m_Capacity = m_AppliedCapacity;
		m_Backend = m_ActiveBackend;
	}
}


bool EmitterRuntime::BuildProgram(const CompiledEmitter& compiled, std::shared_ptr<EmitterGpuProgram>& out)
{
	std::string cacheKey;
	if (s_ProgramCacheEnabled)
	{
		const GeneratedShader generated = GenerateEmitterShader(compiled, "Main.generated.hlsl");
		if (generated.Ok())
		{
			cacheKey = generated.source;
			auto it = s_ProgramCache.find(cacheKey);
			if (it != s_ProgramCache.end())
			{
				if (std::shared_ptr<EmitterGpuProgram> cached = it->second.lock())
				{
					++s_ProgramCacheHits;
					out = std::move(cached);
					return true;
				}
			}
		}
	}

	if (!m_ShaderCompiler.Ready() && !m_ShaderCompiler.Initialize(ShaderCompiler::FindIncludeDir(L"ModuleCommon.hlsli")))
	{
		m_LastError = "ShaderCompiler initialize failed\n" + m_ShaderCompiler.Log();
		return false;
	}


	std::shared_ptr<EmitterGpuProgram> program = std::make_shared<EmitterGpuProgram>();
	const bool built = program->Build(compiled, m_ShaderCompiler, "Main");
	Utility::Print(program->Log().c_str());
	if (!built)
	{
		m_LastError = program->Log();
		return false;
	}

	if (!cacheKey.empty())
	{
		for (auto it = s_ProgramCache.begin(); it != s_ProgramCache.end(); )
			it = it->second.expired() ? s_ProgramCache.erase(it) : std::next(it);
		s_ProgramCache[cacheKey] = program;
	}

	out = std::move(program);
	return true;
}

bool EmitterRuntime::RebuildPreserve()
{
	CompiledEmitter next = CompileStack(m_Modules, m_SpriteDesc);
	if (!next.Ok())
	{
		m_LastError = JoinLines(next.errors);
		return false;
	}

	if (!m_Ready || !SameParticleLayout(next, m_Compiled))
		return RebuildRestart();

	if (m_ActiveBackend == EmitterBackend::Cpu)
	{
		m_Cpu.ReplaceCompiled(next);
	}
	else
	{
		EnsureUberProgram();
		std::shared_ptr<EmitterGpuProgram> program;
		if (NeedGeneratedProgram(BuildUberEmitterData(m_Modules, next)) && !BuildProgram(next, program))
			return false;

		Graphics::g_CommandManager.IdleGPU();
		m_Gpu.Rebind(next, program.get());
		m_GpuProgram = std::move(program);
	}

	m_Compiled = std::move(next);
	RebuildUberData();
	return true;
}

bool EmitterRuntime::RebuildRestart()
{
	CompiledEmitter next = CompileStack(m_Modules, m_SpriteDesc);
	if (!next.Ok())
	{
		m_LastError = JoinLines(next.errors);
		return false;
	}

	std::shared_ptr<EmitterGpuProgram> program;
	if (m_Backend == EmitterBackend::Gpu)
	{
		EnsureUberProgram();
		if (NeedGeneratedProgram(BuildUberEmitterData(m_Modules, next)) && !BuildProgram(next, program))
			return false;
	}

	// 여기까지 성공해야 실행 중인 이미터를 내린다
	Graphics::g_CommandManager.IdleGPU();
	ShutdownActive();

	// 새 파티클의 렌더 값 (CPU 이미터는 Initialize 에서 버퍼를 이 값으로 채운다)
	ApplyRenderDefaults();

	if (m_Backend == EmitterBackend::Cpu)
	{
		m_Ready = m_Cpu.Initialize(m_Modules, { m_SpriteDesc }, m_Capacity)
			&& m_CpuRenderer.Initialize(m_Cpu, m_SpriteDesc);
	}
	else
	{
		m_GpuProgram = std::move(program);
		m_Ready = m_Gpu.Initialize(next, m_GpuProgram.get(), m_Capacity)
			&& m_GpuRenderer.InitializeGpu(m_Gpu, m_SpriteDesc);
	}

	m_ActiveBackend = m_Backend;
	m_Compiled = std::move(next);
	RebuildUberData();

	if (!m_Ready)
		m_LastError = "Emitter resources could not be created (see Output window)\n";
	return m_Ready;
}

void EmitterRuntime::RebuildUberData()
{
	m_Uber = BuildUberEmitterData(m_Modules, m_Compiled);
	if (!m_Uber.warnings.empty() || !m_Uber.errors.empty())
		Utility::Print(DescribeUber(m_Uber).c_str());

	if (m_ActiveBackend == EmitterBackend::Gpu)
		m_Gpu.SetUberData(m_Uber);
}

bool EmitterRuntime::NeedGeneratedProgram(const UberEmitterData& uber) const
{
	return UberGpuProgram::Mode() == GpuSimMode::Generated || !UberGpuProgram::Ready() || !uber.Ok();
}

void EmitterRuntime::EnsureUberProgram()
{
	static bool s_Done = false;
	if (s_Done)
		return;

	// 배치 검사에 DXC 반사가 필요하다. 실패하면 Uber 는 준비 안 됨 -> 생성 방식으로 실행 (BuildProgram 이 오류를 남김)
	if (!m_ShaderCompiler.Ready() && !m_ShaderCompiler.Initialize(ShaderCompiler::FindIncludeDir(L"ModuleCommon.hlsli")))
		return;

	std::string log;
	UberGpuProgram::SetLayoutVerified(CheckUberShaderLayout(m_ShaderCompiler, log));
	UberGpuProgram::Initialize();
	Utility::Print(log.c_str());
	s_Done = true;
}

void EmitterRuntime::SyncProgramWithMode()
{
	if (!m_Ready || m_ActiveBackend != EmitterBackend::Gpu || m_Pending >= Pending::Preserve)
		return;     // 재빌드가 예정돼 있으면 그쪽에서 맞춘다

	const bool need = NeedGeneratedProgram(m_Uber);
	if (need && !m_GpuProgram)
	{
		// Uber -> Generated: 생성 방식 셰이더를 만든다 (레이아웃이 같으므로 파티클은 유지)
		Request(Pending::Preserve);
	}
	else if (!need && m_GpuProgram)
	{
		// Generated -> Uber: 생성 방식 PSO 를 내린다. 지난 프레임이 쓰던 PSO 이므로 GPU 를 기다린 뒤에
		Graphics::g_CommandManager.IdleGPU();
		m_Gpu.Rebind(m_Compiled, nullptr);
		m_GpuProgram.reset();
	}
}


const std::string& EmitterRuntime::GeneratedMaterialSource() const
{
	static const std::string kEmpty;
	return m_MaterialProgram ? m_MaterialProgram->Source() : kEmpty;
}

void EmitterRuntime::SetMaterialProgram(std::shared_ptr<MaterialGpuProgram> program)
{
	// PSO 자체는 MiniEngine 이 끝까지 갖고 있으므로, 지난 프레임이 쓰던 것을 바꿔도 GPU 를 기다릴 필요가 없다
	m_MaterialProgram = std::move(program);
	m_MaterialPreset = false;

	const GraphicsPSO* pso = m_MaterialProgram ? &m_MaterialProgram->PSO() : nullptr;
	m_CpuRenderer.SetMaterialPSO(pso);
	m_GpuRenderer.SetMaterialPSO(pso);
}

void EmitterRuntime::SyncMaterialProgram()
{
	if (MaterialGpuProgram::Mode() != MaterialShaderMode::Generated)
	{
		// Generated -> Interpreted: 기본 PS 로 돌아간다
		if (m_MaterialProgram || m_MaterialProgramTried)
		{
			SetMaterialProgram(nullptr);
			m_MaterialProgramTried = false;
			m_MaterialProgramError.clear();
		}
		return;
	}

	// ---- 값을 드래그하는 중: 값이 셰이더에 숫자로 들어가므로 매 프레임 소스가 달라진다. 놓을 때까지 만들지 않는다 ----
	//   VM 프로그램이 있으면 인터프리터로 그려서 값이 바로 보이게 한다 (값을 MaterialParams 로 읽는다).
	//   VM 한도를 넘는 그래프는 인터프리터로 그릴 수 없으므로, 놓을 때까지 직전 셰이더(직전 값)로 그린다
	if (m_MaterialLiveEdit && m_Material.GetMode() == ParticleMaterial::Mode::Custom)
	{
		if (m_Material.ProgramError().empty() && (m_MaterialProgram || m_MaterialPreset))
			SetMaterialProgram(nullptr);
		m_MaterialProgramTried = false;
		return;
	}
	m_MaterialLiveEdit = false;

	// 그래프가 그대로면 할 일 없음 (값을 원래대로 되돌린 경우처럼 소스가 같으면 캐시에서 바로 찾는다)
	const uint32_t revision = m_Material.GraphRevision();
	if (m_MaterialProgramTried && revision == m_MaterialProgramRevision)
		return;

	m_MaterialProgramTried = true;
	m_MaterialProgramRevision = revision;
	m_MaterialProgramError.clear();

	// ---- Preset: 효과 하나 -> 그 효과의 고정 셰이더 (빌드할 때 컴파일된 것, 여기서 만들 것이 없다) ----
	if (m_Material.GetMode() == ParticleMaterial::Mode::Preset && m_Material.Layers().size() <= 1)
	{
		SetMaterialProgram(nullptr);

		const GraphicsPSO* pso = &SpriteRenderer::PresetPSO(m_Material.GetEffect());
		m_CpuRenderer.SetMaterialPSO(pso);
		m_GpuRenderer.SetMaterialPSO(pso);
		m_MaterialPreset = true;
		return;
	}

	// ---- Custom: 그래프에서 셰이더를 만든다 (이미 만든 것이 있으면 그것을 쓴다) ----
	const std::string name = "Main";
	const GeneratedShader generated = GenerateMaterialShader(m_Material.CurrentGraph(), name + ".material.generated.hlsl");

	std::string log;
	std::shared_ptr<MaterialGpuProgram> program;
	if (generated.Ok())
		program = MaterialGpuProgram::Find(generated.source);   // 이미 만든 셰이더가 있으면 그대로 쓴다 (컴파일러 불필요)

	if (!program)
	{
		if (generated.Ok() && !m_ShaderCompiler.Ready()
			&& !m_ShaderCompiler.Initialize(ShaderCompiler::FindIncludeDir(L"ModuleCommon.hlsli")))
		{
			log = "ShaderCompiler initialize failed\n" + m_ShaderCompiler.Log();
		}
		else
		{
			program = MaterialGpuProgram::Acquire(generated, m_ShaderCompiler, name, log);
			if (program)
				Utility::Print(log.c_str());
		}
	}

	if (!program)
	{
		// 실패: 기본 PS (VM) 로 그린다. 그래프를 고치면 revision 이 올라가서 다시 시도한다
		m_MaterialProgramError = "[Generated material] build failed -> drawing with the interpreter\n" + log;
		Utility::Print(m_MaterialProgramError.c_str());
	}

	SetMaterialProgram(std::move(program));
}

uint32_t EmitterRuntime::AliveCount() const
{
	if (!m_Ready)
		return 0;
	return m_ActiveBackend == EmitterBackend::Cpu ? m_Cpu.Count() : m_Gpu.ReadbackAlive();
}
