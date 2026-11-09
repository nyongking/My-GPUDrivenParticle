#pragma once
#include <cstdint>
#include <string>
#include <vector>

class ProfileRecorder
{
public:
	void SetTimers(const std::vector<std::wstring>& names);
	void Start(uint32_t warmupFrames, uint32_t recordFrames);
	bool Tick(uint32_t aliveCount);

	bool		IsBusy() const { return m_State != State::Idle; }
	float		GetProgress() const;
	const char* GetStateText() const;

	bool AppendCSV(const std::string& path, const std::string& condHeader, const std::string& condValues,
		const std::string& tag, uint32_t maxParticles) const;

private:
	enum class State { Idle, Warmup, Recording };

	struct Summary { float mean = 0.f, median = 0.f, p95 = 0.f, max = 0.f; };
	static Summary Summarize(std::vector<float> samples);

	State		m_State = State::Idle;
	uint32_t	m_Remaining = 0;
	uint32_t	m_RecordFrames = 0;

	std::vector<std::wstring>		m_Names;
	std::vector<std::vector<float>>	m_Samples;
	std::vector<float>				m_FrameMs;
	std::vector<float>				m_Alive;
};

