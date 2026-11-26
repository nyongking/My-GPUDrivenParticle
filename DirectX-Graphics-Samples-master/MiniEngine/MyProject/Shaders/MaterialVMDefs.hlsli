#ifndef MATERIAL_VM_DEFS_HLSLI
#define MATERIAL_VM_DEFS_HLSLI

#define MAX_REGS    16 // 중간에 저장되는 레지스터의 수
#define MAX_INSTRS  256 // 최대 명령어 수

#define OP_QUAD_UV          0   // 사각형 UV
#define OP_BASE_COLOR       1   // Base 색상
#define OP_COVERAGE         2   // Base 불투명도
#define OP_PARTICLE         3   // 파티클별 값
#define OP_CONST            4   // PARAM(P)
#define OP_UV_TRANSFORM     5   // 타일링 + 스크롤
#define OP_SAMPLE           6   // 효과용 텍스처를 샘플링
#define OP_ADD              7   // 
#define OP_SUB              8
#define OP_MUL              9
#define OP_LERP             10
#define OP_SATURATE         11
#define OP_ONE_MINUS        12
#define OP_SMOOTHSTEP       13
#define OP_APPEND           14  // 벡터 합치기 (RGB, A)
#define OP_UNPACK_SIGNED    15  // 0~1 -> -1~1
#define OP_BASE_SAMPLE      16  // 베이스를 a.xy만큼 밀어 다시 읽은 색
#define OP_POWER            17


// val에서 s에 설정된 비트에 따라서 return의 value가 채워진다
// s -> x = 00, y = 01, z = 10, w = 11
// 2비트씩 비트연산, 해당되는 원소를 쓰게된다.
float4 Swizzle(float4 val, uint swz)
{
    return float4(val[swz & 3], val[(swz >> 2) & 3], val[(swz >> 4) & 3], val[(swz >> 6) & 3]);
}


#endif