#pragma once

#include "AttributeType.h"
#include <vector>
#include <string>
#include <functional>

enum class Stage 
{ 
    SystemSpawn, 
    SystemUpdate, 
    EmitterSpawn, 
    EmitterUpdate, 
    ParticleSpawn, 
    ParticleUpdate 
};

int         StageLevel(Stage s);   // 0 = System, 1 = Emitter, 2 = Particle
const char* StageName(Stage s);

// 값 하나 사용: 전체 이름 + 자료형
struct Usage
{
    std::string     attr;   // 전체 이름: "Particles.Previous.Position"
    const TypeInfo* type;   // 예: &Types::Float3
};

struct ParamDesc
{
    std::string     name;
    const TypeInfo* type;
    std::vector<uint8_t> value;
};

template<typename T>
ParamDesc MakeParam(const std::string& name, const T& v)
{
    ParamDesc ret;
    ret.name = name;
    ret.type = &TypeOf<T>();
    ret.value.resize(sizeof(T));
    memcpy(ret.value.data(), &v, sizeof(T));
    return ret;
}


class ModuleContext;
// 모듈 선언 (값으로 복사해서 저장할 수 있도록 일반 구조체로 둔다)
struct ModuleDesc
{
    std::string        name;
    Stage              stage;
    std::vector<Usage> reads;
    std::vector<Usage> writes;
    std::vector<ParamDesc> params;   // 모듈 입력값 (GPU 상수 버퍼로 감)

    std::string hlsl;

    std::function<void(ModuleContext&)> execute;
};

// 렌더러(또는 엔진 단계)는 읽기만 한다
struct RendererDesc
{
    std::string        name;
    std::vector<Usage> reads;
};

// --------------------------------------
//          hlsl 조각 규칙
// --------------------------------------

// GPU 에서 실행할 HLSL 조각
    //   - 파티클 값   : p.Position, p.Previous_Position ...   (reads/writes 에 선언한 것만)
    //   - Emitter 값  : e.SpawnCount ...                       (EmitterUpdate 단계)
    //   - 입력값      : PARAM(Radius)                          (params 에 선언한 것만)
    //   - 엔진 값     : DeltaTime, OwnerPosition
    //   - 그 외       : index, Random01(index, salt), KILL()
