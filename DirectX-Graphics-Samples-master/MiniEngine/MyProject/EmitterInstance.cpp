#include "pch.h"
#include "EmitterInstance.h"
#include "EmitterCompiler.h"
#include "ModuleContext.h"

#include "TagLibrary.h"

#include <algorithm>

namespace
{
	uint32_t HashFrame(uint32_t x)
	{
		x ^= x >> 16; x *= 0x7feb352dU;
		x ^= x >> 15; x *= 0x846ca68bU;
		x ^= x >> 16;
		return x;
	}
}

bool EmitterInstance::Initialize(const std::vector<ModuleDesc>& modules,
	const std::vector<RendererDesc>& renders,
	uint32_t capacity)
{
	m_EmitterUpdates.clear();
	m_ParticleSpawns.clear();
	m_ParticleUpdates.clear();
#ifndef RELEASE
	m_Log.clear();
#endif
	EmitterCompiler compiler;
	for (size_t moduleIndex = 0; moduleIndex < modules.size(); ++moduleIndex)
	{
		ModuleDesc mod = modules[moduleIndex];
		mod.compiledIndex = static_cast<int>(moduleIndex);
		compiler.Add(mod);

		switch (mod.stage)
		{
		case Stage::EmitterUpdate:
			m_EmitterUpdates.push_back(mod);
			break;
		case Stage::ParticleSpawn:
			m_ParticleSpawns.push_back(mod);
			break;
		case Stage::ParticleUpdate:
			m_ParticleUpdates.push_back(mod);
			break;
		default:
#ifndef RELEASE
			m_Log += "[경고] '" + mod.name + "' (" + StageName(mod.stage) + ") 단계는 아직 실행하지 않음\n";
#endif
			break;
		}
	}

	for (auto& mod : renders)
	{
		compiler.Add(mod);
	}

	m_CompiledEmitter = compiler.Compile();

#ifndef RELEASE
	m_Log = DescribeCompiled(m_CompiledEmitter, "EmitterInstance") + m_Log;
#endif	
	if (!m_CompiledEmitter.Ok())
		return false;

	m_Capacity = capacity;
	const size_t bytes = size_t(m_CompiledEmitter.particles.Stride()) * capacity;
	m_ParticleData.assign((bytes + 15) / 16 * 16, 0);	// RoundUp16
	m_EmitterData.assign(m_CompiledEmitter.emitterParams.Stride(), 0);
	m_AliveFlags.assign(capacity, 1);


	m_Count = m_Frame = m_NewBegin = m_NewEnd = m_KilledLastTick = 0;
	return true;
}

void EmitterInstance::Tick(float deltaTime, const float3& ownerPosition)
{
	// -------------------
	//		Tick
	// -------------------

	// [1] EngineValues
	m_EngineValues.DeltaTime = min(max(deltaTime, 0.f), float(kMaxDeltaTime));	// 멈췄다 돌아온 프레임에서 한 번에 너무 많이 생성되지 않게
	m_EngineValues.OwnerPosition = ownerPosition;
	m_EngineValues.RandomSeed = HashFrame(m_Frame + 1);

	// [2] Check SpawnCount
	uint32_t* spawnCount = nullptr;
	if (const Slot* slot = m_CompiledEmitter.emitterParams.Find(Emitter::SpawnCount))
	{
		spawnCount = reinterpret_cast<uint32_t*>(m_EmitterData.data() + slot->offset);
		*spawnCount = 0;
	}

	// [3] Run Module - Emitter Update
	RunModules(m_EmitterUpdates, 0, 0);

	m_KilledLastTick = 0;
	if (m_Count > 0 && !m_ParticleUpdates.empty())
	{
		// [4] 일단 전부 살아있다고 표시
		std::fill(m_AliveFlags.begin(), m_AliveFlags.begin() + m_Count, uint8_t(1));

		// [5] Previous <- 현재 (위치 등을 바꾸기 전에)
		ApplyCopyOps(m_CompiledEmitter.copyAtUpdateStart, 0, m_Count);

		// [6] Particle Update: 기존 파티클 전부. 모듈은 ctx.Kill(i) 로 죽음 표시만 한다
		RunModules(m_ParticleUpdates, 0, m_Count, m_AliveFlags.data());

		// [7] Compaction
		m_KilledLastTick = CompactAlive();
	}

	// [4] 새칸확보
	const uint32_t want = spawnCount ? *spawnCount : 0;
	const uint32_t n = min(want, m_Capacity - m_Count);
	const uint32_t begin = m_Count;
	const uint32_t end = m_Count + n;

	m_Count = end;

	if (n > 0)
	{
		ZeroRange(begin, end);
		RunModules(m_ParticleSpawns, begin, end);
		ApplyCopyOps(m_CompiledEmitter.copyAtSpawnEnd, begin, end);
	}

	m_NewBegin = begin;
	m_NewEnd = end;
	++m_Frame;
}

void EmitterInstance::RunModules(std::vector<ModuleDesc>& modules, uint32_t begin, uint32_t end,
	uint8_t* aliveFlags)
{
	ModuleContext ctx(m_CompiledEmitter, m_ParticleData.data(), m_EmitterData.data(),
		m_EngineValues, m_Capacity, begin, end, aliveFlags);

	for (ModuleDesc& m : modules)
		if (m.execute)          // 본문이 없는 모듈은 건너뛴다
		{
			ctx.BeginModule(m.compiledIndex);
			m.execute(ctx);
		}
}

void EmitterInstance::ZeroRange(uint32_t begin, uint32_t end)
{
	for (auto& s : m_CompiledEmitter.particles.Slots())
	{
		const uint32_t size = s.type->size;
		uint8_t* column = m_ParticleData.data() + size_t(s.offset) * m_Capacity;
		std::memset(column + size_t(begin) * size, 0, size_t(end - begin) * size);
	}
}

void EmitterInstance::ApplyCopyOps(const std::vector<CopyOp>& ops, uint32_t begin, uint32_t end)
{
	for (const CopyOp& op : ops)
	{
		const uint8_t* src = m_ParticleData.data() + size_t(op.srcOffset) * m_Capacity + size_t(begin) * op.size;
		uint8_t* dst = m_ParticleData.data() + size_t(op.dstOffset) * m_Capacity + size_t(begin) * op.size;
		std::memcpy(dst, src, size_t(end - begin) * op.size);
	}
}

void EmitterInstance::CopyParticles(uint32_t from, uint32_t to)
{
	for (auto& s : m_CompiledEmitter.particles.Slots())
	{
		const uint32_t size = s.type->size;
		uint8_t* column = m_ParticleData.data() + size_t(s.offset) * m_Capacity;
		std::memcpy(column + size_t(to) * size, column + size_t(from) * size, size);
	}
}

uint32_t EmitterInstance::CompactAlive()
{
	uint32_t write = 0;
	for (uint32_t read = 0; read < m_Count; ++read)
	{
		if (!m_AliveFlags[read])
			continue;

		if (write != read)
			CopyParticles(read, write);
		++write;
	}

	const uint32_t killed = m_Count - write;
	m_Count = write;
	return killed;
}
