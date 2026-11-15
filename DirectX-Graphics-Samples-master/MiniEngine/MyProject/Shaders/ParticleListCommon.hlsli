// ParticleRenderList 공용 정의 (Gather CS / List VS 가 같이 쓴다)

#define NO_ATTRIBUTE 0xFFFFFFFF

// RenderParticle: 48 바이트 (C++ ParticleRenderList::kRenderParticleBytes)
//   0  float3 Position   12 float Size
//   16 float4 Color
//   32 float  SubImage   36 uint  EmitterIndex   40 uint2 (pad)
#define RENDER_PARTICLE_BYTES 48

struct RenderParticle
{
    float3 Position;
    float Size;
    float4 Color;
    float SubImage;
    uint EmitterIndex;
};

void StoreRenderParticle(RWByteAddressBuffer buf, uint slot, RenderParticle p)
{
    const uint a = slot * RENDER_PARTICLE_BYTES;
    buf.Store4(a + 0, uint4(asuint(p.Position), asuint(p.Size)));
    buf.Store4(a + 16, asuint(p.Color));
    buf.Store4(a + 32, uint4(asuint(p.SubImage), p.EmitterIndex, 0, 0));
}

RenderParticle LoadRenderParticle(ByteAddressBuffer buf, uint slot)
{
    const uint a = slot * RENDER_PARTICLE_BYTES;
    const uint4 v0 = buf.Load4(a + 0);
    const uint4 v1 = buf.Load4(a + 16);
    const uint2 v2 = buf.Load2(a + 32);
    RenderParticle p;
    p.Position = asfloat(v0.xyz);
    p.Size = asfloat(v0.w);
    p.Color = asfloat(v1);
    p.SubImage = asfloat(v2.x);
    p.EmitterIndex = v2.y;
    return p;
}

// 이미터별 그리기 설정 (C++ ParticleRenderList::EmitterInfo, 32 바이트)
struct EmitterInfo
{
    uint SubImageCols;
    uint SubImageRows;
    uint SubUVBlend;
    uint BlendMode; // 0 = Translucent, 1 = Additive
    uint TextureIndex;
    uint3 _Pad;
};
