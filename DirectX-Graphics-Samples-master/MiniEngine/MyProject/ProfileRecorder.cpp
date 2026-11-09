#include "pch.h"
#include "ProfileRecorder.h"

#include <algorithm>
#include <fstream>

namespace
{
	std::string ToNarrow(const std::wstring& w)
	{
		std::string s;
		for (wchar_t c : w)
			s += (c < 128) ? char(c) : '?';
		return s;
	}
}

void ProfileRecorder::SetTimers(const std::vector<std::wstring>& names)
{
	m_Names = names;
	m_Samples.assign(m_Names.size(), {});
}

void ProfileRecorder::Start(uint32_t warmupFrames, uint32_t recordFrames)
{
	m_RecordFrames = max(recordFrames, 1u);

	for (auto& s : m_Samples)
	{
		s.clear();
		s.reserve(m_RecordFrames);
	}
	m_FrameMs.clear();	m_FrameMs.reserve(m_RecordFrames);
	m_Alive.clear();	m_Alive.reserve(m_RecordFrames);

	if (warmupFrames > 0)
	{
		m_State = State::Warmup;
		m_Remaining = warmupFrames;
	}
	else
	{
		m_State = State::Recording;
		m_Remaining = m_RecordFrames;
	}
}

bool ProfileRecorder::Tick(uint32_t aliveCount)
{
	if (m_State == State::Idle || EngineProfiling::IsPaused())
		return false;

	if (m_State == State::Warmup)
	{
		if (--m_Remaining == 0)
		{
			m_State = State::Recording;
			m_Remaining = m_RecordFrames;
		}
		return false;
	}

	for (size_t i = 0; i < m_Names.size(); ++i)
	{
		const float ms = EngineProfiling::GetGpuTimeLast(m_Names[i]);
		if (ms >= 0.f)
			m_Samples[i].push_back(ms);
	}
	m_FrameMs.push_back(EngineProfiling::GetFrameTimeLast());
	m_Alive.push_back(float(aliveCount));

	if (--m_Remaining == 0)
	{
		m_State = State::Idle;
		return true;
	}
	return false;
}

float ProfileRecorder::GetProgress() const
{
	if (m_State != State::Recording || m_RecordFrames == 0)
		return 0.f;
	return 1.f - float(m_Remaining) / float(m_RecordFrames);
}

const char* ProfileRecorder::GetStateText() const
{
	switch (m_State)
	{
	case State::Warmup:		return "Warmup...";
	case State::Recording:	return "Recording...";
	default:				return "Idle";
	}
}

ProfileRecorder::Summary ProfileRecorder::Summarize(std::vector<float> v)
{
	Summary s;
	if (v.empty())
		return s;

	double sum = 0.0;
	for (float x : v)
		sum += x;

	std::sort(v.begin(), v.end());
	s.mean = float(sum / double(v.size()));
	s.median = v[v.size() / 2];
	s.p95 = v[min(v.size() - 1, v.size() * 95 / 100)];
	s.max = v.back();
	return s;
}

bool ProfileRecorder::AppendCSV(const std::string& path, const std::string& condHeader, const std::string& condValues,
	const std::string& tag, uint32_t maxParticles) const
{
	const bool needHeader = !std::ifstream(path).good();

	std::ofstream file(path, std::ios::app);
	if (!file)
		return false;

	if (needHeader)
	{
		file << condHeader << ",tag,maxParticles,frames,aliveAvg,aliveRatio,frameMs_mean,frameMs_median";
		for (const auto& name : m_Names)
		{
			const std::string n = ToNarrow(name);
			file << ',' << n << "_mean," << n << "_median," << n << "_p95," << n << "_max";
		}
		file << '\n';
	}

	double aliveSum = 0.0;
	for (float a : m_Alive)
		aliveSum += a;
	const double aliveAvg = m_Alive.empty() ? 0.0 : aliveSum / double(m_Alive.size());
	const double aliveRatio = maxParticles ? aliveAvg / double(maxParticles) : 0.0;
	const Summary frame = Summarize(m_FrameMs);

	file.setf(std::ios::fixed);
	file.precision(4);
	file << condValues << ',' << tag << ',' << maxParticles << ',' << m_FrameMs.size() << ','
		<< aliveAvg << ',' << aliveRatio << ',' << frame.mean << ',' << frame.median;

	for (const auto& samples : m_Samples)
	{
		if (samples.empty())
		{
			file << ",,,,";
			continue;
		}
		const Summary s = Summarize(samples);
		file << ',' << s.mean << ',' << s.median << ',' << s.p95 << ',' << s.max;
	}
	file << '\n';
	return true;
}