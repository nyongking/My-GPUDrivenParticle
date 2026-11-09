#include "pch.h"
#include "LayoutBuilder.h"

void Block::Add(const AttributeKey& key, const TypeInfo* type)
{
    // 정렬 맞추기 (현재는 모든 타입이 4바이트 정렬이라 실제로는 변화 없음)
    const uint32_t a = type->align;
    m_Stride = (m_Stride + a - 1) / a * a;

    if (m_Packing == Packing::ConstantBuffer)
    {
        const uint32_t start = m_Stride;
        const uint32_t end = start + type->size - 1;
        if (type->size > 16 || (start / 16u) != (end / 16u))
            m_Stride = (m_Stride + 15u) / 16u * 16u;
    }


    Slot slot;
    slot.key = key;
    slot.type = type;
    slot.offset = m_Stride;
    m_Slots.push_back(slot);

    m_Stride += type->size;
}

const Slot* Block::Find(const std::string& fullName) const
{
    for (const Slot& s : m_Slots)
        if (s.key.Full() == fullName) return &s;
    return nullptr;
}
