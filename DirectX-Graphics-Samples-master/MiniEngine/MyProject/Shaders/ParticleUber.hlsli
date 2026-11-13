#include "ModuleCommon.hlsli"

#define NO_ATTRIBUTE 0xFFFFFFFF      // 렌더러(ParticleSpriteVS) 와 같은 규칙

// =====================================================================
// 0. 모듈 번호, C++의 ModuleID
// =====================================================================
#define MODID_SPAWN_RATE                0
#define MODID_INITIALIZE_PARTICLE       1
#define MODID_SPHERE_LOCATION           2
#define MODID_ADD_VELOCITY_IN_CONE      3
#define MODID_PARTICLE_STATE            4
#define MODID_GRAVITY_FORCE             5
#define MODID_SOLVE_FORCE_AND_VELOCITY  6
#define MODID_SUBUV_ANIMATION           7

// =====================================================================
// 1. 레이아웃 (b2). 이미터마다 다른 값. 전부 uint 라 C++ 구조체와 그대로 맞는다
// =====================================================================
cbuffer EmitterLayout : register(b2)
{
    uint EmitterInstBegin;
    uint EmitterInstCount;
    uint SpawnInstBegin;
    uint SpawnInstCount;
    uint UpdateInstBegin;
    uint UpdateInstCount; // 20
    uint Ofs_Emitter_SpawnCount; // 24  Emitter 블록
    uint Ofs_Emitter_SpawnRemainder; // 28
    uint Ofs_Position; // 32  Particles 블록 (SoA 열 시작 = offset * Capacity)
    uint Ofs_Color; // 36
    uint Ofs_Size; // 40
    uint Ofs_Lifetime; // 44
    uint Ofs_Age; // 48
    uint Ofs_Mass; // 52
    uint Ofs_NormalizedAge; // 56
    uint Ofs_Velocity; // 60
    uint Ofs_Acceleration; // 64
    uint Ofs_SubImageIndex; // 68
    uint2 _LayoutPad; // 72
};

bool HasAttr(uint ofs)
{
    return ofs != NO_ATTRIBUTE;
}

StructuredBuffer<uint> ModuleData : register(t0);

struct ModuleInst
{
    uint Id;
    uint Slot;
    uint Base;
    uint Flags;
};

ModuleInst LoadInst(uint i)
{
    ModuleInst m;
    m.Id = ModuleData[i * 4 + 0];
    m.Slot = ModuleData[i * 4 + 1];
    m.Base = ModuleData[i * 4 + 2];
    m.Flags = ModuleData[i * 4 + 3];
    return m;
}

uint ParamU(ModuleInst m, uint k)
{
    return ModuleData[m.Base + k];
}
float ParamF(ModuleInst m, uint k)
{
    return asfloat(ParamU(m, k));
}
float3 ParamF3(ModuleInst m, uint k)
{
    return float3(ParamF(m, k), ParamF(m, k + 1), ParamF(m, k + 2));
}
float4 ParamF4(ModuleInst m, uint k)
{
    return float4(ParamF(m, k), ParamF(m, k + 1), ParamF(m, k + 2), ParamF(m, k + 3));
}
bool PerComp(ModuleInst m, uint paramIndex)
{
    return (m.Flags & (1u << paramIndex)) != 0;
}

// =====================================================================
// 3. RandomRange
//   생성 방식(ShaderGenerator)과 같은 식, 같은 salt 를 써서 결과가 비트 단위로 같다.
//     salt = 0x10000 + 스택순번 * 0x100 + 입력값순번 * 4   (EmitterCompiler 의 kSalt* 와 같음)
//     값   = Min + (Max - Min) * t
//     perComponent 가 꺼져 있으면 t 하나를 모든 성분에, 켜져 있으면 성분마다 salt + c
//   Min == Max (상수) 이면 랜덤을 계산하지 않는다. 생성 방식에서 상수는 랜덤을 안 부르므로
//   비용을 맞추기 위해서다. 조건이 cbuffer 값이라 분기는 발산하지 않는다.
// =====================================================================
uint ParamSalt(uint slot, uint paramIndex)
{
    return 0x10000u + slot * 0x100u + paramIndex * 4u;
}

float RandomParam1(float mn, float mx, uint index, uint salt)
{
    if (mn == mx)
        return mn;
    return mn + (mx - mn) * Random01(index, salt);
}

float3 RandomParam3(float3 mn, float3 mx, uint index, uint salt, bool perComponent)
{
    if (all(mn == mx))
        return mn;
    if (perComponent)
        return mn + (mx - mn) * float3(Random01(index, salt), Random01(index, salt + 1), Random01(index, salt + 2));
    return mn + (mx - mn) * Random01(index, salt);
}

float4 RandomParam4(float4 mn, float4 mx, uint index, uint salt, bool perComponent)
{
    if (all(mn == mx))
        return mn;
    if (perComponent)
        return mn + (mx - mn) * float4(Random01(index, salt), Random01(index, salt + 1), Random01(index, salt + 2), Random01(index, salt + 3));
    return mn + (mx - mn) * Random01(index, salt);
}

// =====================================================================
// 4. 구조체 (모든 속성을 가진 고정 구조체)
// =====================================================================
struct Particle
{
    float3 Position;
    float4 Color;
    float Size;
    float Lifetime;
    float Age;
    float Mass;
    float NormalizedAge;
    float3 Velocity;
    float3 Acceleration;
    float SubImageIndex;
};

struct EmitterState
{
    uint SpawnCount;
    float SpawnRemainder;
};

// =====================================================================
// 5. 읽기 / 쓰기: 이 이미터에 있는 열만 건드린다 (trim 된 속성은 NO_ATTRIBUTE)
// =====================================================================
Particle LoadParticle(RWByteAddressBuffer b, uint i)
{
    Particle p = (Particle) 0;
    if (HasAttr(Ofs_Position))
        p.Position = LoadF3(b, Ofs_Position, i);
    if (HasAttr(Ofs_Color))
        p.Color = LoadF4(b, Ofs_Color, i);
    if (HasAttr(Ofs_Size))
        p.Size = LoadF1(b, Ofs_Size, i);
    if (HasAttr(Ofs_Lifetime))
        p.Lifetime = LoadF1(b, Ofs_Lifetime, i);
    if (HasAttr(Ofs_Age))
        p.Age = LoadF1(b, Ofs_Age, i);
    if (HasAttr(Ofs_Mass))
        p.Mass = LoadF1(b, Ofs_Mass, i);
    if (HasAttr(Ofs_NormalizedAge))
        p.NormalizedAge = LoadF1(b, Ofs_NormalizedAge, i);
    if (HasAttr(Ofs_Velocity))
        p.Velocity = LoadF3(b, Ofs_Velocity, i);
    if (HasAttr(Ofs_Acceleration))
        p.Acceleration = LoadF3(b, Ofs_Acceleration, i);
    if (HasAttr(Ofs_SubImageIndex))
        p.SubImageIndex = LoadF1(b, Ofs_SubImageIndex, i);
    return p;
}

void StoreParticle(RWByteAddressBuffer b, uint i, Particle p)
{
    if (HasAttr(Ofs_Position))
        StoreF3(b, Ofs_Position, i, p.Position);
    if (HasAttr(Ofs_Color))
        StoreF4(b, Ofs_Color, i, p.Color);
    if (HasAttr(Ofs_Size))
        StoreF1(b, Ofs_Size, i, p.Size);
    if (HasAttr(Ofs_Lifetime))
        StoreF1(b, Ofs_Lifetime, i, p.Lifetime);
    if (HasAttr(Ofs_Age))
        StoreF1(b, Ofs_Age, i, p.Age);
    if (HasAttr(Ofs_Mass))
        StoreF1(b, Ofs_Mass, i, p.Mass);
    if (HasAttr(Ofs_NormalizedAge))
        StoreF1(b, Ofs_NormalizedAge, i, p.NormalizedAge);
    if (HasAttr(Ofs_Velocity))
        StoreF3(b, Ofs_Velocity, i, p.Velocity);
    if (HasAttr(Ofs_Acceleration))
        StoreF3(b, Ofs_Acceleration, i, p.Acceleration);
    if (HasAttr(Ofs_SubImageIndex))
        StoreF1(b, Ofs_SubImageIndex, i, p.SubImageIndex);
}

EmitterState LoadEmitter()
{
    EmitterState e = (EmitterState) 0;
    if (HasAttr(Ofs_Emitter_SpawnCount))
        e.SpawnCount = EmitterData.Load(Ofs_Emitter_SpawnCount);
    if (HasAttr(Ofs_Emitter_SpawnRemainder))
        e.SpawnRemainder = asfloat(EmitterData.Load(Ofs_Emitter_SpawnRemainder));
    return e;
}

void StoreEmitter(EmitterState e)
{
    if (HasAttr(Ofs_Emitter_SpawnCount))
        EmitterData.Store(Ofs_Emitter_SpawnCount, asuint(e.SpawnCount));
    if (HasAttr(Ofs_Emitter_SpawnRemainder))
        EmitterData.Store(Ofs_Emitter_SpawnRemainder, asuint(e.SpawnRemainder));
}

// =====================================================================
// 6. 모듈 (ModuleLibrary.cpp 의 m.hlsl 본문을 그대로 옮김)
//   본문 안의 PARAM(n) 과 KILL() 은 바꾸지 않는다. 함수 앞에서 P_<입력값> 지역 변수를 만들고
//   PARAM(n) 을 P_##n 으로 펼친다. RandomRange 는 이 지역 변수를 만들 때 한 번만 계산된다.
//   ModuleLibrary 의 hlsl 을 고치면 여기도 같이 고친다.
// =====================================================================
#define PARAM(n) P_##n

// ---- [EmitterUpdate] Spawn Rate ----
void Module_SpawnRate(inout EmitterState e, ModuleInst m)
{
    const float P_Rate = ParamF(m, 0);

    const float want = PARAM(Rate) * DeltaTime + e.SpawnRemainder;
    const uint n = (uint) want;
    e.SpawnRemainder = want - (float) n;
    e.SpawnCount += n;
}

// ---- [ParticleSpawn] Initialize Particle ----
void Module_InitializeParticle(inout Particle p, uint index, ModuleInst m)
{
    const float P_Size = RandomParam1(ParamF(m, 0), ParamF(m, 1), index, ParamSalt(m.Slot, 0));
    const float P_Mass = RandomParam1(ParamF(m, 2), ParamF(m, 3), index, ParamSalt(m.Slot, 1));
    const float P_Lifetime = RandomParam1(ParamF(m, 4), ParamF(m, 5), index, ParamSalt(m.Slot, 2));
    const float3 P_Pos = RandomParam3(ParamF3(m, 6), ParamF3(m, 9), index, ParamSalt(m.Slot, 3), PerComp(m, 3));
    const float4 P_Color = RandomParam4(ParamF4(m, 12), ParamF4(m, 16), index, ParamSalt(m.Slot, 4), PerComp(m, 4));

    p.Position = OwnerPosition + PARAM(Pos);
    p.Mass = PARAM(Mass);
    p.Color = PARAM(Color);
    p.Size = PARAM(Size);
    p.Lifetime = PARAM(Lifetime);
    p.Age = 0.f;
}

// ---- [ParticleSpawn] Sphere Location ----
void Module_SphereLocation(inout Particle p, uint index, ModuleInst m)
{
    const float P_Radius = RandomParam1(ParamF(m, 0), ParamF(m, 1), index, ParamSalt(m.Slot, 0));
    const uint P_SurfaceOnly = ParamU(m, 2);
    
    const float kTwoPi = 6.28318530f;
    const float z = Random01(index, 0) * 2.f - 1.f;
    const float phi = Random01(index, 1) * kTwoPi;
    const float r = sqrt(max(0.f, 1.f - z * z));
    const float dist = (PARAM(SurfaceOnly) != 0) ? PARAM(Radius) :
    		PARAM(Radius) * pow(Random01(index, 2), 1.f / 3.f);
    p.Position += float3(r * cos(phi) * dist, r * sin(phi) * dist, z * dist);
}

// ---- [ParticleSpawn] Add Velocity In Cone ----
void Module_AddVelocityInCone(inout Particle p, uint index, ModuleInst m)
{
    const float P_Speed = RandomParam1(ParamF(m, 0), ParamF(m, 1), index, ParamSalt(m.Slot, 0));
    const float P_ConeAngle = RandomParam1(ParamF(m, 2), ParamF(m, 3), index, ParamSalt(m.Slot, 1));
    const float3 P_ConeAxis = RandomParam3(ParamF3(m, 4), ParamF(m, 7), index, ParamSalt(m.Slot, 2), PerComp(m, 2));
    const float P_InnerConeAngle = RandomParam1(ParamF(m, 10), ParamF(m, 11), index, ParamSalt(m.Slot, 3));
    const float P_SpeedFalloff = RandomParam1(ParamF(m, 12), ParamF(m, 13), index, ParamSalt(m.Slot, 4));
    const float P_Distribution = RandomParam1(ParamF(m, 14), ParamF(m, 15), index, ParamSalt(m.Slot, 5));
    const uint P_LocalSpace = ParamU(m, 16);

    const float outerA = radians(clamp(PARAM(ConeAngle), 0.f, 180.f));
    const float innerA = radians(clamp(PARAM(InnerConeAngle), 0.f, clamp(PARAM(ConeAngle), 0.f, 180.f)));
    const float cosOuter = cos(outerA);
    const float cosInner = cos(innerA);

    const float u = pow(max(Random01(index, 3), 1e-6f), PARAM(Distribution));
    const float cosT = clamp(lerp(cosInner, cosOuter, u), -1.f, 1.f);
    const float sinT = sqrt(saturate(1.f - cosT * cosT));
    const float phi = Random01(index, 4) * 6.28318530f;

    const float axisFrac = (outerA > 0.f) ? acos(cosT) / outerA : 0.f;
    const float speed = PARAM(Speed) * saturate(1.f - PARAM(SpeedFalloff) * axisFrac);

    float3 n = normalize(PARAM(ConeAxis));
    if (PARAM(LocalSpace) != 0)
        n = normalize(mul(float4(n, 0.f), OwnerLocalToWorld).xyz);

    const float3 helper = (abs(n.y) < 0.999f) ? float3(0.f, 1.f, 0.f) : float3(1.f, 0.f, 0.f);
    const float3 tAxis = normalize(cross(helper, n));
    const float3 bAxis = cross(n, tAxis);

    const float3 dir = tAxis * (sinT * cos(phi)) + bAxis * (sinT * sin(phi)) + n * cosT;
    p.Velocity += dir * speed;
}

// ---- [ParticleUpdate] Particle State ----
void Module_ParticleState(inout Particle p, uint index, inout bool alive, ModuleInst m)
{
    const uint P_KillOnLifetime = ParamU(m, 0);

    p.Age += DeltaTime;
    const float t = (p.Lifetime > 0.f) ? p.Age / p.Lifetime : 1.f;
    p.NormalizedAge = min(t, 1.f);
    if (PARAM(KillOnLifetime) != 0 && t >= 1.f)
    	KILL();
}

// ---- [ParticleUpdate] Gravity Force ----
void Module_GravityForce(inout Particle p, uint index, ModuleInst m)
{
    const float3 P_Gravity = ParamF3(m, 0);

    p.Acceleration += PARAM(Gravity) / p.Mass;
}

// ---- [ParticleUpdate] Solve Force and Velocity ----
void Module_SolveForceandVelocity(inout Particle p, uint index, ModuleInst m)
{
    const float P_AccelLimit = ParamF(m, 0);
    const float P_SpeedLimit = ParamF(m, 1);

    float3 accel = p.Acceleration;
    const float aLen = length(accel);
    if (PARAM(AccelLimit) > 0.f && aLen > PARAM(AccelLimit))
        accel *= PARAM(AccelLimit) / aLen;

    p.Velocity += accel * DeltaTime;

    const float sLen = length(p.Velocity);
    if (PARAM(SpeedLimit) > 0.f && sLen > PARAM(SpeedLimit))
        p.Velocity *= PARAM(SpeedLimit) / sLen;

    p.Position += p.Velocity * DeltaTime;
    p.Acceleration = float3(0.f, 0.f, 0.f);
}

// ---- [ParticleUpdate] SubUV Animation ----
void Module_SubUVAnimation(inout Particle p, uint index, ModuleInst m)
{
    const float P_StartFrame = ParamF(m, 0);
    const float P_EndFrame = ParamF(m, 1);
    const uint P_Loop = ParamU(m, 2);
    const float P_FramesPerSecond = ParamU(m, 3);

    const float count = max(PARAM(EndFrame) - PARAM(StartFrame) + 1.f, 1.f);
    float t;
    if (PARAM(Loop) != 0)
        t = frac(p.Age * PARAM(FramesPerSecond) / count) * count;
    else
        t = min(p.NormalizedAge * count, count - 1.f);
    p.SubImageIndex = PARAM(StartFrame) + t;
}

#undef PARAM

// =====================================================================
// 7. 단계 함수. 실행 순서는 여기서 고정된다 (스택 순서와 다르면 3단계에서 경고)
//   Emitter Update : SpawnRate
//   Particle Spawn : InitializeParticle -> SphereLocation -> AddVelocityInCone
//   Particle Update: ParticleState -> GravityForce -> SolveForceandVelocity -> SubUVAnimation
// =====================================================================
void EmitterUpdate(inout EmitterState e)
{
    [loop]
    for (uint i = 0; i < EmitterInstCount; ++i)
    {
        const ModuleInst m = LoadInst(EmitterInstBegin + i);
        switch (m.Id)
        {
            case MODID_SPAWN_RATE:
                Module_SpawnRate(e, m);
                break;
            default:
                break;
        }
    }  

}

void ParticleSpawn(inout Particle p, uint index)
{
    [loop]
    for (uint i = 0; i < SpawnInstCount; ++i)
    {
        const ModuleInst m = LoadInst(SpawnInstBegin + i);
        switch (m.Id)
        {
            case MODID_INITIALIZE_PARTICLE:
                Module_InitializeParticle(p, index, m);
                break;
            case MODID_SPHERE_LOCATION:
                Module_SphereLocation(p, index, m);
                break;
            case MODID_ADD_VELOCITY_IN_CONE:
                Module_AddVelocityInCone(p, index, m);
                break;
        }
    }
}

void ParticleUpdate(inout Particle p, uint index, inout bool alive)
{
    [loop]
    for (uint i = 0; i < UpdateInstCount; ++i)
    {
        const ModuleInst m = LoadInst(UpdateInstBegin + i);
        switch (m.Id)
        {
            case MODID_PARTICLE_STATE:
                Module_ParticleState(p, index, alive, m);
            break;
            case MODID_GRAVITY_FORCE:
                Module_GravityForce(p, index, m);
                break;
            case MODID_SOLVE_FORCE_AND_VELOCITY:
                Module_SolveForceandVelocity(p, index, m);
                break;
             case MODID_SUBUV_ANIMATION:
                Module_SubUVAnimation(p, index, m);
                break;
        }
    }

}

// =====================================================================
// 8. 단계별 본문 (생성 방식의 *Main 과 같은 내용). 진입점은 ParticleUber*CS.hlsl 에 있다
// =====================================================================
void UberEmitterUpdate()
{
    EmitterState e = LoadEmitter();
    e.SpawnCount = 0; // spawn modules accumulate with +=
    EmitterUpdate(e);
    StoreEmitter(e);

    const uint request = min(e.SpawnCount, Capacity);
    Counters.Store(COUNTER_SPAWN_REQUEST, request);
    IndirectArgs.Store3(0, uint3((request + GROUP_SIZE - 1) / GROUP_SIZE, 1, 1));
}

void UberParticleUpdate(uint3 dtid)
{
    const uint i = dtid.x;
    if (i >= Counters.Load(COUNTER_ALIVE_IN))
        return;

    Particle p = LoadParticle(ParticlesIn, i);
    bool alive = true;
    ParticleUpdate(p, i, alive);
    if (!alive)
        return; // not written to Out = compaction

    uint dst;
    Counters.InterlockedAdd(COUNTER_WRITE_CURSOR, 1, dst);
    StoreParticle(ParticlesOut, dst, p);
}

void UberParticleSpawn(uint3 dtid)
{
    if (dtid.x >= Counters.Load(COUNTER_SPAWN_REQUEST))
        return;

    uint dst;
    Counters.InterlockedAdd(COUNTER_WRITE_CURSOR, 1, dst);
    if (dst >= Capacity)
        return; // overflow is clamped by Finalize

    Particle p = (Particle) 0; // new particles start from 0 (CPU ZeroRange)
    ParticleSpawn(p, dst); // random index = slot index (same as CPU)
    StoreParticle(ParticlesOut, dst, p);
}
