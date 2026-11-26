#include "SpriteMaterialCommon.hlsli"
#include "MaterialVMDefs.hlsli"

StructuredBuffer<uint4> InstrWords : register(t7); // 4,5,6,7 = texture slot

cbuffer MaterialProgramCB : register(b3)
{
    uint UseVM; // 0 false, 1 true -> Op 실행
    uint InstrCount; 
    uint OutputReg; // 결과가 든 register
    uint _ProgramPad;
}

void LoadInstr(MaterialInput i, uint k, out uint4 w0, out uint4 w1)
{
    w0 = InstrWords[k * 2];
    w1 = InstrWords[k * 2 + 1];
}

#include "MaterialVM.hlsli"

float4 main(PSInput input) : SV_TARGET
{
    if (UseVM != 0)
    {
        const float4 vmTex = SampleBase(input, float2(0.f, 0.f));           // 베이스 텍스처 읽기

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
