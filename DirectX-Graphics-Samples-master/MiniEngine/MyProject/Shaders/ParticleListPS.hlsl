#include "SpriteDissolve.hlsli"
#include "ParticleListCommon.hlsli"
#define MAX_TEXTURES 16 // C++ ParticleRenderList::kMaxTextures

Texture2D<float4> SpriteTextures[MAX_TEXTURES] : register(t0, space1);
StructuredBuffer<EmitterInfo> Emitters : register(t2); // VS 와 같은 루트 SRV (PS 에서도 보이게 함)
SamplerState LinearClamp : register(s0);

struct PSInput
{
    float4 position : SV_Position;
    float4 color : COLOR0;
    float4 uv : TEXCOORD0;
    nointerpolation float blend : TEXCOORD1;
    nointerpolation uint textureIndex : TEXCOORD2;
    nointerpolation uint blendMode : TEXCOORD3;
    nointerpolation uint textureAlpha : TEXCOORD4;
    float2 quadUV : TEXCOORD5;
    nointerpolation uint emitterIndex : TEXCOORD6;
    nointerpolation float dissolve : TEXCOORD7;
};

float4 main(PSInput input) : SV_TARGET
{
    // 한 draw 안에서 이미터마다 텍스처가 달라서 non-uniform 인덱스
    const uint t = NonUniformResourceIndex(input.textureIndex);
    const float4 tex0 = SpriteTextures[t].Sample(LinearClamp, input.uv.xy);
    const float4 tex1 = SpriteTextures[t].Sample(LinearClamp, input.uv.zw);
    const float4 tex = lerp(tex0, tex1, input.blend);

    const float a = input.color.a * tex.a;
    const float3 rgb = input.color.rgb * tex.rgb * a;

    // premultiplied 블렌드: Additive 는 alpha 0 (뒤를 지우지 않고 더하기만)
    
    float4 result = ResolveSpriteColor(input.color, tex, input.textureAlpha, input.blendMode);

    // 디졸브: 이미터마다 켜짐 / 꺼짐이 달라서 이미터 정보에서 읽는다
    const EmitterInfo e = Emitters[input.emitterIndex];
    if (e.DissolveTexture != NO_TEXTURE)
    {
        DissolveParams d;
        d.Range = e.DissolveRange;
        d.EdgeWidth = e.DissolveEdgeWidth;
        d.Softness = e.DissolveSoftness;
        d.EdgeColor = e.DissolveEdgeColor;
        d.EdgeIntensity = e.DissolveEdgeIntensity;
        const float mask = SpriteTextures[NonUniformResourceIndex(e.DissolveTexture)].Sample(LinearClamp, input.quadUV).r;
        result = ApplyDissolve(result, SpriteCoverage(input.color, tex, input.textureAlpha), mask, input.dissolve, d);
    }
    return result;

}
