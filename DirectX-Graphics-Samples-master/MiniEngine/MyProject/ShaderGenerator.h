#pragma once
#include "CompiledEmitter.h"

#include <string>
#include <vector>

struct GeneratedShader
{
	std::string              source;     // HLSL 전체
	std::string              fileName;
	std::vector<std::string> errors;
	std::vector<std::string> warnings;

	bool Ok() const { return errors.empty(); }
};

// ------생성된 셰이더파일 규칙------
// 
// [1] 레이아웃 : Particle Data에서 Offset 이름과 Offset 값 설정
// 예시) static const uint OFS_Particle_Color = 12;
// 
// [2] 모듈 입력값 상수 버퍼 : 모듈 전체의 Parameter를 ConstBuffer로
// 예시) cbuffer ModuleParams : register(b1)
// { float M0_SpawnRate_Rate; ...}
//
// [3] 구조체 : 실제 파티클 구조체 & 이미터 구조체 선언
// [4] 읽기/저장 : Update에서 파티클 데이터를 읽고 쓰는 내용
// [5] 각 단계별 모듈 함수 : 모듈 조각들을 순서대로 이어 붙인다.
// [6] 각 단계의 내용 고정

GeneratedShader GenerateEmitterShader(const CompiledEmitter& compiled,
	const std::string& fileName = "Emitter.generated.hlsl");

namespace ShaderNames
{
	// Particles.Previous.Position -> Previous_Position,  Emitter.SpawnCount -> SpawnCount
	std::string Field(const AttributeKey& key);

	// Particles.Position -> OFS_Particles_Position,  Particles.Previous.Position -> OFS_Particles_Previous_Position
	std::string OffsetConst(const AttributeKey& key);

	// Module.M2_ShapeLocation.Radius -> M2_ShapeLocation_Radius
	std::string Param(const AttributeKey& key);

	// float3 -> "float3", bool -> "uint". 지원 안 하면 nullptr
	const char* HlslType(const TypeInfo* type);
}


