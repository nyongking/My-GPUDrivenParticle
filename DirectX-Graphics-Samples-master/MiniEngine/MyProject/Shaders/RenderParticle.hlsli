// 렌더링에 꼭 필요한 파티클 값 (64 B). C++ RenderParticle.h 의 RenderParticleData 와 같은 배치
//   시뮬레이션 (모듈) 은 RWStructuredBuffer 로 읽고 쓰고, VS / 정렬 / Gather 는 StructuredBuffer 로 읽기만 한다
//   필드 이름 = 속성 이름 (Particles.Position -> Position). 생성 방식 셰이더가 이름으로 접근한다
#ifndef RENDER_PARTICLE_HLSLI
#define RENDER_PARTICLE_HLSLI

struct RenderParticle
{
    float3 Position; // 0
    float SubImageIndex; // 12
    float4 Color; // 16
    float2 SpriteSize; // 32  x < 0 이면 (Size * 2, Size * 2)
    float SpriteRotation; // 40  도
    float NormalizedAge; // 44  머티리얼의 ParticleParam.x
    float3 Velocity; // 48
    float Size; // 60
};

// 그릴 때 쓰는 가로 / 세로 전체 길이
float2 RenderSpriteSize(RenderParticle r)
{
    return (r.SpriteSize.x < 0.f) ? float2(r.Size, r.Size) * 2.f : r.SpriteSize;
}

#endif
