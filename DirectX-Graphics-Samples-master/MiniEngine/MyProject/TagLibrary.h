#pragma once
namespace Engine
{
	constexpr const char* DeltaTime = "Engine.DeltaTime";
	constexpr const char* OwnerPosition = "Engine.Owner.Position";
	constexpr const char* OwnerVelocity = "Engine.Owner.Velocity";
	constexpr const char* OwnerLocalToWorld = "Engine.Owner.LocalToWorld";
	constexpr const char* NumParticles = "Engine.Emitter.NumParticles";
	constexpr const char* EmitterAge = "Engine.Emitter.Age";
};

namespace Emitter
{
	constexpr const char* SpawnCount =		"Emitter.SpawnCount";
	constexpr const char* SpawnRemainder =	"Emitter.SpawnRemainder";
}

namespace Particles
{

	constexpr const char* Position =		"Particles.Position";
	constexpr const char* Lifetime =		"Particles.Lifetime";
	constexpr const char* Age =				"Particles.Age";
	constexpr const char* NormalizedAge =	"Particles.NormalizedAge";
	constexpr const char* Color =			"Particles.Color";
	constexpr const char* Size =			"Particles.Size";
	constexpr const char* Velocity =		"Particles.Velocity";
	constexpr const char* Mass =			"Particles.Mass";
	constexpr const char* Acceleration	=	"Particles.Acceleration";
	constexpr const char* SubImageIndex =	"Particles.SubImageIndex";
	constexpr const char* SpriteSize =		"Particles.SpriteSize";		// float2 (full width / height)
	constexpr const char* SpriteRotation =	"Particles.SpriteRotation";	// float (degrees)
	constexpr const char* SpriteFacing =	"Particles.SpriteFacing";	// float3
	constexpr const char* SpriteAlignment =	"Particles.SpriteAlignment";// float3

	// Spawn 이 끝났을 때의 값 (엔진이 복사). Scale Color 처럼 "처음 값 x 배율" 을 계산할 때 쓴다
	constexpr const char* InitialColor =	"Particles.Initial.Color";	// float4
}
