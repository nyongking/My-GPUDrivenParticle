#pragma once

#include <cstdint>
#include <string>
#include "ParticleTypes.h"
#include "ParticleEmitter.h"

// ImGui에서 바꾸는 측정 조건을 전부 한곳에 모은 것
struct ParticleSettings
{
	// --- Pool: MAX나 계열(Slot <-> Alive)이 바뀌면 버퍼를 새로 만든다 ---
	Particles::Mode	mode = Particles::Mode::AliveIndirect;
	uint32_t		maxParticles = 1000000;

	// --- Emit: 바뀌면 Emitter만 다시 설정한다 ---
	float	steadyRatio = 0.5f;		// steady로 유지할 alive 비율 (MAX 대비)
	float	burstRatio = 0.0f;		// burst 한 번에 만들 양 (MAX 대비). 0이면 burst 끔
	float	burstInterval = 2.0f;	// 초 (시뮬레이션 시간)
	float	minLife = 1.0f;
	float	maxLife = 3.0f;

	bool operator==(const ParticleSettings& o) const;
	bool operator!=(const ParticleSettings& o) const { return !(*this == o); }
};

namespace Particles
{
	// Dispatch1D(MAX, 64)가 넘으면 안 되는 한도 (그룹 수 65535 x 64)
	static const uint32_t kMaxDispatchParticles = 65535u * 64u;

	ParticleSettings	Sanitize(ParticleSettings s);					// 잘못된 값 정리
	EmitterParams		BuildEmitterParams(const ParticleSettings& s);	// 비율 -> 실제 개수 (변환은 여기서만)

	const char* ModeName(Mode mode);
	std::string	SettingsCsvHeader();
	std::string	SettingsCsvValues(const ParticleSettings& s);
}