#pragma once
#include "AttributeType.h"

#include <cstdint>
#include <vector>

class CommandContext;
class StructuredBuffer;

// =====================================================================
// 커브 입력값 (ParamKind::Curve)
//   편집 = 키 (시간 0~1, 값). 키 사이는 선형 보간
//   실행 = 구운 LUT (kSamples 개의 float4). CPU / 생성 셰이더 / 공유 셰이더가 같은 표를 같은 식으로 읽는다
//
//   LUT 는 전역 버퍼 하나에 모아 둔다 (StructuredBuffer<float4>, 셰이더의 CurveData : t1)
//     커브 하나 = 칸(slot) 하나 = kSamples 개. 입력값 자리에는 칸 번호(uint) 만 들어간다
//     키를 고치면 그 칸만 다시 굽고 다음 Flush 때 올린다 (재컴파일 / 이미터 재시작 없음)
//
//   ModuleCommon.hlsli 의 CURVE_SAMPLES / CURVE_MAX_CURVES / SampleCurve 와 같이 고친다
// =====================================================================

struct CurveKey
{
	float  time = 0.f;                       // 0 ~ 1 (커브 인덱스 속성 값, 기본 NormalizedAge)
	float4 value = { 1.f, 1.f, 1.f, 1.f };   // float 는 x 만, float3 는 xyz 만 쓴다
};

namespace CurveLibrary
{
	constexpr uint32_t kSamples = 128;       // HLSL CURVE_SAMPLES
	constexpr uint32_t kMaxCurves = 1024;    // HLSL CURVE_MAX_CURVES (128 x 16 B x 1024 = 2 MB)
	constexpr uint32_t kIdentitySlot = 0;    // 항상 (1,1,1,1). 칸이 모자랄 때의 대체 칸

	// GPU 버퍼. Initialize 전에 만든 커브도 CPU 쪽 표에 들어 있다가 같이 올라간다
	void Initialize();
	void Shutdown();

	uint32_t Allocate();                     // 빈 칸이 없으면 kIdentitySlot
	void     Free(uint32_t slot);
	void     Write(uint32_t slot, const float4* samples);   // kSamples 개
	const float4* Samples(uint32_t slot);
	uint32_t UsedCount();

	// HLSL SampleCurve 와 같은 식 (t 는 0~1 로 자름, 이웃한 두 샘플을 선형 보간)
	float4 Sample(uint32_t slot, float t);

	// 바뀐 칸을 GPU 로 올리고 버퍼를 읽기 상태로 둔다. 시뮬레이션 컨텍스트 시작에서 한 번
	void Flush(CommandContext& ctx);
	const StructuredBuffer& Buffer();
}

// 커브 하나 = 키 목록 + 전역 버퍼의 칸 하나. 없어질 때 칸을 돌려준다
//   ParamDesc 가 shared_ptr 로 들고 있어서 ModuleDesc 를 복사하면 같은 커브를 가리킨다
//   (편집 중인 스택과 적용된 스택이 같은 커브를 보므로 키 편집이 바로 반영된다)
//   따로 편집해야 하는 복사본은 DetachCurves(ModuleDesc&) 로 떼어 낸다
class CurveAsset
{
public:
	CurveAsset();
	explicit CurveAsset(const std::vector<CurveKey>& keys);
	~CurveAsset();
	CurveAsset(const CurveAsset&) = delete;
	CurveAsset& operator=(const CurveAsset&) = delete;

	std::vector<CurveKey>&       Keys()       { return m_Keys; }
	const std::vector<CurveKey>& Keys() const { return m_Keys; }

	// LUT 를 다시 굽는다. 키를 고친 뒤에 부른다 (키는 시간 순이 아니어도 된다. 구울 때 정렬해서 본다)
	void Bake();
	// 저장된 키를 시간 순으로 다시 놓는다 (결과는 같다. 보기 좋게 정리할 때)
	void SortKeys();

	uint32_t Slot() const { return m_Slot; }

	// 키에서 바로 계산한 값 (LUT 를 거치지 않음. 첫 키 앞 / 마지막 키 뒤는 끝 값 유지)
	float4 Evaluate(float t) const;

private:
	std::vector<CurveKey> m_Keys;
	uint32_t              m_Slot = CurveLibrary::kIdentitySlot;
};
