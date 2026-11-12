#include "pch.h"
#include "EmitterRuntime.h"

#include "EmitterCompiler.h"

#include "CommandListManager.h"
#include "GraphicsCore.h"

#include "UberGpuProgram.h"

#include <algorithm>
#include <utility>

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
	m_Modules = modules;
	m_AppliedModules = modules;
	m_Capacity = capacity;
	m_Backend = backend;
	m_ActiveBackend = backend;
	m_Ready = false;

	m_Pending = Pending::Restart;
	ApplyPending();
	return m_Ready;
}

void EmitterRuntime::Shutdown()
{
	Graphics::g_CommandManager.IdleGPU();     // GPU 가 버퍼를 다 쓴 뒤 지운다
	ShutdownActive();
	m_CpuRenderer.ReleaseTextures();
	m_GpuRenderer.ReleaseTextures();
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
	else
	{
		p.value.resize(size * 2);   // 현재 값을 Min, Max 양쪽에 복사
		std::memcpy(p.value.data() + size, p.value.data(), size);
	}
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

void EmitterRuntime::SetBindingAttribute(size_t bindingIndex, const std::string& attribute)
{
	RenderBinding& b = m_SpriteDesc.bindings[bindingIndex];
	if (b.attribute == attribute)
		return;

	// 칸 연결이 바뀌면 Trim 대상(레이아웃)과 렌더러 offset 이 바뀐다.
	// 레이아웃이 같아도 렌더러가 offset 을 다시 읽어야 하므로 Preserve 가 아니라 Restart
	b.attribute = attribute;
	Request(Pending::Restart);
}

void EmitterRuntime::SetBindingDefault(size_t bindingIndex, const std::vector<uint8_t>& value)
{
	RenderBinding& b = m_SpriteDesc.bindings[bindingIndex];
	if (value.size() != b.type->size)
		return;

	b.defaultValue = value;
	m_AppliedSpriteDesc.bindings[bindingIndex].defaultValue = value;   // 재컴파일과 무관한 값

	// 상수 버퍼 값이라 바로 반영 (실행 중이 아닌 쪽도 같이 맞춰 둔다)
	m_CpuRenderer.SetDefaults(m_SpriteDesc);
	m_GpuRenderer.SetDefaults(m_SpriteDesc);
}

bool EmitterRuntime::SetSpriteTexture(const std::string& path)
{
	m_SpriteTexture = path;

	// 텍스처는 레이아웃과 무관 -> 재시작 없이 두 렌더러에 바로 (실행 중이 아닌 쪽도 맞춰 둔다)
	const std::wstring wpath = Utility::UTF8ToWideString(path);
	const bool cpuOk = m_CpuRenderer.SetTexture(wpath);
	const bool gpuOk = m_GpuRenderer.SetTexture(wpath);
	return cpuOk && gpuOk;
}

void EmitterRuntime::SetSpriteSettings(const SpriteSettings& settings)
{
	// 상수 버퍼 / 루트 상수 값이라 바로 반영 (실행 중이 아닌 쪽도 맞춰 둔다)
	m_CpuRenderer.SetSettings(settings);
	m_GpuRenderer.SetSettings(settings);
}

std::vector<std::string> EmitterRuntime::BindableAttributes(const TypeInfo* type) const
{
	std::vector<std::string> out;
	auto add = [&](const std::string& name)
		{
			if (std::find(out.begin(), out.end(), name) == out.end())
				out.push_back(name);
		};

	// 적용된 스택에서 누가 "쓰는" 속성만 (안 쓰는 속성은 연결해도 기본값이 됨)
	for (const ModuleDesc& m : m_AppliedModules)
	{
		for (const Usage& u : m.writes)
		{
			const AttributeKey k = AttributeKey::Parse(u.attr);
			if (k.ns != Namespace::Particles || k.mod != Modifier::None || u.type != type)
				continue;

			add(k.Full());
			add("Particles.Previous." + k.name);   // 엔진이 복사해서 채움
			add("Particles.Initial." + k.name);
		}
	}
	return out;
}

void EmitterRuntime::ApplyPending()
{
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
		m_LastError.clear();
	}
	else
	{
		// 새 구성이 잘못됨: 실행 중인 이미터는 건드리지 않았으므로 UI 만 되돌린다
		// (컴파일 오류 문구에 한글이 있어 ImGui 에서는 깨질 수 있으므로 출력 창에도 남긴다)
		Utility::Print(m_LastError.c_str());
		m_Modules = m_AppliedModules;
		m_SpriteDesc = m_AppliedSpriteDesc;
		m_Backend = m_ActiveBackend;
	}
}


bool EmitterRuntime::BuildProgram(const CompiledEmitter& compiled, std::unique_ptr<EmitterGpuProgram>& out)
{
	if (!m_ShaderCompiler.Ready() && !m_ShaderCompiler.Initialize(ShaderCompiler::FindIncludeDir(L"ModuleCommon.hlsli")))
	{
		m_LastError = "ShaderCompiler initialize failed\n" + m_ShaderCompiler.Log();
		return false;
	}

	static bool s_UberLayoutChecked = false;
	if (!s_UberLayoutChecked)
	{
		std::string log;
		UberGpuProgram::SetLayoutVerified(CheckUberShaderLayout(m_ShaderCompiler, log));
		UberGpuProgram::Initialize();
		Utility::Print(log.c_str());
		s_UberLayoutChecked = true;
	}

	std::unique_ptr<EmitterGpuProgram> program = std::make_unique<EmitterGpuProgram>();
	const bool built = program->Build(compiled, m_ShaderCompiler, "Main");
	Utility::Print(program->Log().c_str());
	if (!built)
	{
		m_LastError = program->Log();
		return false;
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
		std::unique_ptr<EmitterGpuProgram> program;
		if (!BuildProgram(next, program))
			return false;

		Graphics::g_CommandManager.IdleGPU();
		m_Gpu.Rebind(next, *program);
		m_GpuProgram = std::move(program);      // 이전 프로그램은 Rebind 뒤에 해제
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

	std::unique_ptr<EmitterGpuProgram> program;
	if (m_Backend == EmitterBackend::Gpu && !BuildProgram(next, program))
		return false;

	// 여기까지 성공해야 실행 중인 이미터를 내린다
	Graphics::g_CommandManager.IdleGPU();
	ShutdownActive();

	if (m_Backend == EmitterBackend::Cpu)
	{
		m_Ready = m_Cpu.Initialize(m_Modules, { m_SpriteDesc }, m_Capacity)
			&& m_CpuRenderer.Initialize(m_Cpu, m_SpriteDesc);
	}
	else
	{
		m_GpuProgram = std::move(program);
		m_Ready = m_Gpu.Initialize(next, *m_GpuProgram, m_Capacity)
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


uint32_t EmitterRuntime::AliveCount() const
{
	if (!m_Ready)
		return 0;
	return m_ActiveBackend == EmitterBackend::Cpu ? m_Cpu.Count() : m_Gpu.ReadbackAlive();
}
