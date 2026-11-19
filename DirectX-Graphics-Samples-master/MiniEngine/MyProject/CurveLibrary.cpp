#include "pch.h"
#include "CurveLibrary.h"

#include "CommandContext.h"
#include "GpuBuffer.h"

#include <algorithm>
#include <cstring>

namespace
{
	// WriteBuffer / InitializeBuffer 는 16 바이트 정렬된 주소를 요구한다
	struct alignas(16) Sample16 { float4 v; };
	static_assert(sizeof(Sample16) == 16, "Sample16 은 float4 하나");

	struct State
	{
		std::vector<Sample16> samples;      // kMaxCurves x kSamples
		std::vector<uint8_t>  used;
		std::vector<uint32_t> freeList;     // 뒤에서부터 꺼낸다 (작은 번호 먼저)
		uint32_t              usedCount = 1;
		uint32_t              dirtyBegin = CurveLibrary::kMaxCurves;   // [begin, end) 칸. 비어 있으면 begin >= end
		uint32_t              dirtyEnd = 0;
		bool                  warnedFull = false;

		StructuredBuffer      buffer;
		bool                  gpuReady = false;

		State()
		{
			samples.assign(size_t(CurveLibrary::kMaxCurves) * CurveLibrary::kSamples, Sample16{ float4{ 1.f, 1.f, 1.f, 1.f } });
			used.assign(CurveLibrary::kMaxCurves, 0);
			used[CurveLibrary::kIdentitySlot] = 1;
			for (uint32_t s = CurveLibrary::kMaxCurves - 1; s > CurveLibrary::kIdentitySlot; --s)
				freeList.push_back(s);
		}
	};

	// 정적 객체(모듈 목록 등)가 가진 커브가 프로그램 종료 때 Free 를 부르므로 State 는 지우지 않는다
	State& S()
	{
		static State* s = new State();
		return *s;
	}

	void MarkDirty(State& s, uint32_t slot)
	{
		s.dirtyBegin = (std::min)(s.dirtyBegin, slot);
		s.dirtyEnd = (std::max)(s.dirtyEnd, slot + 1);
	}
}

void CurveLibrary::Initialize()
{
	State& s = S();
	if (s.gpuReady)
		return;

	s.buffer.Create(L"Curve LUT", kMaxCurves * kSamples, sizeof(Sample16), s.samples.data());
	s.dirtyBegin = kMaxCurves;
	s.dirtyEnd = 0;
	s.gpuReady = true;
}

void CurveLibrary::Shutdown()
{
	State& s = S();
	if (!s.gpuReady)
		return;

	s.buffer.Destroy();
	s.gpuReady = false;
}

uint32_t CurveLibrary::Allocate()
{
	State& s = S();
	if (s.freeList.empty())
	{
		if (!s.warnedFull)
			Utility::Print("[CurveLibrary] 커브 칸이 모자람 (kMaxCurves). 넘는 커브는 1 로 고정된다\n");
		s.warnedFull = true;
		return kIdentitySlot;
	}

	const uint32_t slot = s.freeList.back();
	s.freeList.pop_back();
	s.used[slot] = 1;
	++s.usedCount;
	return slot;
}

void CurveLibrary::Free(uint32_t slot)
{
	State& s = S();
	if (slot == kIdentitySlot || slot >= kMaxCurves || !s.used[slot])
		return;

	s.used[slot] = 0;
	--s.usedCount;
	s.freeList.push_back(slot);
}

void CurveLibrary::Write(uint32_t slot, const float4* samples)
{
	State& s = S();
	if (slot == kIdentitySlot || slot >= kMaxCurves)
		return;     // 대체 칸은 덮어쓰지 않는다

	std::memcpy(&s.samples[size_t(slot) * kSamples], samples, sizeof(float4) * kSamples);
	MarkDirty(s, slot);
}

const float4* CurveLibrary::Samples(uint32_t slot)
{
	State& s = S();
	if (slot >= kMaxCurves)
		slot = kIdentitySlot;
	return &s.samples[size_t(slot) * kSamples].v;
}

uint32_t CurveLibrary::UsedCount()
{
	return S().usedCount;
}

float4 CurveLibrary::Sample(uint32_t slot, float t)
{
	const float4* lut = Samples(slot);

	// saturate: NaN 도 0 이 된다 (HLSL 과 같게)
	const float clamped = (t > 0.f) ? ((t < 1.f) ? t : 1.f) : 0.f;
	const float u = clamped * float(kSamples - 1);
	const uint32_t i0 = uint32_t(u);
	const uint32_t i1 = (std::min)(i0 + 1, kSamples - 1);
	const float f = u - float(i0);

	const float4& a = lut[i0];
	const float4& b = lut[i1];
	return float4{ a.x + (b.x - a.x) * f, a.y + (b.y - a.y) * f, a.z + (b.z - a.z) * f, a.w + (b.w - a.w) * f };
}

void CurveLibrary::Flush(CommandContext& ctx)
{
	State& s = S();
	if (!s.gpuReady)
		return;

	if (s.dirtyBegin < s.dirtyEnd)
	{
		const size_t bytesPerCurve = sizeof(Sample16) * kSamples;
		ctx.WriteBuffer(s.buffer, s.dirtyBegin * bytesPerCurve,
			&s.samples[size_t(s.dirtyBegin) * kSamples], (s.dirtyEnd - s.dirtyBegin) * bytesPerCurve);
		s.dirtyBegin = kMaxCurves;
		s.dirtyEnd = 0;
	}

	// 루트 SRV 로 묶으려면 읽기 상태여야 한다 (WriteBuffer 가 COPY_DEST 로 바꿔 둔다)
	ctx.TransitionResource(s.buffer, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, true);
}

const StructuredBuffer& CurveLibrary::Buffer()
{
	return S().buffer;
}


CurveAsset::CurveAsset()
{
	m_Slot = CurveLibrary::Allocate();
	m_Keys = { CurveKey{ 0.f, float4{ 1.f, 1.f, 1.f, 1.f } }, CurveKey{ 1.f, float4{ 1.f, 1.f, 1.f, 1.f } } };
	Bake();
}

CurveAsset::CurveAsset(const std::vector<CurveKey>& keys)
	: m_Keys(keys)
{
	m_Slot = CurveLibrary::Allocate();
	Bake();
}

CurveAsset::~CurveAsset()
{
	CurveLibrary::Free(m_Slot);
}

namespace
{
	std::vector<CurveKey> SortedByTime(const std::vector<CurveKey>& keys)
	{
		std::vector<CurveKey> sorted = keys;
		std::stable_sort(sorted.begin(), sorted.end(),
			[](const CurveKey& a, const CurveKey& b) { return a.time < b.time; });
		return sorted;
	}

	// 시간 순으로 놓인 키에서 t 의 값. 첫 키 앞 / 마지막 키 뒤는 끝 값 유지
	float4 EvaluateSorted(const std::vector<CurveKey>& keys, float t)
	{
		if (keys.empty())
			return float4{ 1.f, 1.f, 1.f, 1.f };
		if (t <= keys.front().time)
			return keys.front().value;
		if (t >= keys.back().time)
			return keys.back().value;

		for (size_t k = 0; k + 1 < keys.size(); ++k)
		{
			const CurveKey& a = keys[k];
			const CurveKey& b = keys[k + 1];
			if (t > b.time)
				continue;

			const float span = b.time - a.time;
			if (span <= 0.f)
				return b.value;     // 같은 시간의 키 두 개 = 계단 (뒤 키 값)
			const float w = (t - a.time) / span;
			return float4{ a.value.x + (b.value.x - a.value.x) * w, a.value.y + (b.value.y - a.value.y) * w,
				a.value.z + (b.value.z - a.value.z) * w, a.value.w + (b.value.w - a.value.w) * w };
		}
		return keys.back().value;
	}
}

float4 CurveAsset::Evaluate(float t) const
{
	return EvaluateSorted(SortedByTime(m_Keys), t);
}

void CurveAsset::Bake()
{
	// 저장된 키 순서는 건드리지 않는다 (UI 에서 시간을 끌어 옮기는 중에 줄이 바뀌지 않게)
	const std::vector<CurveKey> sorted = SortedByTime(m_Keys);

	float4 lut[CurveLibrary::kSamples];
	for (uint32_t i = 0; i < CurveLibrary::kSamples; ++i)
		lut[i] = EvaluateSorted(sorted, float(i) / float(CurveLibrary::kSamples - 1));
	CurveLibrary::Write(m_Slot, lut);
}

void CurveAsset::SortKeys()
{
	m_Keys = SortedByTime(m_Keys);
}
