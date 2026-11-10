
#pragma once

struct float2 { float x, y; };
struct float3 { float x, y, z; };
struct float4 { float x, y, z, w; };

struct float4x4 { float m[4][4]; };

struct NBool
{
    int32_t value = 0;

    NBool() = default;
    NBool(bool b) : value(b ? 1 : 0) {}
    explicit operator bool() const { return value != 0; }
};

struct TypeInfo
{
    std::string name;
    uint32_t    size;    // 바이트 수
    uint32_t    align;   // 정렬 (바이트)
};


enum class AttrType 
{ 
    Float, 
    Float2, 
    Float3, 
    Float4,
    Int
};

namespace Types
{
    extern const TypeInfo Float;
    extern const TypeInfo Float2;
    extern const TypeInfo Float3;
    extern const TypeInfo Float4;
    extern const TypeInfo Int32;
    extern const TypeInfo UInt32;
    extern const TypeInfo Bool;   // NBool
    extern const TypeInfo Float4x4; // 상수버퍼 전용의 행렬

    // GPU 버퍼로 그대로 옮길 수 있는지 (크기 4의 배수, 정렬 4)
    bool IsGpuCompatible(const TypeInfo& t, std::string* whyNot = nullptr);
}

template <typename T> const TypeInfo& TypeOf();

template <> const TypeInfo& TypeOf<float>();
template <> const TypeInfo& TypeOf<float2>();
template <> const TypeInfo& TypeOf<float3>();
template <> const TypeInfo& TypeOf<float4>();
template <> const TypeInfo& TypeOf<int32_t>();
template <> const TypeInfo& TypeOf<uint32_t>();
template <> const TypeInfo& TypeOf<NBool>();
template <> const TypeInfo& TypeOf<float4x4>();
