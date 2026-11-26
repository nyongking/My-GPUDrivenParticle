// 이미터 하나의 살아 있는 파티클 -> 공용 렌더 목록 (ParticleRenderList::Add)
//   입력은 이미터의 고정 렌더 구조체 (RenderParticle). 모든 이미터가 같은 형식이라 offset 이 필요 없다
//   빌보드 방향(Facing / Alignment / 회전 / 피벗)을 여기서 계산해서 두 축 벡터로 넘긴다 (ListParticle)
//   정렬 목록에는 uint2(목록 번호, 깊이 키) 를 쓴다. 자리는 공용 카운터에서 웨이브 단위로 한꺼번에 받는다
//   CullEnable 이면 프러스텀 밖 / 투명 / 크기 0 인 파티클은 목록에 넣지 않는다 (자리도 받지 않음)
#include "RenderParticle.hlsli"
#include "ParticleListCommon.hlsli"
#include "SpriteFacing.hlsli"

// C++ GatherConstants 와 같은 배치 (240 B)
cbuffer GatherConstants : register(b0)
{
    uint Capacity; // 0   이미터 버퍼 길이
    uint CountOffset; // 4   CountBuffer 안의 개수 위치
    uint ConstCount; // 8   CountBuffer 를 쓰지 않을 때 (CPU 이미터)
    uint UseCountBuffer; // 12
    uint EmitterIndex; // 16
    uint ListCapacity; // 20  공용 목록 크기
    uint SpriteFacingOffset; // 24  가변 속성(SoA) 의 Particles.SpriteFacing, NO_ATTRIBUTE = 기본값
    uint SpriteAlignmentOffset; // 28
    float3 CamPos; // 32
    uint FacingMode; // 44
    float3 CamForward; // 48
    uint AlignmentMode; // 60
    float3 CamRight; // 64
    float CameraOffset; // 76
    float3 CamUp; // 80
    float VelocityStretch; // 92
    float3 DefaultSpriteFacing; // 96
    float BlendNear; // 108
    float3 DefaultSpriteAlignment; // 112
    float BlendFar; // 124
    float2 Pivot; // 128
    uint CullEnable; // 136  0 = 컬링 안 함 (전부 append)
    uint _Pad0; // 140
    float4 FrustumPlanes[6]; // 144  월드 공간, 안쪽이 + (dot(n, p) + d >= 0), 정규화됨
    // 240
};

StructuredBuffer<RenderParticle> Particles : register(t0); // 고정 렌더 구조체
ByteAddressBuffer ParticleData : register(t1); // 가변 속성 (SoA). SpriteFacing / SpriteAlignment 만 읽는다
ByteAddressBuffer CountBuffer : register(t2);
RWByteAddressBuffer RenderParticles : register(u0);
RWByteAddressBuffer SortList : register(u1);
RWByteAddressBuffer Counter : register(u2);

// float -> 크기 순서가 같은 uint (음수 깊이 = 카메라 뒤도 순서가 맞게)
uint SortableKey(float f)
{
    const uint u = asuint(f);
    return (u & 0x80000000u) ? ~u : (u | 0x80000000u);
}

float3 ReadF3(uint ofs, uint i, float3 def)
{
    return (ofs == NO_ATTRIBUTE) ? def : asfloat(ParticleData.Load3(ofs * Capacity + i * 12));
}

// 그려도 화면에 안 보일 파티클인가 (보수적: 사각형을 감싸는 구가 평면 하나의 완전히 바깥일 때만)
bool IsCulled(ListParticle p)
{
    if (p.Color.a <= 0.f)
        return true;
    if (dot(p.AxisX, p.AxisX) <= 0.f || dot(p.AxisY, p.AxisY) <= 0.f)
        return true; // 넓이 0 (래스터화되지 않음)

    // 꼭짓점 = Center +- AxisX +- AxisY 이므로 반지름 |AxisX| + |AxisY| 가 사각형 전체를 감싼다
    const float radius = length(p.AxisX) + length(p.AxisY);
    [unroll]
    for (uint k = 0; k < 6; ++k)
    {
        if (dot(FrustumPlanes[k].xyz, p.Center) + FrustumPlanes[k].w < -radius)
            return true;
    }
    return false;
}

[numthreads(256, 1, 1)]
void main(uint3 dtid : SV_DispatchThreadID)
{
    const uint count = min(UseCountBuffer ? CountBuffer.Load(CountOffset) : ConstCount, Capacity);
    const uint i = dtid.x;
    const bool valid = i < count;

    // 1) 살아 있는 칸만 읽고 빌보드 축 계산 + 컬링 판정
    ListParticle p = (ListParticle) 0;
    float depth = 0.f;
    bool visible = false;
    if (valid)
    {
        const RenderParticle r = Particles[i];

        SpriteShape s;
        s.Position = r.Position;
        s.Size = RenderSpriteSize(r);
        s.Rotation = r.SpriteRotation;
        s.Velocity = r.Velocity;
        s.Facing = ReadF3(SpriteFacingOffset, i, DefaultSpriteFacing);
        s.Alignment = ReadF3(SpriteAlignmentOffset, i, DefaultSpriteAlignment);

        SpriteOptions o;
        o.Facing = FacingMode;
        o.Alignment = AlignmentMode;
        o.Pivot = Pivot;
        o.CameraOffset = CameraOffset;
        o.VelocityStretch = VelocityStretch;
        o.BlendNear = BlendNear;
        o.BlendFar = BlendFar;

        SpriteCamera cam;
        cam.Position = CamPos;
        cam.Right = CamRight;
        cam.Up = CamUp;
        cam.Forward = CamForward;

        ComputeSpriteAxes(s, o, cam, p.Center, p.AxisX, p.AxisY);
        p.Color = r.Color;
        p.SubImage = r.SubImageIndex;
        p.EmitterIndex = EmitterIndex;
        p.Params = float2(r.NormalizedAge, 0.f); // 머티리얼의 ParticleParam (ParticleSpriteVS 와 같음)

        // 깊이는 파티클 위치 기준 (피벗 / 카메라 오프셋과 관계없이 순서가 같게)
        depth = dot(s.Position - CamPos, CamForward);
        visible = (CullEnable == 0) || !IsCulled(p);
    }

    // 2) 웨이브에서 보이는 파티클 수만큼 한 번에 자리를 받는다 (분기 밖, 모든 레인이 같이 실행)
    const uint waveCount = WaveActiveCountBits(visible);
    uint waveBase = 0;
    if (WaveIsFirstLane() && waveCount > 0)
        Counter.InterlockedAdd(0, waveCount, waveBase);
    waveBase = WaveReadLaneFirst(waveBase);
    const uint slot = waveBase + WavePrefixCountBits(visible);

    // 3) 보이는 것만 쓴다
    if (!visible)
        return;
    if (slot >= ListCapacity)
        return; // 목록이 꽉 참 (Reserve 크기 = capacity 합이라 정상이면 오지 않음)

    StoreListParticle(RenderParticles, slot, p);
    SortList.Store2(slot * 8, uint2(slot, SortableKey(depth)));
}
