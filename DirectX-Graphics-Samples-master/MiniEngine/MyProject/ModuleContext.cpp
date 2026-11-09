#include "pch.h"
#include "ModuleContext.h"

namespace
{
    // 정수 해시 (입력이 조금만 달라도 결과가 크게 달라진다)
    uint32_t Hash(uint32_t x)
    {
        x ^= x >> 16; x *= 0x7feb352dU;
        x ^= x >> 15; x *= 0x846ca68bU;
        x ^= x >> 16;
        return x;
    }
}

ModuleContext::ModuleContext(const CompiledEmitter& compiled,
    uint8_t* particleData,
    uint8_t* emitterData,
    const EngineValues& engine,
    uint32_t               capacity,
    uint32_t               begin,
    uint32_t               end,
    uint8_t*                aliveFlags)
    : m_Compiled(compiled)
    , m_ParticleData(particleData)
    , m_EmitterData(emitterData)
    , m_Engine(engine)
    , m_Capacity(capacity)
    , m_Begin(begin)
    , m_End(end)
    , m_AliveFlags(aliveFlags)
{
}

float ModuleContext::Random01(uint32_t particleIndex, uint32_t salt) const
{
    const uint32_t h = Hash(m_Engine.RandomSeed ^ Hash(particleIndex * 0x9E3779B9U + salt));
    return float(h >> 8) * (1.0f / 16777216.0f);   // 상위 24비트 -> [0, 1)
}

void ModuleContext::Kill(uint32_t i)
{
    assert(m_AliveFlags != nullptr && "Kill 은 Particle Update 단계에서만 쓸 수 있음");
    assert(i >= m_Begin && i < m_End && "Kill: 처리 범위 밖의 파티클");

    m_AliveFlags[i] = 0;
}

const Slot* ModuleContext::FindChecked(const Block& block, const std::string& fullName, const TypeInfo& expected)
{
    const Slot* slot = block.Find(fullName);

    assert(slot != nullptr && "레이아웃에 없는 값: 모듈 선언(reads/writes)에 넣었는지, 누가 읽는지 확인");

    assert(slot->type == &expected && "타입이 다름: 선언한 타입과 꺼내는 타입을 맞출 것");

    (void)expected;
    return slot;
}
