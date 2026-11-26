#include "RenderParticle.hlsli"
#include "SpriteFacing.hlsli"

#define NO_ATTRIBUTE 0xFFFFFFFF

// C++ SpriteConstants 와 같은 배치 (224 B)
//   파티클 값은 고정 구조체 (RenderParticle) 에서 읽으므로 속성 offset / 기본값이 없다.
//   예외: Custom Facing / Custom Alignment 벡터는 일부 모드에서만 쓰여서 가변 속성 버퍼(SoA) 에 그대로 있다
cbuffer SpriteConstants : register(b0)
{
    float4x4 ViewProj; // 0
    float4 CamRight; // 64
    float4 CamUp; // 80
    float4 CamPosition; // 96
    float4 CamForward; // 112

    uint SortEnabled; // 128 1 = sort, 0 = not sorted
    uint SortStride; // 132 정렬 목록 한 칸 바이트 (CPU 4 = uint index / GPU 8 = uint2(index, key))
    uint SubUVBlend; // 136 1 = 다음 프레임과 섞기
    uint FacingMode; // 140

    float2 SubImageSize; // 144 (가로 칸, 세로 칸)
    float2 Pivot; // 152

    uint AlignmentMode; // 160
    float CameraOffset; // 164
    float VelocityStretch; // 168
    float BlendNear; // 172

    float BlendFar; // 176
    uint Capacity; // 180 가변 속성 버퍼의 SoA 열 길이
    uint SpriteFacingOffset; // 184 Particles.SpriteFacing (없으면 NO_ATTRIBUTE -> 기본값)
    uint SpriteAlignmentOffset; // 188 Particles.SpriteAlignment

    float3 DefaultSpriteFacing; // 192
    float _Pad0; // 204
    float3 DefaultSpriteAlignment; // 208
    float _Pad1; // 220
    // 224
};

StructuredBuffer<RenderParticle> Particles : register(t0); // 고정 렌더 구조체
ByteAddressBuffer SortedIndices : register(t1); // 칸의 첫 uint = 파티클 번호 (GPU 는 뒤에 키가 붙음)
ByteAddressBuffer ParticleData : register(t2); // 가변 속성 (SoA). SpriteFacing / SpriteAlignment 만 읽는다


struct VSOutput
{
    float4 position : SV_Position;
    float4 color : COLOR0;
    float4 uv : TEXCOORD0; // xy = 현재 프레임 uv, zw = 다음 프레임 uv
    float2 quadUV : TEXCOORD1;
    nointerpolation float blend : TEXCOORD2; // 다음 프레임 비율 (0 = 현재 프레임만)
    nointerpolation float2 params : TEXCOORD3;
};

static const float2 kCorners[6] =
{
    float2(-1.f, 1.f), float2(1.f, 1.f), float2(1.f, -1.f),
    float2(-1.f, 1.f), float2(1.f, -1.f), float2(-1.f, -1.f)
};

// 가변 속성 (SoA): 칸이 연결되어 있으면 버퍼에서, 아니면 기본값 (cbuffer 값이라 draw 전체가 같은 쪽으로 분기)
float3 ReadF3(uint ofs, uint idx, float3 def)
{
    return (ofs == NO_ATTRIBUTE) ? def : asfloat(ParticleData.Load3(ofs * Capacity + idx * 12));
}

// SubUV: 프레임 번호 -> 격자 칸 안의 uv (왼쪽 위 칸부터 가로로 0, 1, 2 ...)
float2 SubImageUV(uint frame, float2 uv)
{
    const uint cols = (uint) SubImageSize.x;
    return (float2(frame % cols, frame / cols) + uv) / SubImageSize;
}

VSOutput main(uint vertexId : SV_VertexID, uint instanceId : SV_InstanceID)
{
    const uint idx = SortEnabled ? SortedIndices.Load(instanceId * SortStride) : instanceId;

    const RenderParticle r = Particles[idx];

    SpriteShape s;
    s.Position = r.Position;
    s.Size = RenderSpriteSize(r);
    s.Rotation = r.SpriteRotation;
    s.Velocity = r.Velocity;
    s.Facing = ReadF3(SpriteFacingOffset, idx, DefaultSpriteFacing);
    s.Alignment = ReadF3(SpriteAlignmentOffset, idx, DefaultSpriteAlignment);

    SpriteOptions o;
    o.Facing = FacingMode;
    o.Alignment = AlignmentMode;
    o.Pivot = Pivot;
    o.CameraOffset = CameraOffset;
    o.VelocityStretch = VelocityStretch;
    o.BlendNear = BlendNear;
    o.BlendFar = BlendFar;

    SpriteCamera cam;
    cam.Position = CamPosition.xyz;
    cam.Right = CamRight.xyz;
    cam.Up = CamUp.xyz;
    cam.Forward = CamForward.xyz;

    float3 center, axisX, axisY;
    ComputeSpriteAxes(s, o, cam, center, axisX, axisY);

    const float2 corner = kCorners[vertexId];
    const float3 worldPos = center + corner.x * axisX + corner.y * axisY;

    VSOutput output;
    output.position = mul(ViewProj, float4(worldPos, 1.f));
    output.color = r.Color;
    const float2 uv = corner * float2(0.5f, -0.5f) + 0.5f; // 왼쪽 위 (0,0) ~ 오른쪽 아래 (1,1)

    // QuadUV : 일반 사각 Sprite의 UV 좌표
    output.quadUV = uv;

    // SubUV: 정수부 = 프레임, 소수부 = 다음 프레임으로 넘어가는 비율. 칸 수를 넘으면 처음으로 돈다
    const uint total = (uint) SubImageSize.x * (uint) SubImageSize.y;
    const float index = max(r.SubImageIndex, 0.f);
    const uint frame0 = (uint) index % total;
    const uint frame1 = (frame0 + 1) % total;
    output.uv = float4(SubImageUV(frame0, uv), SubImageUV(frame1, uv));
    output.blend = SubUVBlend ? frac(index) : 0.f;
    // 머티리얼의 ParticleParam: x = NormalizedAge, y = 0 (고정)
    output.params = float2(r.NormalizedAge, 0.f);
    return output;
}
