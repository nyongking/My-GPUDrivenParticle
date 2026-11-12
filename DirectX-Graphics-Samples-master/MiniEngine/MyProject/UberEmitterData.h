#pragma once
// 5주차 3단계: CompiledEmitter -> 공유 셰이더(ParticleUber.hlsli)가 읽을 데이터
//
//   공유 셰이더는 이미터마다 아래 두 cbuffer 만 바꿔서 같은 PSO 로 실행한다.
//     b2 EmitterLayout : 어떤 모듈을 쓰나 + 속성 열의 위치   -> EmitterLayoutCB
//     b1 ModuleParams  : 모든 모듈의 고정 자리 입력값         -> UberEmitterData::params
//
//   이 파일과 ParticleUber.hlsli 는 같이 고친다.
//     ModuleId 순서         == HLSL MOD_ 비트
//     EmitterLayoutCB 필드  == HLSL cbuffer EmitterLayout
//     UberParamBlock() 배치 == HLSL cbuffer ModuleParams (CheckUberShaderLayout 이 시작할 때 확인)

#include "CompiledEmitter.h"
#include "ModuleDesc.h"

#include <cstdint>
#include <string>
#include <vector>

class ShaderCompiler;

namespace Uber
{
	constexpr uint32_t kNoAttribute = 0xFFFFFFFFu;   // HLSL NO_ATTRIBUTE

	// HLSL PC_ 비트 (벡터 입력값의 perComponent)
	constexpr uint32_t kPC_InitializeParticlePos = 1u << 0;
	constexpr uint32_t kPC_InitializeParticleColor = 1u << 1;
	constexpr uint32_t kPC_AddVelocityInConeAxis = 1u << 2;
}

// HLSL cbuffer EmitterLayout (b2) 와 같은 배치. 전부 uint 라 C++ 에서도 그대로 맞는다
struct EmitterLayoutCB
{
	uint32_t moduleMask = 0;                                // 1 << ModuleId
	uint32_t perComponentMask = 0;                          // Uber::kPC_*
	uint32_t slotInitializeParticle = 0;                    // 스택 순번 (RandomRange salt 용)
	uint32_t slotSphereLocation = 0;
	uint32_t slotAddVelocityInCone = 0;
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
	uint32_t pad[3] = {};
};
static_assert(sizeof(EmitterLayoutCB) == 80, "EmitterLayoutCB 는 HLSL EmitterLayout(80 바이트) 과 같아야 한다");

struct UberEmitterData
{
	EmitterLayoutCB          layout;
	std::vector<uint8_t>     params;     // UberParamBlock().ConstantBufferSize() 바이트 (b1)
	std::vector<std::string> warnings;   // 실행은 되지만 생성 방식과 다르게 동작할 수 있음
	std::vector<std::string> errors;     // 공유 셰이더로 실행할 수 없음

	bool Ok() const { return errors.empty(); }
};

const char* ModuleIdName(ModuleId id);

// 모든 모듈의 입력값을 ModuleId 순서로 넣은 고정 자리 Block (처음 부를 때 한 번 만든다)
//   키 = "Module.<모듈 이름에서 공백 뺀 것>.<입력값>[_Min|_Max]", HLSL 이름 = "<Scope>_<Slot>"
const Block& UberParamBlock();

// 공유 셰이더에서 이 입력값이 _Min/_Max 두 칸인가 (ParticleSpawn 단계의 float 계열이면 상수여도 두 칸)
bool IsUberRangeParam(Stage stage, const TypeInfo* type);

// 모듈 스택 + 컴파일 결과 -> 공유 셰이더 데이터. modules 는 compiled 를 만든 것과 같은 순서여야 한다
UberEmitterData BuildUberEmitterData(const std::vector<ModuleDesc>& modules, const CompiledEmitter& compiled);

// 입력값만 바뀌었을 때 (스택 구성은 그대로) params 만 다시 채운다
void FillUberParams(const std::vector<ModuleDesc>& modules, std::vector<uint8_t>& out, std::vector<std::string>* errors);

// 출력 창 확인용 (3단계 완료 기준: Main.generated.hlsl 의 모듈 목록 / OFS_ 값과 비교)
std::string DescribeUber(const UberEmitterData& data);

// 빌드된 공유 셰이더 3개의 cbuffer 배치가 C++ 쪽(UberParamBlock, EmitterLayoutCB)과 같은지 확인
// 반사 정보를 못 읽으면 log 에 남기고 true (검사를 못 한 것이지 틀린 것은 아님)
bool CheckUberShaderLayout(const ShaderCompiler& compiler, std::string& log);
