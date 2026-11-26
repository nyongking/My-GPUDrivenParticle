#include "pch.h"
#include "RenderParticle.h"

#include "TagLibrary.h"

#include <cstddef>

namespace
{
	Block BuildLayout()
	{
		// 구조체의 필드 순서 그대로 (Tight 배치 = HLSL StructuredBuffer 의 배치)
		Block b;
		b.Add(AttributeKey::Parse(Particles::Position),       &Types::Float3);
		b.Add(AttributeKey::Parse(Particles::SubImageIndex),  &Types::Float);
		b.Add(AttributeKey::Parse(Particles::Color),          &Types::Float4);
		b.Add(AttributeKey::Parse(Particles::SpriteSize),     &Types::Float2);
		b.Add(AttributeKey::Parse(Particles::SpriteRotation), &Types::Float);
		b.Add(AttributeKey::Parse(Particles::NormalizedAge),  &Types::Float);
		b.Add(AttributeKey::Parse(Particles::Velocity),       &Types::Float3);
		b.Add(AttributeKey::Parse(Particles::Size),           &Types::Float);

		// 배치표와 C++ 구조체가 어긋나면 CPU 이미터가 엉뚱한 자리를 쓴다
		ASSERT(b.Stride() == RenderParticle::kBytes);
		ASSERT(b.Find(Particles::Position)->offset       == offsetof(RenderParticleData, position));
		ASSERT(b.Find(Particles::SubImageIndex)->offset  == offsetof(RenderParticleData, subImageIndex));
		ASSERT(b.Find(Particles::Color)->offset          == offsetof(RenderParticleData, color));
		ASSERT(b.Find(Particles::SpriteSize)->offset     == offsetof(RenderParticleData, spriteSize));
		ASSERT(b.Find(Particles::SpriteRotation)->offset == offsetof(RenderParticleData, spriteRotation));
		ASSERT(b.Find(Particles::NormalizedAge)->offset  == offsetof(RenderParticleData, normalizedAge));
		ASSERT(b.Find(Particles::Velocity)->offset       == offsetof(RenderParticleData, velocity));
		ASSERT(b.Find(Particles::Size)->offset           == offsetof(RenderParticleData, size));
		return b;
	}
}

const Block& RenderParticle::Layout()
{
	static const Block s_Layout = BuildLayout();
	return s_Layout;
}

const Slot* RenderParticle::Find(const std::string& fullName)
{
	return Layout().Find(fullName);
}

RenderParticleData RenderParticle::Defaults()
{
	RenderParticleData d = {};
	d.position = { 0.f, 0.f, 0.f };
	d.subImageIndex = 0.f;
	d.color = { 1.f, 1.f, 1.f, 1.f };
	d.spriteSize = { -1.f, -1.f };      // SpriteSize 를 쓰는 모듈이 없으면 VS 가 Size * 2 로 그린다
	d.spriteRotation = 0.f;
	d.normalizedAge = 0.f;
	d.velocity = { 0.f, 0.f, 0.f };
	d.size = 0.1f;
	return d;
}
