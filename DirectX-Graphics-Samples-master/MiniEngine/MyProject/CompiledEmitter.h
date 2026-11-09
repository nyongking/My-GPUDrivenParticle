#pragma once
#include "LayoutBuilder.h"
#include "ModuleDesc.h"

#include <string>
#include <vector>

// Shader Generator가 써내려갈 모듈 정보.
// 입력값 하나의 평가 방법 (셰이더 생성기와 CPU ModuleContext 가 같이 사용)
struct CompiledParam
{
    std::string     name;
    const TypeInfo* type;
    ParamKind       kind;
    bool            perComponent;
    uint32_t        salt;   // RandomRange 용. 모듈이 직접 쓰는 salt 와 겹치지 않는 대역
};

struct CompiledModule
{
    std::string     name; // 예시 : "Shape Location"
    std::string     scope; // 예시 : "M2_ShapeLocation"
    Stage           stage;
    std::string     hlsl; // 셰이더 조각
    std::vector<Usage> reads; // read
    std::vector<Usage> writes; // write
    std::vector<std::string> params; // param
    std::vector<CompiledParam> paramInfos;
};

struct DeclaredAttribute
{
    AttributeKey key;
    const TypeInfo* type;
    bool    inLayout; // false면 trim
};

// Compile 된 Emitter 자료
struct CompiledEmitter
{
    Block particles;       // 파티클 버퍼 레이아웃 (= "파티클 구조체")
    Block emitterParams;   // Emitter 당 1개씩
    Block systemParams;    // System 당 1개씩

    // 모듈입력값들, HLSL 상수 버퍼 규칙으로 배치
    // 키 = 'Module.<Scope>.<Param>', HLSL 이름 = '<Scope>_<Param>'
    Block                   moduleParams{ Packing::ConstantBuffer };
    std::vector<uint8_t>    moduleParamData; // 처음 값이 채워진 바이트
    std::vector<std::string>    moduleScopes; // 모듈 순서대로 / 예시 : M0_SpawnRate, M1_ ... 

    // 셰이더 생성기 전용
    std::vector<CompiledModule> modules; // 모듈 순서대로 구조체
    std::vector<DeclaredAttribute> declaredParticles; // Particles.* 내용 전부.
    std::vector<DeclaredAttribute> declaredEmitter; // Emitters.* 내용 전부

    std::vector<CopyOp> copyAtSpawnEnd;     // Initial  <- 현재 (새로 태어난 파티클만)
    std::vector<CopyOp> copyAtUpdateStart;  // Previous <- 현재 (살아 있는 파티클 전부)

    std::vector<std::string> temporaries;
    std::vector<std::string> engineValues;
    std::vector<std::string> trimmed;
    std::vector<std::string> warnings;
    std::vector<std::string> errors;

    bool Ok() const { return errors.empty(); }
};

std::string DescribeCompiled(const CompiledEmitter& c, const std::string& title);
