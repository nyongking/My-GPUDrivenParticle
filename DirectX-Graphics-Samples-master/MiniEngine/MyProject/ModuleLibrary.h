#pragma once
#include "ModuleDesc.h"

// Module 자료에 따른 동작 방식을 만드는 곳

// Emitter Update
ModuleDesc SpawnRate(float rate); // Rate에 따라 Spawn 개수를 계산하는 Module

// Particle Spawn
ModuleDesc InitializeParticle(Input<float> size, Input<float> mass, Input<float> lifeTime, Input<float4> color); // Particle의 입자
ModuleDesc SphereLocation(Input<float> radius, bool surfaceOnly = false);

// Particle Update
ModuleDesc ParticleState(bool killOnLifeTime = true);
ModuleDesc SolveForceAndVelocity(Input<float> speedLimit, Input<float> accelLimit);
ModuleDesc GravityForce(float3 gravity);
ModuleDesc SubUVAnimation(float startFrame, float endFrame, bool loop = false, float framePerSecond = 30.f);