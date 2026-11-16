// 공용 렌더 목록 그리기 (ParticleRenderList::Render). 인스턴스 = 정렬된 목록의 칸 (먼 것부터)
//   빌보드 축은 Gather 가 이미 계산했다 (Center + corner.x * AxisX + corner.y * AxisY)
//   SubUV 계산은 ParticleSpriteVS 와 같다. 이미터별 설정은 EmitterInfo 에서 읽는다
#include "ParticleListCommon.hlsli"

cbuffer ListDrawConstants : register(b0)
{
    float4x4 ViewProj;
};

ByteAddressBuffer RenderParticles : register(t0);
ByteAddressBuffer SortList : register(t1); // uint2(목록 번호, 키)
StructuredBuffer<EmitterInfo> Emitters : register(t2);

struct VSOutput
{
    float4 position : SV_Position;
    float4 color : COLOR0;
    float4 uv : TEXCOORD0; // xy = 현재 프레임 uv, zw = 다음 프레임 uv
    nointerpolation float blend : TEXCOORD1; // 다음 프레임 비율
    nointerpolation uint textureIndex : TEXCOORD2;
    nointerpolation uint blendMode : TEXCOORD3;
    nointerpolation uint textureAlpha : TEXCOORD4;
    float2 quadUV : TEXCOORD5; // 사각형 uv (레이어용, SubUV 와 무관)
    nointerpolation uint emitterIndex : TEXCOORD6; // PS 가 EmitterInfo / 레이어를 읽는다
    nointerpolation float2 params : TEXCOORD7; // MaterialParam0 / 1
};

static const float2 kCorners[6] =
{
    float2(-1.f, 1.f), float2(1.f, 1.f), float2(1.f, -1.f),
    float2(-1.f, 1.f), float2(1.f, -1.f), float2(-1.f, -1.f)
};

float2 SubImageUV(uint frame, float2 uv, float2 grid)
{
    const uint cols = (uint) grid.x;
    return (float2(frame % cols, frame / cols) + uv) / grid;
}

VSOutput main(uint vertexId : SV_VertexID, uint instanceId : SV_InstanceID)
{
    const uint slot = SortList.Load(instanceId * 8);
    const RenderParticle p = LoadRenderParticle(RenderParticles, slot);
    const EmitterInfo e = Emitters[p.EmitterIndex];

    const float2 corner = kCorners[vertexId];
    const float3 worldPos = p.Center + corner.x * p.AxisX + corner.y * p.AxisY;

    VSOutput output;
    output.position = mul(ViewProj, float4(worldPos, 1.f));
    output.color = p.Color;
    const float2 uv = corner * float2(0.5f, -0.5f) + 0.5f;

    const float2 grid = float2(e.SubImageCols, e.SubImageRows);
    const uint total = e.SubImageCols * e.SubImageRows;
    const float index = max(p.SubImage, 0.f);
    const uint frame0 = (uint) index % total;
    const uint frame1 = (frame0 + 1) % total;
    output.uv = float4(SubImageUV(frame0, uv, grid), SubImageUV(frame1, uv, grid));
    output.blend = e.SubUVBlend ? frac(index) : 0.f;
    output.textureIndex = e.TextureIndex;
    output.blendMode = e.BlendMode;
    output.textureAlpha = e.TextureAlpha;
    output.quadUV = uv;
    output.emitterIndex = p.EmitterIndex;
    output.params = UnpackMaterialParams(p.MaterialParams);
    return output;
}
