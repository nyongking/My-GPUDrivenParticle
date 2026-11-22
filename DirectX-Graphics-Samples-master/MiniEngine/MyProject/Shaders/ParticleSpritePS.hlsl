#include "SpriteAlpha.hlsli"
#include "MaterialVMDefs.hlsli"

#define MAX_MATERIAL_TEXTURES 4
#define MAX_MATERIAL_PARAMS 16

#define EFFECT_NONE         0
#define EFFECT_PATTERN      1
#define EFFECT_TINT         2
#define EFFECT_DISSOLVE     3
#define EFFECT_DISTORTION   4
#define EFFECT_GLOW         5

Texture2D<float4> SpriteTexture : register(t2); // t0, t1 은 VS (파티클 / 정렬 인덱스)
Texture2D<float4> MaterialTextures[MAX_MATERIAL_TEXTURES] : register(t3);

SamplerState LinearClamp : register(s0);
SamplerState LinearWrap : register(s1);

cbuffer SpritePSConstants : register(b1)
{
    uint BlendMode; // 0 = Translucent, 1 = Additive
    uint TextureAlpha; // 0 = Straight, 1 = Premultiplied, 2 = FromLuminance
    float Time; // Global Time
    uint EffectMode; // Temp
    float2 CellSize; // SubUV 한칸 크기
};

cbuffer MaterialParamsCB : register(b2)
{
    float4 MaterialParams[MAX_MATERIAL_PARAMS];
};

cbuffer MaterialProgramCB : register(b3)
{
    uint UseVM; // 0 false, 1 true -> Op 실행
    uint InstrCount; 
    uint OutputReg; // 결과가 든 register
    uint _ProgramPad;
    uint4 InstrWords[MAX_INSTRS * 2]; // 명령이 들어있는 내용
}

struct PSInput
{
    float4 position : SV_Position;
    float4 color : COLOR0;
    float4 uv : TEXCOORD0;
    float2 quadUV : TEXCOORD1;
    nointerpolation float blend : TEXCOORD2;
    nointerpolation float2 params : TEXCOORD3;
};

float4 Param(uint k)
{
    return MaterialParams[k];
}

float4 SampleTex(uint slot, float2 uv)
{
    return MaterialTextures[slot].Sample(LinearWrap, uv);
}

struct MaterialInput
{
    float2 quadUV;
    float4 baseColor;
    float  coverage;
    float2 particleParams;
    float4 subUV;
    float  blend;
    float4 vertexColor;
};

float4 Param(MaterialInput i, uint k)
{
    return MaterialParams[k];
}

float4 SampleTex(MaterialInput i, uint slot, float2 uv)
{
    return MaterialTextures[slot].Sample(LinearWrap, uv);
}

float4 SampleBaseUV(float4 subUV, float2 quadUV, float blend, float2 quadOffset)
{
    const float2 offset = quadOffset * CellSize;
    const float2 origin0 = subUV.xy - quadUV * CellSize;
    const float2 origin1 = subUV.zw - quadUV * CellSize;
    const float2 uv0 = clamp(subUV.xy + offset, origin0, origin0 + CellSize);
    const float2 uv1 = clamp(subUV.zw + offset, origin1, origin1 + CellSize);
    const float4 tex0 = SpriteTexture.Sample(LinearClamp, uv0);
    const float4 tex1 = SpriteTexture.Sample(LinearClamp, uv1);
    return lerp(tex0, tex1, blend);
}

float4 SampleBaseOffset(MaterialInput i, float2 quadOffset)
{
    const float4 tex = SampleBaseUV(i.subUV, i.quadUV, i.blend, quadOffset);
    return ResolveSpriteColor(i.vertexColor, tex, TextureAlpha, BlendMode);
}

void LoadInstr(MaterialInput i, uint k, out uint4 w0, out uint4 w1)
{
    w0 = InstrWords[k * 2];
    w1 = InstrWords[k * 2 + 1];
}

float2 UVTransform(float2 uv, uint k)
{
    return uv * Param(k).xy + Param(k).zw + Param(k + 1).xy * Time; // 타일링 + 오프셋
}

float4 SampleBase(PSInput input, float2 quadOffset)
{
    return SampleBaseUV(input.uv, input.quadUV, input.blend, quadOffset);
}

float BaseCoverage(float4 color, float4 tex)
{
    const float texAlpha = (TextureAlpha == ALPHA_FROM_LUMINANCE) ? max(tex.r, max(tex.g, tex.b)) : tex.a;
    return saturate(color.a * texAlpha);
}


#include "MaterialVM.hlsli"

float4 main(PSInput input) : SV_TARGET
{
    if (UseVM != 0)
    {
        const float4 vmTex = SampleBase(input, float2(0.f, 0.f));

        MaterialInput mi;
        mi.quadUV = input.quadUV;
        mi.baseColor = ResolveSpriteColor(input.color, vmTex, TextureAlpha, BlendMode);
        mi.coverage = BaseCoverage(input.color, vmTex);
        mi.particleParams = input.params;
        mi.subUV = input.uv;
        mi.blend = input.blend;
        mi.vertexColor = input.color;

        const float4 vmResult = RunMaterial(mi, InstrCount, OutputReg);

        const float vmVisible = (BlendMode == 1) ? max(vmResult.r, max(vmResult.g, vmResult.b)) : vmResult.a;
        if (vmVisible < 0.01f)
            discard;
        return vmResult;
    }

    float2 distortion = float2(0.f, 0.f);
    if (EffectMode == EFFECT_DISTORTION)
    {
        const float2 n0 = input.quadUV;
        const float2 n1 = UVTransform(n0, 0);
        const float4 n2 = SampleTex(0, n1);
        distortion = (n2.rg * 2.f - 1.f) * Param(2).xy;
    }
    
    // distortion 적용
    const float4 tex = SampleBase(input, distortion);
    const float4 baseColor = ResolveSpriteColor(input.color, tex, TextureAlpha, BlendMode);
    const float coverage = BaseCoverage(input.color, tex);
    
    float4 result = baseColor;
    
    if (EffectMode == EFFECT_PATTERN)
    {
        const float2 n0 = input.quadUV;
        const float2 n1 = UVTransform(n0, 0);
        const float4 n2 = SampleTex(0, n1);
        result = float4(baseColor.rgb * n2.r, baseColor.a);
    }
    else if (EffectMode == EFFECT_TINT)
    {
        const float2 n0 = input.quadUV;
        const float2 n1 = UVTransform(n0, 0);
        const float4 n2 = SampleTex(0, n1);
        result = float4(baseColor.rgb * n2.rgb, baseColor.a); 
    }
    else if (EffectMode == EFFECT_DISSOLVE)
    {
        const float2 n0 = input.quadUV;
        const float2 n1 = UVTransform(n0, 0);
        const float4 n2 = SampleTex(0, n1);     // mask
        const float n3 = input.params.x;            
        const float n4 = n3 + Param(2).x;
        const float n5 = smoothstep(n3, n4, n2.r);
        const float4 n6 = baseColor * n5;
        const float n7 = n4 + Param(2).y;       // 
        const float n8 = 1.f - smoothstep(n4, n7, n2.r);
        const float3 n9 = n6.rgb + Param(3).rgb * (n8 * n5 * coverage);
        result = float4(n9, n6.a);
    }
    else if (EffectMode == EFFECT_DISTORTION)
    {
        const float2 n3 = input.quadUV + distortion;
        const float4 n4 = SampleTex(1, n3);
        result = float4(baseColor.rgb * n4.r, baseColor.a);
    }
    else if (EffectMode == EFFECT_GLOW)
    {
        const float2 n0 = input.quadUV;
        const float2 n1 = UVTransform(n0, 0);
        const float4 n2 = SampleTex(0, n1);
        const float2 m1 = UVTransform(n0, 2);
        const float4 m2 = SampleTex(1, m1);
        const float n3 = n2.r * m2.r;
        const float3 n4 = baseColor.rgb + Param(4).rgb * (n3 * coverage);
        result = float4(n4, baseColor.a);
    }
         
    const float visible = (BlendMode == 1) ? max(result.r, max(result.g, result.b)) : result.a;
    if (visible < 0.01f)
        discard;

    return result;
}
