#pragma once
namespace Engine
{
	constexpr const char* DeltaTime = "Engine.DeltaTime";
	constexpr const char* OwnerPosition = "Engine.Owner.Position";
	constexpr const char* OwnerVelocity = "Engine.Owner.Velocity";
	constexpr const char* OwnerLocalToWorld = "Engine.Owner.LocalToWorld";
	constexpr const char* NumParticles = "Engine.Emitter.NumParticles";
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
}
