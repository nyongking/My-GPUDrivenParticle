#include "pch.h"
#include "EmitterRuntime.h"

#include "EmitterCompiler.h"

#include "CommandListManager.h"
#include "GraphicsCore.h"

#include <utility>

namespace
{
	CompiledEmitter CompileStack(const std::vector<ModuleDesc>& modules)
	{
		EmitterCompiler compiler;
		for (const ModuleDesc& m : modules)
			compiler.Add(m);
		compiler.Add(SpriteRenderer::Desc());
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
	m_ShaderCompiler.Shutdown();
	m_Ready = false;
}

void EmitterRuntime::Tick(float deltaTime, const OwnerTransform& owner)
{
	if (!m_Ready)
		return;

	if (m_ActiveBackend == EmitterBackend::Cpu)
		m_Cpu.Tick(deltaTime, owner);
	else
		m_Gpu.Tick(deltaTime, owner);
}

void EmitterRuntime::Tick(float deltaTime, const float3& ownerPosition)
{
	OwnerTransform owner;
	owner.position = ownerPosition;
	Tick(deltaTime, owner);
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

void EmitterRuntime::ApplyPending()
{
	const Pending pending = m_Pending;
	m_Pending = Pending::None;

	if (pending == Pending::None)
		return;

	if (pending == Pending::Values)
	{
		FillModuleParamData(m_Modules, m_Compiled);
		m_AppliedModules = m_Modules;

		if (m_Ready)
		{
			if (m_ActiveBackend == EmitterBackend::Cpu)
				m_Cpu.SetModuleParamData(m_Compiled.moduleParamData);
			else
				m_Gpu.SetModuleParamData(m_Compiled.moduleParamData);
		}
		return;
	}

	const bool ok = (pending == Pending::Preserve) ? RebuildPreserve() : RebuildRestart();
	if (ok)
	{
		m_AppliedModules = m_Modules;
		m_LastError.clear();
	}
	else
	{
		// 새 구성이 잘못됨: 실행 중인 이미터는 건드리지 않았으므로 UI 만 되돌린다
		// (컴파일 오류 문구에 한글이 있어 ImGui 에서는 깨질 수 있으므로 출력 창에도 남긴다)
		Utility::Print(m_LastError.c_str());
		m_Modules = m_AppliedModules;
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
	CompiledEmitter next = CompileStack(m_Modules);
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
	return true;
}

bool EmitterRuntime::RebuildRestart()
{
	CompiledEmitter next = CompileStack(m_Modules);
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
		m_Ready = m_Cpu.Initialize(m_Modules, { SpriteRenderer::Desc() }, m_Capacity)
			&& m_CpuRenderer.Initialize(m_Cpu);
	}
	else
	{
		m_GpuProgram = std::move(program);
		m_Ready = m_Gpu.Initialize(next, *m_GpuProgram, m_Capacity)
			&& m_GpuRenderer.InitializeGpu(m_Gpu);
	}

	m_ActiveBackend = m_Backend;
	m_Compiled = std::move(next);

	if (!m_Ready)
		m_LastError = "Emitter resources could not be created (see Output window)\n";
	return m_Ready;
}


uint32_t EmitterRuntime::AliveCount() const
{
	if (!m_Ready)
		return 0;
	return m_ActiveBackend == EmitterBackend::Cpu ? m_Cpu.Count() : m_Gpu.ReadbackAlive();
}
