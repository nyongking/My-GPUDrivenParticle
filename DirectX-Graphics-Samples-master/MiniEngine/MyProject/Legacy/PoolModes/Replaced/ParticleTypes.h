#pragma once
#include <cstdint>
#include <DirectXMath.h>

namespace Particles
{
	//-------------------------
	//		For Buffer
	//-------------------------
	struct Particle
	{
		DirectX::XMFLOAT3 position;	float life;
		DirectX::XMFLOAT3 velocity; float maxlife;
		uint32_t		  color;	float size;
		uint32_t		  flags;	uint32_t pad;
	};	
	static_assert(sizeof(Particle) == 48, "HLSL Particle stride(48 B)와 일치해야 함");

	
	// CounterBuffer에서 각 값들의 바이트 위치
	enum CounterOffset : uint32_t
	{
		kAliveCount = 0,
		kDeadCount = 4,
		kEmitCount = 8,
		kAliveAfterSim = 12,
		kCounterBufferSize = 16
	};

	enum IndirectArgsOffset : uint32_t
	{
		kSimulateDispatchArgs = 0,
		kDrawArgs = 16,
		kIndirectArgsSize = 32,
	};

	// CounterBuffer에 사용될 값
	struct alignas(16) CounterSnapshot
	{
		uint32_t aliveCount;	// 현재 살아있는 파티클 수
		uint32_t deadCount;		// 죽은 파티클 수
		uint32_t emitCount;		// 
		uint32_t aliveAfterSim;
	};
	
	struct alignas(16) Simconstants
	{
		DirectX::XMFLOAT3 emitterPos;	float deltaTime;
		DirectX::XMFLOAT3 gravity;		uint32_t emitCount;
		float minLife; float maxLife; float minSpeed; float maxSpeed;
		float particleSize; uint32_t maxParticles; uint32_t randomSeed; uint32_t baseColor;
	};
	static_assert(sizeof(Simconstants) == 64, "HLSL cbuffer SimConstants(64 B)와 일치해야 함");

	// 파티클 렌더링에 필요한 Constant Buffer
	struct alignas(16) DrawConstants
	{
		DirectX::XMFLOAT4X4 viewProj;
		DirectX::XMFLOAT4	camRight;
		DirectX::XMFLOAT4	camUp;
	};

	enum class Mode : uint32_t
	{
		NONE = 0,
		Default = 1,
		SlotReuse = 2,
		SlotReuseFlags = 3,
		AliveDirect = 4,
		AliveIndirect = 5,
		AliveIndirectWave = 6,
		Compaction = 7,
	};

	inline bool IsDefault(Mode m) { return m == Mode::Default; }
	inline bool IsSlotFamily(Mode m) { return m == Mode::SlotReuse || m == Mode::SlotReuseFlags; }
	inline bool IsAliveFamily(Mode m) { return m == Mode::AliveDirect || m == Mode::AliveIndirect || m == Mode::AliveIndirectWave; }
	inline bool IsCompactFamily(Mode m) { return m == Mode::Compaction; }


	inline bool IsValidMode(Mode m) { return IsDefault(m) || IsSlotFamily(m) || IsAliveFamily(m); }


	inline bool IsSameFamily(Mode a, Mode b) 
	{
		if (a == b)
			return true;
		return (IsSlotFamily(a) && IsSlotFamily(b)) || (IsAliveFamily(a) && IsAliveFamily(b));
	}

	// ----------------------
	//		For DEBUG
	// ----------------------
	struct ParticleStat
	{
		CounterSnapshot counters = {};
		Mode	mode = {};
		uint32_t maxParticles = 0;
		uint32_t emitRequested = 0;   // counters와 같은 프레임의 요청량
		bool     hasCounter = false;
	};

	
}

