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

// 입력값을 만드는 방법 (Niagara 의 Input Mode 에 해당)
enum class ParamKind
{
    Constant,       // 고정값 1개
    RandomRange     // Min ~ Max 사이의 파티클별 랜덤값 (ParticleSpawn 전용)
};

struct ParamDesc
{
    std::string     name;
    const TypeInfo* type;
    std::vector<uint8_t> value;     // Constant: T 1개 / RandomRange: Min, Max 순서로 T 2개
    ParamKind       kind = ParamKind::Constant;
    bool            perComponent = false;   // RandomRange: 성분마다 독립 랜덤

    // ImGui 표시용 힌트
    bool            uiColor = false;
    bool            uiHasMin = false;
    float           uiMin = 0.f;

    ParamDesc& AsColor() { uiColor = true; return *this; }
    ParamDesc& WithMin(float v) { uiHasMin = true; uiMin = v; return *this; }
};

inline bool IsFloatFamily(const TypeInfo* t)
{
    return t == &Types::Float || t == &Types::Float2 || t == &Types::Float3 || t == &Types::Float4;
}

// 하나의 입력값이 상수 버퍼에서 차지하는 슬롯 이름들 (컴파일러, 셰이더 생성기, CPU 가 공유)
inline std::vector<std::string> ParamSlotNames(const std::string& name, ParamKind kind)
{
    if (kind == ParamKind::RandomRange)
        return { name + "_Min", name + "_Max" };
    return { name };
}

// 모듈 함수의 인자 타입. T 에서 암시적으로 변환되므로 상수는 그대로 넘길 수 있다
template<typename T>
struct Input
{
    T           a;
    T           b;
    ParamKind   kind = ParamKind::Constant;
    bool        perComponent = false;

    Input(const T& v) : a(v), b(v) {}
};

// perComponent = false : t 하나로 보간 (색상이면 두 색을 잇는 선 위의 값)
// perComponent = true  : 성분마다 독립적으로 보간
template<typename T>
Input<T> RandomRange(const T& min, const T& max, bool perComponent = false)
{
    Input<T> ret(min);
    ret.b = max;
    ret.kind = ParamKind::RandomRange;
    ret.perComponent = perComponent;
    return ret;
}

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

template<typename T>
ParamDesc MakeParam(const std::string& name, const Input<T>& input)
{
    ParamDesc ret;
    ret.name = name;
    ret.type = &TypeOf<T>();
    ret.kind = input.kind;
    ret.perComponent = input.perComponent;

    const size_t count = ParamSlotNames(name, input.kind).size();
    ret.value.resize(sizeof(T) * count);
    memcpy(ret.value.data(), &input.a, sizeof(T));
    if (count > 1)
        memcpy(ret.value.data() + sizeof(T), &input.b, sizeof(T));
    return ret;
}


enum class ModuleId : uint32_t
{
    SpawnRate,
    InitializeParticle,
    SphereLocation,
    AddVelocityInCone,
    ParticleState,
    GravityForce,
    SolveForceAndVelocity,
    SubUVAnimation,
    Count,
    Unknown = 0xFFFFFFFFu       // ModuleLibrary 밖에서 만든 모듈 (공유 셰이더로 실행 불가)
};

class ModuleContext;
// 모듈 선언 (값으로 복사해서 저장할 수 있도록 일반 구조체로 둔다)
struct ModuleDesc
{
    Stage              stage;
    ModuleId           id = ModuleId::Unknown;
    std::string        name;
    std::vector<Usage> reads;
    std::vector<Usage> writes;
    std::vector<ParamDesc> params;   // 모듈 입력값 (GPU 상수 버퍼로 감)

    std::string hlsl;

    std::function<void(ModuleContext&)> execute;

    int compiledIndex = -1;     // CompiledEmitter::modules 에서의 위치 (EmitterInstance 가 채움)
};

struct RenderBinding
{
    std::string             slot;
    const TypeInfo*         type;
    std::string             attribute;
    std::vector<uint8_t>    defaultValue;
};

template<typename T>
RenderBinding MakeRenderBinding(const std::string& slot, const std::string& attribute, const T& defaultValue)
{
    RenderBinding b;
    b.slot = slot;
    b.type = &TypeOf<T>();
    b.attribute = attribute;
    b.defaultValue.resize(sizeof(T));
    memcpy(b.defaultValue.data(), &defaultValue, sizeof(T));
    return b;

}


// 렌더러(또는 엔진 단계)는 읽기만 한다
struct RendererDesc
{
    std::string                 name;
    std::vector<RenderBinding>  bindings;
};

// --------------------------------------
//          hlsl 조각 규칙
// --------------------------------------

// GPU 에서 실행할 HLSL 조각
    //   - 파티클 값   : p.Position, p.Previous_Position ...   (reads/writes 에 선언한 것만)
    //   - Emitter 값  : e.SpawnCount ...                       (EmitterUpdate 단계)
    //   - 입력값      : PARAM(Radius)                          (params 에 선언한 것만)
    //   - 엔진 값     : DeltaTime, OwnerPosition ...
    //   - 그 외       : index, Random01(index, salt), KILL()
