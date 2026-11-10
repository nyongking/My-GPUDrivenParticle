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

ModuleDesc InitializeParticle(Input<float> size, Input<float> mass, Input<float> lifeTime, Input<float4> color)
{
	ModuleDesc m;

	m.name = "Initialize Particle";
	m.stage = Stage::ParticleSpawn;
	m.params =
	{
		MakeParam<float>("Size", size).WithMin(0.f),
		MakeParam<float>("Mass", mass).WithMin(0.001f),
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
		{Particles::Age, &Types::Float},
		{Particles::Mass, &Types::Float}
	};
	m.hlsl = R"(
p.Position = OwnerPosition;
p.Mass = PARAM(Mass);
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
			const auto massParam = ctx.Param<float>("Mass");

			const bool hasPos = ctx.HasParticle(Particles::Position);
			const bool hasCol = ctx.HasParticle(Particles::Color);
			const bool hasSize = ctx.HasParticle(Particles::Size);
			const bool hasMass = ctx.HasParticle(Particles::Mass);
			const bool hasLife = ctx.HasParticle(Particles::Lifetime);
			const bool hasAge = ctx.HasParticle(Particles::Age);

			StridedView<float3> pos = {};
			StridedView<float4> col = {};
			StridedView<float> sizes = {};
			StridedView<float> masses = {};
			StridedView<float> life = {};
			StridedView<float> age = {};
			if (hasPos)  pos = ctx.Particles<float3>(Particles::Position);
			if (hasCol)  col = ctx.Particles<float4>(Particles::Color);
			if (hasSize) sizes = ctx.Particles<float>(Particles::Size);
			if (hasMass) masses = ctx.Particles<float>(Particles::Mass);
			if (hasLife) life = ctx.Particles<float>(Particles::Lifetime);
			if (hasAge)  age = ctx.Particles<float>(Particles::Age);

			for (uint32_t i = ctx.Begin(); i < ctx.End(); ++i)
			{
				if (hasPos)  pos[i] = origin;
				if (hasCol)  col[i] = colorParam(i);
				if (hasSize) sizes[i] = sizeParam(i);
				if (hasMass) masses[i] = massParam(i);
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

ModuleDesc SolveForceAndVelocity(Input<float> speedLimit, Input<float> accelLimit)
{
	ModuleDesc m;
	m.name = "Solve Force and Velocity";
	m.stage = Stage::ParticleUpdate;
	m.params =
	{
		MakeParam<float>("AccelLimit", accelLimit).WithMin(0.f),
		MakeParam<float>("SpeedLimit", speedLimit).WithMin(0.f)
	};
	m.reads =
	{
		{Engine::DeltaTime, &Types::Float},
		{Particles::Position, &Types::Float3},
		{Particles::Velocity, &Types::Float3},
		{Particles::Acceleration, &Types::Float3},
	};

	m.writes =
	{
		{Particles::Position, &Types::Float3},
		{Particles::Velocity, &Types::Float3},
		{Particles::Acceleration, &Types::Float3}
	};
	m.hlsl = R"(
float3 accel = p.Acceleration;
const float aLen = length(accel);
if (PARAM(AccelLimit) > 0.f && aLen > PARAM(AccelLimit))
	accel *= PARAM(AccelLimit) / aLen;

p.Velocity += accel * DeltaTime;

const float sLen = length(p.Velocity);
if (PARAM(SpeedLimit) > 0.f && sLen > PARAM(SpeedLimit))
	p.Velocity *= PARAM(SpeedLimit) / sLen;

p.Position += p.Velocity * DeltaTime;
p.Acceleration = float3(0.f, 0.f, 0.f);
)";

	m.execute = [](ModuleContext& ctx)
		{
			const float dt = ctx.DeltaTime();
			const float aMax = ctx.Param<float>("AccelLimit")(0);
			const float sMax = ctx.Param<float>("SpeedLimit")(0);

			auto pos = ctx.Particles<float3>(Particles::Position);
			auto vel = ctx.Particles<float3>(Particles::Velocity);
			auto acc = ctx.Particles<float3>(Particles::Acceleration);


			for (uint32_t i = ctx.Begin(); i < ctx.End(); ++i)
			{
				float3 a = acc[i];
				const float aLen = std::sqrt(a.x * a.x + a.y * a.y + a.z * a.z);
				if (aMax > 0.f && aLen > aMax)
				{
					const float ratio = aMax / aLen;
					a.x *= ratio;
					a.y *= ratio;
					a.z *= ratio;
				}

				float3& v = vel[i];
				v.x += a.x * dt;
				v.y += a.y * dt;
				v.z += a.z * dt;

				const float sLen = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
				if (sMax > 0.f && sLen > sMax)
				{
					const float ratio = sMax / sLen;
					v.x *= ratio;
					v.y *= ratio;
					v.z *= ratio;
				}

				pos[i].x += v.x * dt;
				pos[i].y += v.y * dt;
				pos[i].z += v.z * dt;

				acc[i] = { 0.f, 0.f, 0.f };
			}
		};



	return m;
}

ModuleDesc GravityForce(float3 gravity)
{
	ModuleDesc m;

	m.name = "Gravity Force";
	m.stage = Stage::ParticleUpdate;
	m.params = { MakeParam<float3>("Gravity", gravity) };
	m.reads =
	{
		{Particles::Acceleration, &Types::Float3},
		{Particles::Mass, &Types::Float}
	};
	m.writes = { {Particles::Acceleration, &Types::Float3} };

	m.hlsl = R"(
p.Acceleration += PARAM(Gravity) / p.Mass;
)";

	m.execute = [](ModuleContext& ctx)
		{
			const float3 g = ctx.Param<float3>("Gravity")(0);
			auto acc = ctx.Particles<float3>(Particles::Acceleration);
			auto mass = ctx.Particles<float>(Particles::Mass);
			for (uint32_t i = ctx.Begin(); i < ctx.End(); ++i)
			{
				acc[i].x += g.x / mass[i];
				acc[i].y += g.y / mass[i];
				acc[i].z += g.z / mass[i];

			}

		};

	return m;
}

ModuleDesc SubUVAnimation(float startFrame, float endFrame, bool loop, float framePerSecond)
{
	ModuleDesc m;

	m.name = "SubUV Animation";
	m.stage = Stage::ParticleUpdate;
	m.params =
	{
		MakeParam<float>("StartFrame", startFrame).WithMin(0.f),
		MakeParam<float>("EndFrame", endFrame).WithMin(0.f),
		MakeParam<NBool>("Loop", NBool(loop)),
		MakeParam<float>("FramesPerSecond", framePerSecond).WithMin(0.f)
	};
	m.reads =
	{
		{Particles::Age, &Types::Float},
		{Particles::NormalizedAge, &Types::Float}
	};
	m.writes =
	{
		{Particles::SubImageIndex, &Types::Float}
	};
	m.hlsl = R"(
const float count = max(PARAM(EndFrame) - PARAM(StartFrame) + 1.f, 1.f);
float t;
if (PARAM(Loop) != 0)
	t = frac(p.Age * PARAM(FramesPerSecond) / count) * count;
else
	t = min(p.NormalizedAge * count, count - 0.001f);
p.SubImageIndex = PARAM(StartFrame) + t;
)";	

	m.execute = [](ModuleContext& ctx)
		{
			const float start = ctx.Param<float>("StartFrame")(0);
			const float end = ctx.Param<float>("EndFrame")(0);
			const bool loop = static_cast<bool>(ctx.Param<NBool>("Loop")(0));
			const float fps = ctx.Param<float>("FramesPerSecond")(0);
			const float count = std::fmax(end - start + 1.f, 1.f);

			auto age = ctx.Particles<float>(Particles::Age);
			auto nage = ctx.Particles<float>(Particles::NormalizedAge);
			auto index = ctx.Particles<float>(Particles::SubImageIndex);

			for (uint32_t i = ctx.Begin(); i < ctx.End(); ++i)
			{
				float t;
				if (loop)
				{
					const float cycles = age[i] * fps / count;
					t = (cycles - std::floor(cycles)) * count;
				}
				else
				{
					t = std::fmin(nage[i] * count, count - 0.001f);
				}
				index[i] = start + t;
			}
		};

	return m;
}


