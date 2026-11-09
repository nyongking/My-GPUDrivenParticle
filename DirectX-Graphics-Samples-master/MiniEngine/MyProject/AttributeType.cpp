#include "pch.h"
#include "AttributeType.h"

// C++ 구조체 크기가 TypeInfo 에 적은 크기와 같은지 컴파일 때 확인
static_assert(sizeof(float2) == 8, "float2 must be 8 bytes");
static_assert(sizeof(float3) == 12, "float3 must be 12 bytes");
static_assert(sizeof(float4) == 16, "float4 must be 16 bytes");
static_assert(sizeof(NBool) == 4, "NBool must be 4 bytes");

namespace Types
{
    const TypeInfo Float{ "float",     4,  4 };
    const TypeInfo Float2{ "float2",    8,  4 };
    const TypeInfo Float3{ "float3",    12, 4 };
    const TypeInfo Float4{ "float4",    16, 4 };
    const TypeInfo Int32{ "int",       4,  4 };
    const TypeInfo UInt32{ "uint",      4,  4 };
    const TypeInfo Bool{ "bool",      4,  4 };

    bool IsGpuCompatible(const TypeInfo& t, std::string* whyNot)
    {
        if (t.size == 0 || t.size % 4 != 0)
        {
            if (whyNot) *whyNot = "크기 " + std::to_string(t.size) + "바이트가 4의 배수가 아님";
            return false;
        }
        if (t.align != 4)
        {
            if (whyNot) *whyNot = "정렬 " + std::to_string(t.align) + "바이트 (4바이트만 허용)";
            return false;
        }
        return true;
    }
}

template <> const TypeInfo& TypeOf<float>() { return Types::Float; }
template <> const TypeInfo& TypeOf<float2>() { return Types::Float2; }
template <> const TypeInfo& TypeOf<float3>() { return Types::Float3; }
template <> const TypeInfo& TypeOf<float4>() { return Types::Float4; }
template <> const TypeInfo& TypeOf<int32_t>() { return Types::Int32; }
template <> const TypeInfo& TypeOf<uint32_t>() { return Types::UInt32; }
template <> const TypeInfo& TypeOf<NBool>() { return Types::Bool; }
