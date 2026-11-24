#ifndef MATERIAL_VM_HLSLI
#define MATERIAL_VM_HLSLI

#include "MaterialVMDefs.hlsli"

//      w0                      w1
// [ op | dst | a | b ]    [ c | swz | param | _pad ]

// swz = uint (0~7 bit = a의 어느 성분 / 8~15 = b의 어느 성분 / 16~23 c의 어느 성분)


float4  RunMaterial(MaterialInput i, uint instrCount, uint outputReg)
{
    if (instrCount == 0)
        return i.baseColor;
    
    float4 reg[MAX_REGS];
    [unroll]
    for (uint r = 0; r < MAX_REGS; ++r)
        reg[r] = 0.f;
    
    const uint count = min(instrCount, (uint) MAX_INSTRS);
    [loop]
    for (uint k = 0; k < count; ++k)
    {
        uint4 w0, w1;
        LoadInstr(i, k, w0, w1);
        const uint op = w0.x;
        const uint dst = w0.y;
        const uint swz = w1.y;
        const uint par = w1.z;
        
        const float4 a = Swizzle(reg[w0.z], swz & 0xFF);
        const float4 b = Swizzle(reg[w0.w], (swz >> 8) & 0xFF);
        const float4 c = Swizzle(reg[w1.x], (swz >> 16) & 0xFF);
        
        float4 v = 0.f;
        switch (op)
        {
            case OP_QUAD_UV:
                v = float4(i.quadUV, 0.f, 0.f);
                break;
            case OP_BASE_COLOR:
                v = i.baseColor;
                break;
            case OP_COVERAGE:
                v = i.coverage;
                break;
            case OP_PARTICLE:
                v = float4(i.particleParams, 0.f, 0.f);
                break;
            case OP_CONST:
                v = Param(i, par);
                break;
            case OP_UV_TRANSFORM:
            {
                const float4 p0 = Param(i, par);                
                const float4 p1 = Param(i, par + 1);
                v = float4(a.xy * p0.xy + p0.zw + p1.xy * Time, 0.f, 0.f);
                break;
            }
            case OP_SAMPLE:
                v = SampleTex(i, par, a.xy);
                break;
            case OP_ADD:
                v = a + b;
                break;
            case OP_SUB:
                v = a - b;
                break;
            case OP_MUL:
                v = a * b;
                break;
            case OP_LERP:
                v = lerp(a, b, c);
                break;
            case OP_SATURATE:
                v = saturate(a);
                break;
            case OP_ONE_MINUS:
                v = 1.f - a;
                break;
            case OP_SMOOTHSTEP:
                v = smoothstep(a, b, c);
                break;
            case OP_APPEND:
                v = float4(a.rgb, b.r);
                break;
            case OP_UNPACK_SIGNED:
                v = a * 2.f - 1.f;
                break;
            case OP_BASE_SAMPLE:
                v = SampleBaseOffset(i, a.xy);
                break;
            default:
                break;
        }
        
        reg[min(dst, (uint) (MAX_REGS - 1))] = v;
    }
    return reg[min(outputReg, (uint) (MAX_REGS - 1))];

}



#endif 