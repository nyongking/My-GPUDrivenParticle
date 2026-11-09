#pragma once

#include "AttributeKey.h"
#include "AttributeType.h"


struct Slot
{
    AttributeKey    key;
    const TypeInfo* type;
    uint32_t        offset;
};

// 데이터의 배치 규칙
// Tight : 4바이트 단위로 이어서 배치
// Constant Buffer : 16바이트 단위로 데이터를 끊어서 사용
enum class Packing { Tight, ConstantBuffer };

class Block
{
public:
    Block() = default;
    explicit Block(Packing packing) : m_Packing(packing) {}

    Packing GetPacking() const { return m_Packing; }

    uint32_t ConstantBufferSize() const { return (m_Stride + 15u) / 16u * 16u; }

    // 다음 빈 자리(정렬 맞춤)에 배정하고 Stride 를 늘린다
    void Add(const AttributeKey& key, const TypeInfo* type);

    // 전체 이름으로 찾기 (Bind 때 한 번만 사용). 없으면 nullptr
    const Slot* Find(const std::string& fullName) const;

    const std::vector<Slot>& Slots() const  { return m_Slots; }

    // 파티클 하나가 차지하는 바이트 수 (= "파티클 구조체 크기")
    uint32_t Stride() const                 { return m_Stride; }

private:
    std::vector<Slot> m_Slots;
    uint32_t          m_Stride = 0;
    Packing           m_Packing = Packing::Tight;
};

// 엔진이 자동으로 넣는 복사, SoA 줄 단위로 통째로 복사한다.
struct CopyOp 
{
    std::string from, to;
    uint32_t    srcOffset;
    uint32_t    dstOffset;
    uint32_t    size;       // 원소 하나 크기 (바이트)
};