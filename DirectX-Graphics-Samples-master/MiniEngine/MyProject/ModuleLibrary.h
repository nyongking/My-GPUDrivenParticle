#pragma once
#include "ModuleDesc.h"

// Module 자료에 따른 동작 방식을 만드는 곳

ModuleDesc SpawnRate(float rate); // Rate에 따라 Spawn 개수를 계산하는 Module
ModuleDesc InitializeParticle(Input<float> size, Input<float> lifeTime, Input<float4> color); // Particle의 입자
ModuleDesc SphereLocation(Input<float> radius, bool surfaceOnly = false);
ModuleDesc ParticleState(bool killOnLifeTime = true);