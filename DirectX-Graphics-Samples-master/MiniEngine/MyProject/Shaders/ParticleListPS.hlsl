#include "SpriteAlpha.hlsli"
#include "ParticleListCommon.hlsli"
#include "MaterialVMDefs.hlsli"

#define MAX_TEXTURES 32 // C++ ParticleRenderList::kMaxTextures

Texture2D<float4> SpriteTextures[MAX_TEXTURES] : register(t0, space1);
StructuredBuffer<EmitterInfo> Emitters : register(t2);
StructuredBuffer<float4> MaterialParams : register(t3);
StructuredBuffer<uint4> InstrWords : register(t4);

SamplerState LinearClamp : register(s0);
SamplerState LinearWrap : register(s1);

cbuffer ListDrawConstants : register(b0)
{
    float4x4 ViewProj;
    float Time;
    float3 _pad;
};

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
    nointerpolation float2 params : TEXCOORD7;    
};

struct MaterialInput
{
    float2 quadUV;
    float4 baseColor;
    float coverage;
    float2 particleParams;
    float4 subUV;
    float blend;
    float4 vertexColor;
    // 이 픽셀의 이미터 (EmitterInfo 에서 미리 꺼내 둔다)
    uint paramBase;
    uint instrBase;
    uint baseTexture;
    uint4 effectTextures;
    float2 cellSize;
    uint textureAlpha;
    uint blendMode;
};

// Base 텍스처 읽기
float4 SampleBaseList(uint textureIndex, float4 subUV, float2 quadUV, float blend, float2 cellSize, float2 quadOffset)
{
    const uint t = NonUniformResourceIndex(textureIndex);   // 스레드마다 다를 수 있는 텍스처 인덱스
    const float2 offset = quadOffset * cellSize;
    const float2 origin0 = subUV.xy - quadUV * cellSize;
    const float2 origin1 = subUV.zw - quadUV * cellSize;
    const float2 uv0 = clamp(subUV.xy + offset, origin0, origin0 + cellSize);
    const float2 uv1 = clamp(subUV.zw + offset, origin1, origin1 + cellSize);
    const float4 tex0 = SpriteTextures[t].Sample(LinearClamp, uv0);
    const float4 tex1 = SpriteTextures[t].Sample(LinearClamp, uv1);
    return lerp(tex0, tex1, blend);
}

float BaseCoverage(float4 color, float4 tex, uint textureAlpha)
{
    const float texAlpha = (textureAlpha == ALPHA_FROM_LUMINANCE) ? max(tex.r, max(tex.g, tex.b)) : tex.a;
    return saturate(color.a * texAlpha);
}

float4 Param(MaterialInput i, uint k)
{
    return MaterialParams[i.paramBase + k];
}

float4 SampleTex(MaterialInput i, uint slot, float2 uv)
{
    return SpriteTextures[NonUniformResourceIndex(i.effectTextures[slot])].Sample(LinearWrap, uv);
}

float4 SampleBaseOffset(MaterialInput i, float2 quadOffset)
{
    const float4 tex = SampleBaseList(i.baseTexture, i.subUV, i.quadUV, i.blend, i.cellSize, quadOffset);
    return ResolveSpriteColor(i.vertexColor, tex, i.textureAlpha, i.blendMode);
}

void LoadInstr(MaterialInput i, uint k, out uint4 w0, out uint4 w1)
{
    const uint at = (i.instrBase + k) * 2;
    w0 = InstrWords[at];
    w1 = InstrWords[at + 1];
}

#include "MaterialVM.hlsli"

float4 main(PSInput input) : SV_TARGET
{
    const EmitterInfo e = Emitters[input.emitterIndex];
    const float2 cellsize = 1.f / float2(e.SubImageCols, e.SubImageRows);
    
    const float4 tex = SampleBaseList(e.TextureIndex, input.uv, input.quadUV, input.blend, cellsize, float2(0.f, 0.f));
    
   
    MaterialInput mi;
    mi.quadUV = input.quadUV;
    mi.baseColor = ResolveSpriteColor(input.color, tex, e.TextureAlpha, e.BlendMode);
    mi.coverage = BaseCoverage(input.color, tex, e.TextureAlpha);
    mi.particleParams = input.params;
    mi.subUV = input.uv;
    mi.blend = input.blend;
    mi.vertexColor = input.color;
    mi.paramBase = e.ParamBase;
    mi.instrBase = e.InstrBase;
    mi.baseTexture = e.TextureIndex;
    mi.effectTextures = e.EffectTextures;
    mi.cellSize = cellsize;
    mi.textureAlpha = e.TextureAlpha;
    mi.blendMode = e.BlendMode;
    
    const float4 result = RunMaterial(mi, e.InstrCount, e.OutputReg);
    const float visible = (e.BlendMode == 1) ? max(result.r, max(result.g, result.b)) : result.a;
    if (visible < 0.01f)
        discard;
    return result;
    
}
