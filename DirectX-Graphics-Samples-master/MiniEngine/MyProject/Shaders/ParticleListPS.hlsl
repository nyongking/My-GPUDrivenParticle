#include "SpriteAlpha.hlsli"
#define MAX_TEXTURES 16 // C++ ParticleRenderList::kMaxTextures

Texture2D<float4> SpriteTextures[MAX_TEXTURES] : register(t0, space1);
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
    
    return ResolveSpriteColor(input.color, tex, input.textureAlpha, input.blendMode);

}
