#include "pch.h"
#include "ModuleLibrary.h"
#include "ModuleContext.h"

#include "AttributeType.h"

#include "TagLibrary.h"

ModuleDesc SpawnRate(float rate)
{
	ModuleDesc m;

	m.name = "Spawn Rate";
	m.stage = Stage::EmitterUpdate;
	m.params =
	{
		MakeParam<float>("Rate", rate).WithMin(0.f)
	};
	m.reads = 
	{ 
		{Engine::DeltaTime, &Types::Float},
		{Emitter::SpawnCount, &Types::UInt32 },
		{Emitter::SpawnRemainder, &Types::Float}	
	};
	m.writes =
	{
		{Emitter::SpawnCount, &Types::UInt32 },
		{Emitter::SpawnRemainder, &Types::Float}
	};
	m.hlsl = R"(
const float want = PARAM(Rate) * DeltaTime + e.SpawnRemainder;
const uint	n = (uint)want;
e.SpawnRemainder = want - (float)n;
e.SpawnCount += n;
)";
	m.execute = [](ModuleContext& ctx) 
		{
			float& remainder = ctx.Emitter<float>(Emitter::SpawnRemainder);
			uint32_t& count = ctx.Emitter<uint32_t>(Emitter::SpawnCount);

			const float want = ctx.Param<float>("Rate")(0) * ctx.DeltaTime() + remainder;
			const uint32_t n = static_cast<uint32_t>(want);

			remainder = want - static_cast<float>(n);
			count += n;
		};

	return m;
}

ModuleDesc InitializeParticle(Input<float> size, Input<float> lifeTime, Input<float4> color)
{
	ModuleDesc m;

	m.name = "Initialize Particle";
	m.stage = Stage::ParticleSpawn;
	m.params =
	{
		MakeParam<float>("Size", size).WithMin(0.f),
		MakeParam<float>("Lifetime", lifeTime).WithMin(0.f),
		MakeParam<float4>("Color", color).AsColor()
	};
	m.reads =
	{
		{Engine::OwnerPosition, &Types::Float3}
	};
	m.writes =
	{
		{Particles::Position, &Types::Float3},
		{Particles::Color, &Types::Float4},
		{Particles::Size, &Types::Float},
		{Particles::Lifetime, &Types::Float},
		{Particles::Age, &Types::Float}
	};
	m.hlsl = R"(
p.Position = OwnerPosition;
p.Color = PARAM(Color);
p.Size = PARAM(Size);
p.Lifetime = PARAM(Lifetime);
p.Age = 0.f;
)";

	m.execute = [](ModuleContext& ctx)
		{
			const float3 origin = ctx.OwnerPosition();

			const auto sizeParam = ctx.Param<float>("Size");
			const auto lifeParam = ctx.Param<float>("Lifetime");
			const auto colorParam = ctx.Param<float4>("Color");

			auto pos = ctx.Particles<float3>(Particles::Position);
			auto col = ctx.Particles<float4>(Particles::Color);
			auto sizes = ctx.Particles<float>(Particles::Size);
		
			const bool hasLife = ctx.HasParticle(Particles::Lifetime);
			const bool hasAge = ctx.HasParticle(Particles::Age);
			StridedView<float> life = {};
			StridedView<float> age = {};
			if (hasLife) life = ctx.Particles<float>(Particles::Lifetime);
			if (hasAge)  age = ctx.Particles<float>(Particles::Age);

			for (uint32_t i = ctx.Begin(); i < ctx.End(); ++i)
			{
				pos[i] = origin;
				col[i] = colorParam(i);
				sizes[i] = sizeParam(i);
				if (hasLife) life[i] = lifeParam(i);
				if (hasAge)  age[i] = 0.f;
			}
		};

	return m;

}

ModuleDesc SphereLocation(Input<float> radius, bool surfaceOnly)
{
	ModuleDesc m;
	m.name = "Sphere Location";
	m.stage = Stage::ParticleSpawn;
	m.params =
	{
		MakeParam<float>("Radius", radius).WithMin(0.f),
		MakeParam<NBool>("SurfaceOnly", NBool(surfaceOnly))
	};
	m.reads =
	{
		{ Particles::Position, &Types::Float3 }
	};
	m.writes = 
	{
		{Particles::Position, &Types::Float3}
	};
	m.hlsl = R"(
const float kTwoPi = 6.28318530f;
const float z = Random01(index, 0) * 2.f - 1.f;
const float phi = Random01(index, 1) * kTwoPi;
const float r = sqrt(max(0.f, 1.f - z * z));
const float dist = (PARAM(SurfaceOnly) != 0) ? PARAM(Radius) : 
		PARAM(Radius) * pow(Random01(index, 2), 1.f / 3.f);
p.Position += float3(r * cos(phi) * dist, r * sin(phi) * dist, z * dist);

)";

	m.execute = [](ModuleContext& ctx)
		{
			const bool surface = static_cast<bool>(ctx.Param<NBool>("SurfaceOnly")(0));
			const float kTwoPi = 6.28318530f;
			const auto radiusParam = ctx.Param<float>("Radius");

			auto pos = ctx.Particles<float3>(Particles::Position);
			
			for (uint32_t i = ctx.Begin(); i < ctx.End(); ++i)
			{
				const float z = ctx.Random01(i, 0) * 2.f - 1.f;
				const float phi = ctx.Random01(i, 1) * kTwoPi;
				const float r = std::sqrt(std::fmax(0.f, 1.f - z * z));

				const float radiusValue = radiusParam(i);
				const float dist = surface ? radiusValue
					: radiusValue * std::cbrt(ctx.Random01(i, 2));

				pos[i].x += r * std::cos(phi) * dist;
				pos[i].y += r * std::sin(phi) * dist;
				pos[i].z += z * dist;
			}
		};

	return m;
}

ModuleDesc ParticleState(bool killOnLifeTime)
{
	ModuleDesc m;
	m.name = "Particle State";
	m.stage = Stage::ParticleUpdate;
	m.params =
	{
		MakeParam<NBool>("KillOnLifetime", NBool(killOnLifeTime))
	};
	m.reads =
	{
		{Engine::DeltaTime, &Types::Float},
		{Particles::Age, &Types::Float},
		{Particles::Lifetime, &Types::Float}
	};
	m.writes =
	{
		{Particles::Age, &Types::Float},
		{Particles::NormalizedAge, &Types::Float}
	};
	m.hlsl = R"(
p.Age += DeltaTime;
const float t = (p.Lifetime > 0.f) ? p.Age / p.Lifetime : 1.f;
p.NormalizedAge = min(t, 1.f);
if (PARAM(KillOnLifetime) != 0 && t >= 1.f)
	KILL();
)";

	m.execute = [](ModuleContext& ctx)
		{
			const bool killAtEnd = static_cast<bool>(ctx.Param<NBool>("KillOnLifetime")(0));
			const float dt = ctx.DeltaTime();

			auto age = ctx.Particles<float>(Particles::Age);
			auto life = ctx.Particles<float>(Particles::Lifetime);

			const bool hasNormalized = ctx.HasParticle(Particles::NormalizedAge);
			StridedView<float> nage = {};
			if (hasNormalized)
				nage = ctx.Particles<float>(Particles::NormalizedAge);

			for (uint32_t i = ctx.Begin(); i < ctx.End(); ++i)
			{
				age[i] += dt;

				const float l = life[i];
				const float t = (l > 0.f) ? age[i] / l : 1.f;   // 수명 0 이면 바로 끝난 것으로

				if (hasNormalized)
					nage[i] = (t < 1.f) ? t : 1.f;

				if (killAtEnd && t >= 1.f)
					ctx.Kill(i);
			}
		};

	return m;
}


