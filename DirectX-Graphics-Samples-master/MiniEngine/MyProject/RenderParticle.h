#pragma once
// 렌더링에 꼭 필요한 파티클 값을 고정된 구조체로 둔다 (StructuredBuffer<RenderParticle>, 64 B)
//
//   - 모듈(시뮬레이션) 은 이 구조체를 읽고 쓴다 (RW).  VS / 정렬 / Gather 는 읽기만 한다
//   - 여기 있는 속성은 이미터가 쓰든 안 쓰든 항상 존재한다 (Trim 대상이 아님, 렌더러 바인딩도 고정)
//       아무 모듈도 쓰지 않는 필드는 CPU 가 정한 기본값이 Spawn 때 들어간다
//   - 나머지 속성 (Lifetime, Age, Mass, Initial / Previous 사본, 모듈 전용 값 ...) 은
//       지금처럼 가변 레이아웃 SoA 버퍼에 둔다 (CompiledEmitter::particles)
//
//   HLSL Shaders/RenderParticle.hlsli 의 struct RenderParticle 과 필드 순서 / 크기가 같아야 한다.
//   필드를 바꾸면 같이 고칠 곳: RenderParticle.hlsli, RenderParticle.cpp (Layout), ParticleUber.hlsli (Load / Store)

#include "AttributeType.h"
#include "LayoutBuilder.h"

#include <cstdint>
#include <string>

struct RenderParticleData
{
	float3 position;        // 0   Particles.Position
	float  subImageIndex;   // 12  Particles.SubImageIndex
	float4 color;           // 16  Particles.Color
	float2 spriteSize;      // 32  Particles.SpriteSize     x < 0 이면 (Size * 2, Size * 2) 로 그린다
	float  spriteRotation;  // 40  Particles.SpriteRotation (도)
	float  normalizedAge;   // 44  Particles.NormalizedAge  머티리얼의 ParticleParam.x 로 전달
	float3 velocity;        // 48  Particles.Velocity       Velocity Aligned 용
	float  size;            // 60  Particles.Size
};
static_assert(sizeof(RenderParticleData) == 64, "HLSL struct RenderParticle (64 B) 와 같아야 함");

namespace RenderParticle
{
	constexpr uint32_t kBytes = sizeof(RenderParticleData);

	// 고정 필드의 배치표. Slot::offset = 구조체 안의 바이트 위치 (SoA 의 열 offset 과는 다른 뜻)
	const Block& Layout();

	// 전체 이름 ("Particles.Position") 으로 찾기. 고정 필드가 아니면 nullptr
	const Slot* Find(const std::string& fullName);

	// 아무 모듈도 쓰지 않을 때의 값. SpriteSize 는 "없음" 표시 (-1, -1)
	RenderParticleData Defaults();
}
