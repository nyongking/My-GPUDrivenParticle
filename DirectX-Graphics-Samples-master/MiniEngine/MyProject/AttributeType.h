
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

inline float3 operator+(const float3& a, const float3& b) { return float3{a.x + b.x, a.y + b.y, a.z + b.z}; }
inline float3 operator*(const float3& a, float s) { return float3{ a.x * s, a.y * s, a.z * s }; }
inline float3 operator/(const float3& a, float s) { return a * (1 / s); }

inline float3& operator+=(float3& a, const float3& b) { a.x += b.x; a.y += b.y; a.z += b.z; return a; }


inline float Dot(const float3& a, const float3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline float Length(const float3& a) { return sqrtf(Dot(a, a)); }
inline float3 Normalize(const float3& a) { return a / Length(a); }

inline float3 Cross(const float3& a, const float3& b)
{
    return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x };
}
inline float3 TransformVector(const float4x4& m, const float3& v)
{
    return {
        v.x * m.m[0][0] + v.y * m.m[1][0] + v.z * m.m[2][0],
        v.x * m.m[0][1] + v.y * m.m[1][1] + v.z * m.m[2][1],
        v.x * m.m[0][2] + v.y * m.m[1][2] + v.z * m.m[2][2] };
}

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
