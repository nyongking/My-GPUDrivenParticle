#include "pch.h"
#include "ParticleSettings.h"
#include "ParticleUtility.h"

#include <cstdio>

namespace
{
	template <typename T>
	T ClampValue(T v, T lo, T hi) { return v < lo ? lo : (v > hi ? hi : v); }
}

bool ParticleSettings::operator==(const ParticleSettings& o) const
{
	return mode == o.mode
		&& maxParticles == o.maxParticles
		&& steadyRatio == o.steadyRatio
		&& burstRatio == o.burstRatio
		&& burstInterval == o.burstInterval
		&& minLife == o.minLife
		&& maxLife == o.maxLife;
}

namespace Particles
{
	ParticleSettings Sanitize(ParticleSettings s)
	{
		if (!IsValidMode(s.mode) && !IsSlotFamily(s.mode) && !IsAliveFamily(s.mode))
			s.mode = Mode::AliveIndirect;

		s.maxParticles = ClampValue(s.maxParticles, 64u, kMaxDispatchParticles);
		s.steadyRatio = ClampValue(s.steadyRatio, 0.f, 1.f);
		s.burstRatio = ClampValue(s.burstRatio, 0.f, 1.f);
		s.burstInterval = ClampValue(s.burstInterval, 1.f / 60.f, 60.f);
		s.minLife = ClampValue(s.minLife, 0.01f, 60.f);
		s.maxLife = ClampValue(s.maxLife, s.minLife, 60.f);	// max >= min 보장
		return s;
	}

	EmitterParams BuildEmitterParams(const ParticleSettings& s)
	{
		EmitterParams p;		// 속도, 크기는 기본값 그대로
		p.minLife = s.minLife;
		p.maxLife = s.maxLife;
		p.emitRate = ComputeEmitRateByAliveRatio(s.steadyRatio, s.minLife, s.maxLife, s.maxParticles);
		p.burstCount = uint32_t(s.burstRatio * float(s.maxParticles));
		p.burstInterval = s.burstInterval;
		return p;
	}

	const char* ModeName(Mode mode)
	{
		switch (mode)
		{
		case Mode::Default:			return "FullScan";
		case Mode::SlotReuse:			return "SlotReuse";
		case Mode::SlotReuseFlags:		return "SlotReuseFlags";
		case Mode::AliveDirect:			return "AliveDirect";
		case Mode::AliveIndirect:		return "AliveIndirect";
		case Mode::AliveIndirectWave:	return "AliveIndirectWave";
		case Mode::Compaction:			return "Compaction";
		default:						return "None";
		}
	}

	std::string SettingsCsvHeader()
	{
		return "mode,steadyRatio,burstRatio,burstInterval,minLife,maxLife";
	}

	std::string SettingsCsvValues(const ParticleSettings& s)
	{
		char buf[160];
		snprintf(buf, sizeof(buf), "%s,%.2f,%.2f,%.2f,%.2f,%.2f",
			ModeName(s.mode), s.steadyRatio, s.burstRatio, s.burstInterval, s.minLife, s.maxLife);
		return buf;
	}
}