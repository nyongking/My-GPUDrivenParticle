#pragma once
// CompiledEmitter -> 공유 셰이더(ParticleUber.hlsli)가 읽을 데이터
//
//   공유 셰이더는 이미터마다 아래 두 데이터만 바꿔서 같은 PSO 로 실행한다.
//     b2 EmitterLayout : 단계별 인스턴스 범위 + 속성 열의 위치   -> EmitterLayoutCB
//     t0 ModuleData    : [인스턴스 uint4 x N][입력값 dword ...]   -> UberEmitterData::params
//       StructuredBuffer<uint> (루트 SRV)
//       인스턴스 = { ModuleId, 스택 순번, 입력값 시작 dword, 플래그 (하위 16 비트 perComponent / 상위 16 비트 Curve) }
//       Curve 입력값은 값 자리의 첫 dword 에 전역 커브 버퍼의 칸 번호가 들어간다 (dword 수는 상수일 때와 같다)
//       같은 모듈이 여러 번 있어도 인스턴스가 여러 개일 뿐이라 스택 그대로 실행된다
//
//   이 파일과 ParticleUber.hlsli 는 같이 고친다.
//     ModuleId 순서          == HLSL MODID_
//     EmitterLayoutCB 필드   == HLSL cbuffer EmitterLayout (CheckUberShaderLayout 이 시작할 때 확인)
//     UberParamRecord() 배치 == HLSL 모듈 함수의 ParamF(m, k) 번호 (kExpectedRecordDwords 로 확인)

#include "CompiledEmitter.h"
#include "ModuleDesc.h"

#include <cstdint>
#include <string>
#include <vector>

class ShaderCompiler;

namespace Uber
{
	constexpr uint32_t kNoAttribute = 0xFFFFFFFFu;   // HLSL NO_ATTRIBUTE

	// StructuredBuffer 라 셰이더 쪽 한계는 없다. 매 프레임 업로드 힙에 올리므로 안전용 상한만 둔다 (1MB)
	constexpr uint32_t kMaxDataDwords = 256u * 1024u;

	// 모듈 인스턴스 1개 = uint4 (HLSL ModuleInst)
	struct ModuleInstGpu
	{
		uint32_t id;      // ModuleId
		uint32_t slot;    // 스택 순번 (RandomRange salt)
		uint32_t base;    // 입력값 시작 dword
		uint32_t flags;   // bit k = k 번째 입력값 perComponent, bit (16 + k) = k 번째 입력값이 Curve
	};
	static_assert(sizeof(ModuleInstGpu) == 16, "ModuleInstGpu 는 uint4");

	constexpr uint32_t kMaxFlaggedParams = 16;   // 플래그를 달 수 있는 입력값 수 (모듈 하나 기준)
	constexpr uint32_t kCurveFlagShift = 16;     // HLSL IsCurve
}

// HLSL cbuffer EmitterLayout (b2) 와 같은 배치. 전부 uint 라 C++ 에서도 그대로 맞는다
struct EmitterLayoutCB
{
	uint32_t emitterInstBegin = 0;                          // 단계별 인스턴스 범위 (ModuleData 의 인스턴스 번호)
	uint32_t emitterInstCount = 0;
	uint32_t spawnInstBegin = 0;
	uint32_t spawnInstCount = 0;
	uint32_t updateInstBegin = 0;
	uint32_t updateInstCount = 0;
	uint32_t ofsEmitterSpawnCount = Uber::kNoAttribute;
	uint32_t ofsEmitterSpawnRemainder = Uber::kNoAttribute;
	uint32_t ofsPosition = Uber::kNoAttribute;
	uint32_t ofsColor = Uber::kNoAttribute;
	uint32_t ofsSize = Uber::kNoAttribute;
	uint32_t ofsLifetime = Uber::kNoAttribute;
	uint32_t ofsAge = Uber::kNoAttribute;
	uint32_t ofsMass = Uber::kNoAttribute;
	uint32_t ofsNormalizedAge = Uber::kNoAttribute;
	uint32_t ofsVelocity = Uber::kNoAttribute;
	uint32_t ofsAcceleration = Uber::kNoAttribute;
	uint32_t ofsSubImageIndex = Uber::kNoAttribute;
	uint32_t ofsSpriteSize = Uber::kNoAttribute;
	uint32_t ofsSpriteRotation = Uber::kNoAttribute;
	uint32_t ofsInitialColor = Uber::kNoAttribute;         // Particles.Initial.Color (Spawn 끝에 Color 를 복사)
	uint32_t reserved[3] = {};                              // cbuffer 크기는 16 의 배수 (HLSL _LayoutReserved)
};
static_assert(sizeof(EmitterLayoutCB) == 96, "EmitterLayoutCB 는 HLSL EmitterLayout(96 바이트) 과 같아야 한다");

struct UberEmitterData
{
	EmitterLayoutCB          layout;
	std::vector<uint8_t>     params;     // t0 ModuleData (인스턴스 + 입력값, 16 바이트 단위)
	std::vector<std::string> warnings;   // 실행은 되지만 생성 방식과 다르게 동작할 수 있음
	std::vector<std::string> errors;     // 공유 셰이더로 실행할 수 없음

	bool Ok() const { return errors.empty(); }
};

// 모듈 하나의 입력값 배치: params[i] 가 시작하는 dword (Min 자리), 전체 dword 수
struct UberParamRecord
{
	std::vector<uint32_t> paramDword;   // m.params 와 같은 순서
	uint32_t              dwords = 0;
};
UberParamRecord MakeUberParamRecord(const ModuleDesc& m);

// 공유 셰이더에서 이 입력값이 _Min/_Max 두 칸인가 (ParticleSpawn 단계의 float 계열이면 상수여도 두 칸)
bool IsUberRangeParam(Stage stage, const TypeInfo* type);

// 모듈 스택 + 컴파일 결과 -> 공유 셰이더 데이터. modules 는 compiled 를 만든 것과 같은 순서여야 한다
UberEmitterData BuildUberEmitterData(const std::vector<ModuleDesc>& modules, const CompiledEmitter& compiled);

// 입력값만 바뀌었을 때 (스택 구성은 그대로) params 만 다시 채운다
//   인스턴스 목록 + 입력값 풀을 통째로 다시 만든다 (범위는 스택이 같으면 같음)
void FillUberParams(const std::vector<ModuleDesc>& modules, std::vector<uint8_t>& out, std::vector<std::string>* errors);

// 출력 창 확인용 (3단계 완료 기준: Main.generated.hlsl 의 모듈 목록 / OFS_ 값과 비교)
std::string DescribeUber(const UberEmitterData& data);

// 빌드된 공유 셰이더 3개의 배치가 C++ 쪽과 같은지 확인 (EmitterLayoutCB + 모듈별 입력값 dword 수)
// 반사 정보를 못 읽으면 log 에 남기고 true (검사를 못 한 것이지 틀린 것은 아님)
bool CheckUberShaderLayout(const ShaderCompiler& compiler, std::string& log);
