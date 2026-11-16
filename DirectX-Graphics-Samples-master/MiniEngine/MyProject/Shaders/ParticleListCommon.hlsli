// ParticleRenderList 공용 정의 (Gather CS / List VS 가 같이 쓴다)

#define NO_ATTRIBUTE 0xFFFFFFFF

// RenderParticle: 64 바이트 (C++ ParticleRenderList::kRenderParticleBytes)
//   빌보드 방향은 Gather 가 미리 계산한다 (이미터마다 Facing / Alignment 가 달라도 VS 는 같은 식)
//   0  float3 Center     12 float SubImage
//   16 float4 Color
//   32 float3 AxisX      44 uint  EmitterIndex
//   48 float3 AxisY      60 float Dissolve (디졸브 바인딩 값)
#define RENDER_PARTICLE_BYTES 64

struct RenderParticle
{
    float3 Center; // 사각형 중심 (피벗 / 카메라 오프셋 적용 후)
    float SubImage;
    float4 Color;
    float3 AxisX; // 가로 반 축 (corner.x 를 곱함)
    uint EmitterIndex;
    float3 AxisY; // 세로 반 축
    float Dissolve;
};

void StoreRenderParticle(RWByteAddressBuffer buf, uint slot, RenderParticle p)
{
    const uint a = slot * RENDER_PARTICLE_BYTES;
    buf.Store4(a + 0, uint4(asuint(p.Center), asuint(p.SubImage)));
    buf.Store4(a + 16, asuint(p.Color));
    buf.Store4(a + 32, uint4(asuint(p.AxisX), p.EmitterIndex));
    buf.Store4(a + 48, uint4(asuint(p.AxisY), asuint(p.Dissolve)));
}

RenderParticle LoadRenderParticle(ByteAddressBuffer buf, uint slot)
{
    const uint a = slot * RENDER_PARTICLE_BYTES;
    const uint4 v0 = buf.Load4(a + 0);
    const uint4 v1 = buf.Load4(a + 16);
    const uint4 v2 = buf.Load4(a + 32);
    const uint4 v3 = buf.Load4(a + 48);
    RenderParticle p;
    p.Center = asfloat(v0.xyz);
    p.SubImage = asfloat(v0.w);
    p.Color = asfloat(v1);
    p.AxisX = asfloat(v2.xyz);
    p.EmitterIndex = v2.w;
    p.AxisY = asfloat(v3.xyz);
    p.Dissolve = asfloat(v3.w);
    return p;
}

#define NO_TEXTURE 0xFFFFFFFF

// 이미터별 그리기 설정 (C++ ParticleRenderList::EmitterInfo, 64 바이트. StructuredBuffer 라 빈틈 없이 붙는다)
struct EmitterInfo
{
    uint SubImageCols;
    uint SubImageRows;
    uint SubUVBlend;
    uint BlendMode; // 0 = Translucent, 1 = Additive
    uint TextureIndex;
    uint TextureAlpha;
    uint DissolveTexture; // 텍스처 표 번호, NO_TEXTURE = 디졸브 꺼짐
    float DissolveEdgeWidth;
    float2 DissolveRange;
    float DissolveSoftness;
    float DissolveEdgeIntensity;
    float3 DissolveEdgeColor;
    uint _Pad;
};
