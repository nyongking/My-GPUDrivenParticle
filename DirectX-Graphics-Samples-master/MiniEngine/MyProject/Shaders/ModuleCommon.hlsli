#define GROUP_SIZE 64

cbuffer EngineConstants : register(b0)
{
    float3 OwnerPosition;
    float DeltaTime;
    uint Capacity;
    uint RandomSeed;
    uint2 _pad;
    float3 OwnerVelocity;
    float _pad1;
    row_major float4x4 OwnerLocalToWorld;
}; 

RWByteAddressBuffer ParticlesIn : register(u0);
RWByteAddressBuffer ParticlesOut : register(u1);
RWByteAddressBuffer Counters : register(u2);
RWByteAddressBuffer EmitterData : register(u3);
RWByteAddressBuffer IndirectArgs : register(u4);

// Counters에서 읽는 주소 공간
#define COUNTER_ALIVE_IN      0   // ParticlesIn 에 들어 있는 개수
#define COUNTER_WRITE_CURSOR  4   // ParticlesOut 에 다음으로 쓸 자리 (InterlockedAdd)
#define COUNTER_SPAWN_REQUEST 8   // 이번 프레임 생성 요청 수 (EmitterUpdate 가 씀)

uint ColumnAddress(uint attrOffset, uint typeSize, uint idx)
{
    return attrOffset * Capacity + idx * typeSize;
}

float LoadF1(RWByteAddressBuffer buf, uint off, uint idx)
{
    return asfloat(buf.Load(ColumnAddress(off, 4, idx)));
}

float2 LoadF2(RWByteAddressBuffer b, uint off, uint idx)
{
    return asfloat(b.Load2(ColumnAddress(off, 8, idx)));
}

float3 LoadF3(RWByteAddressBuffer b, uint off, uint idx)
{
    return asfloat(b.Load3(ColumnAddress(off, 12, idx)));
}

float4 LoadF4(RWByteAddressBuffer b, uint off, uint idx)
{
    return asfloat(b.Load4(ColumnAddress(off, 16, idx)));
}

uint LoadU1(RWByteAddressBuffer b, uint off, uint idx)
{
    return asuint(b.Load(ColumnAddress(off, 4, idx)));
}

int LoadI1(RWByteAddressBuffer b, uint off, uint idx)
{
    return asint(b.Load(ColumnAddress(off, 4, idx)));
}

void StoreF1(RWByteAddressBuffer b, uint off, uint idx, float v)
{
    b.Store(ColumnAddress(off, 4, idx), asuint(v));
}

void StoreF2(RWByteAddressBuffer b, uint off, uint idx, float2 v)
{
    b.Store2(ColumnAddress(off, 8, idx), asuint(v));
}

void StoreF3(RWByteAddressBuffer b, uint off, uint idx, float3 v)
{
    b.Store3(ColumnAddress(off, 12, idx), asuint(v));
}

void StoreF4(RWByteAddressBuffer b, uint off, uint idx, float4 v)
{
    b.Store4(ColumnAddress(off, 16, idx), asuint(v));
}

void StoreU1(RWByteAddressBuffer b, uint off, uint idx, uint v)
{
    b.Store(ColumnAddress(off, 4, idx), v);
}

void StoreI1(RWByteAddressBuffer b, uint off, uint idx, int v)
{
    b.Store(ColumnAddress(off, 4, idx), asuint(v));
}

uint Hash(uint x)
{
    x ^= x >> 16;
    x *= 0x7feb352dU;
    x ^= x >> 15;
    x *= 0x846ca68bU;
    x ^= x >> 16;
    return x;
}

float Random01(uint x, uint salt)
{
    const uint h = Hash(RandomSeed ^ Hash(x * 0x9E3779B9U + salt));
    return float(h >> 8) * (1.0f / 16777216.0f); // 상위 24비트 -> [0, 1)
}

// ---------------------------------------------------------------------
// 커브 입력값 (C++ CurveLibrary 와 같이 고친다)
//   전역 커브 버퍼 하나를 모든 이미터가 같이 쓴다. 커브 하나 = CURVE_SAMPLES 개의 float4
//   입력값 자리에는 칸 번호만 들어 있다. float / float2 / float3 는 앞 성분만 쓴다
//   루트 SRV 는 범위 밖 읽기를 막아 주지 않으므로 칸 번호를 한 번 자른다
// ---------------------------------------------------------------------
#define CURVE_SAMPLES    128
#define CURVE_MAX_CURVES 1024

StructuredBuffer<float4> CurveData : register(t1);

float4 SampleCurve(uint slot, float t)
{
    const uint base = min(slot, CURVE_MAX_CURVES - 1) * CURVE_SAMPLES;
    const float u = saturate(t) * (CURVE_SAMPLES - 1);
    const uint i0 = (uint) u;
    const uint i1 = min(i0 + 1, CURVE_SAMPLES - 1);
    const float4 a = CurveData[base + i0];
    const float4 b = CurveData[base + i1];
    return a + (b - a) * (u - (float) i0);
}

#define KILL() (alive = false)

// ---------------------------------------------------------------------
// 컬 노이즈 (Curl Noise Force). C++ CurlNoise.h 와 같은 식으로 같이 고친다
//   벡터 퍼텐셜 Psi = 채널마다 salt 가 다른 3D 그래디언트 노이즈 3 개
//   각 노이즈의 도함수를 해석적으로 같이 구해 curl(Psi) 를 만든다 (차분 없이 노이즈 3 번)
//   curl 이라 발산이 0: 파티클이 한 점으로 모이거나 흩어지지 않고 소용돌이친다
//   반환값은 노이즈 공간의 도함수라 Frequency 를 바꿔도 크기가 그대로다 (세기는 Strength 로만)
// ---------------------------------------------------------------------
#define CURL_SALT_X 0x68E31DA4u
#define CURL_SALT_Y 0xB5297A4Du
#define CURL_SALT_Z 0x1B56C4E9u

// 격자점의 그래디언트 (각 성분 -1 ~ 1)
float3 NoiseGradient(int3 c, uint salt)
{
    const uint h = Hash(uint(c.x) + Hash(uint(c.y) + Hash(uint(c.z) + salt)));
    return float3(h & 0x3FFu, (h >> 10) & 0x3FFu, (h >> 20) & 0x3FFu) * (2.f / 1023.f) - 1.f;
}

// 3D 그래디언트 노이즈의 도함수 (quintic 보간, 값은 버리고 기울기만)
float3 GradientNoiseDeriv(float3 x, uint salt)
{
    const float3 f = floor(x);
    const int3 i = int3(f);
    const float3 w = x - f;
    const float3 u = w * w * w * (w * (w * 6.f - 15.f) + 10.f);
    const float3 du = 30.f * w * w * (w * (w - 2.f) + 1.f);

    const float3 ga = NoiseGradient(i + int3(0, 0, 0), salt);
    const float3 gb = NoiseGradient(i + int3(1, 0, 0), salt);
    const float3 gc = NoiseGradient(i + int3(0, 1, 0), salt);
    const float3 gd = NoiseGradient(i + int3(1, 1, 0), salt);
    const float3 ge = NoiseGradient(i + int3(0, 0, 1), salt);
    const float3 gf = NoiseGradient(i + int3(1, 0, 1), salt);
    const float3 gg = NoiseGradient(i + int3(0, 1, 1), salt);
    const float3 gh = NoiseGradient(i + int3(1, 1, 1), salt);

    const float va = dot(ga, w - float3(0.f, 0.f, 0.f));
    const float vb = dot(gb, w - float3(1.f, 0.f, 0.f));
    const float vc = dot(gc, w - float3(0.f, 1.f, 0.f));
    const float vd = dot(gd, w - float3(1.f, 1.f, 0.f));
    const float ve = dot(ge, w - float3(0.f, 0.f, 1.f));
    const float vf = dot(gf, w - float3(1.f, 0.f, 1.f));
    const float vg = dot(gg, w - float3(0.f, 1.f, 1.f));
    const float vh = dot(gh, w - float3(1.f, 1.f, 1.f));

    const float kxy = va - vb - vc + vd;
    const float kyz = va - vc - ve + vg;
    const float kzx = va - vb - ve + vf;
    const float kxyz = -va + vb + vc - vd + ve - vf - vg + vh;

    float3 d = ga + u.x * (gb - ga) + u.y * (gc - ga) + u.z * (ge - ga)
        + u.x * u.y * (ga - gb - gc + gd) + u.y * u.z * (ga - gc - ge + gg) + u.z * u.x * (ga - gb - ge + gf)
        + u.x * u.y * u.z * (-ga + gb + gc - gd + ge - gf - gg + gh);
    d += du * float3(
        vb - va + u.y * kxy + u.z * kzx + u.y * u.z * kxyz,
        vc - va + u.z * kyz + u.x * kxy + u.z * u.x * kxyz,
        ve - va + u.x * kzx + u.y * kyz + u.x * u.y * kxyz);
    return d;
}

float3 CurlNoise(float3 x)
{
    const float3 dPx = GradientNoiseDeriv(x, CURL_SALT_X);
    const float3 dPy = GradientNoiseDeriv(x, CURL_SALT_Y);
    const float3 dPz = GradientNoiseDeriv(x, CURL_SALT_Z);
    return float3(dPz.y - dPy.z, dPx.z - dPz.x, dPy.x - dPx.y);
}
