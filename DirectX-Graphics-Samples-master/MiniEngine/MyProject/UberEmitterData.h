#pragma once

#include "CompiledEmitter.h"
#include "ModuleDesc.h"

#include <cstdint>
#include <string>
#include <vector>

class ShaderCompiler;

namespace Uber
{
	constexpr uint32_t kNoAttribute = 0xFFFFFFFFu;   // HLSL NO_ATTRIBUTE

	constexpr uint32_t kMaxDataDwords = 256u * 1024u;
	
	struct ModuleInstGpu
	{
		uint32_t id;
		uint32_t slot;
		uint32_t base;
		uint32_t flags;
	};
	static_assert(sizeof(ModuleInstGpu) == 16, "Module Inst Gpu는 16bytes");
}

struct EmitterLayoutCB
{
	uint32_t emitterInstBegin = 0;
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
	uint32_t pad[2] = {};
};
static_assert(sizeof(EmitterLayoutCB) == 80, "EmitterLayoutCB 는 HLSL EmitterLayout(80 바이트) 과 같아야 한다");

struct UberEmitterData
{
	EmitterLayoutCB          layout;
	std::vector<uint8_t>     params;
	std::vector<std::string> warnings;   // 실행은 되지만 생성 방식과 다르게 동작할 수 있음
	std::vector<std::string> errors;     // 공유 셰이더로 실행할 수 없음

	bool Ok() const { return errors.empty(); }
};

const char* ModuleIdName(ModuleId id);

struct UberParamRecord
{
	std::vector<uint32_t> paramDword;   // m.params
	uint32_t              dwords = 0;
};
UberParamRecord MakeUberParamRecord(const ModuleDesc& m);

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
