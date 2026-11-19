#pragma once

#include "AttributeType.h"
#include "CurveLibrary.h"
#include <vector>
#include <string>
#include <functional>
#include <memory>
#include <utility>

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
    RandomRange,    // Min ~ Max 사이의 파티클별 랜덤값 (ParticleSpawn 전용)
    Curve           // 커브 인덱스 속성(기본 NormalizedAge) 으로 커브를 읽은 값 (ParticleUpdate 전용, float 계열)
};

struct ParamDesc
{
    std::string     name;
    const TypeInfo* type;
    std::vector<uint8_t> value;     // Constant: T 1개 / RandomRange: Min, Max 순서로 T 2개 / Curve: T 1개 (상수로 되돌릴 때 쓸 값)
    ParamKind       kind = ParamKind::Constant;
    bool            perComponent = false;   // RandomRange: 성분마다 독립 랜덤

    // Curve: 키 + 전역 커브 버퍼의 칸. ModuleDesc 를 복사하면 같은 커브를 가리킨다 (CurveLibrary.h)
    std::shared_ptr<CurveAsset> curve;
    std::string     curveIndex = "Particles.NormalizedAge";   // 커브의 가로축으로 쓸 float 속성 (0 ~ 1)

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
    if (kind == ParamKind::Curve)
        return { name + "_Curve" };     // 커브 칸 번호 (uint)
    return { name };
}

// 그 슬롯의 타입. Curve 는 값이 아니라 커브 칸 번호가 들어가므로 uint
inline const TypeInfo* ParamSlotType(const TypeInfo* type, ParamKind kind)
{
    return kind == ParamKind::Curve ? &Types::UInt32 : type;
}

// 모듈 함수의 인자 타입. T 에서 암시적으로 변환되므로 상수는 그대로 넘길 수 있다
template<typename T>
struct Input
{
    T           a;
    T           b;
    ParamKind   kind = ParamKind::Constant;
    bool        perComponent = false;
    std::vector<CurveKey> keys;     // Curve 전용

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

// 커브 입력값: { 시간(0~1), 값 } 목록. 키 사이는 선형 보간
//   예) Curve<float>({ { 0.f, 1.f }, { 1.f, 0.f } })  = 수명 동안 1 -> 0
template<typename T>
Input<T> Curve(std::initializer_list<std::pair<float, T>> keys)
{
    Input<T> ret(keys.size() ? keys.begin()->second : T{});
    ret.kind = ParamKind::Curve;
    for (const std::pair<float, T>& k : keys)
    {
        CurveKey key;
        key.time = k.first;
        key.value = float4{ 0.f, 0.f, 0.f, 0.f };
        memcpy(&key.value, &k.second, sizeof(T));   // float 계열만 (EmitterCompiler 가 검사)
        ret.keys.push_back(key);
    }
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
    if (input.kind == ParamKind::Curve)
        ret.curve = std::make_shared<CurveAsset>(input.keys);
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
    InitializeSprite,
    SpriteRotationRate,
    ScaleColor,
    CurlNoiseForce,
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

// 복사한 모듈이 원본과 커브를 같이 쓰지 않게 커브를 새로 만든다 (모듈 목록에서 스택으로 추가할 때)
void DetachCurves(ModuleDesc& module);

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
    //                   상수 / RandomRange / Curve 어느 것이든 조각은 PARAM(n) 그대로 쓴다
    //   - 엔진 값     : DeltaTime, OwnerPosition ...
    //   - 그 외       : index, Random01(index, salt), KILL()
