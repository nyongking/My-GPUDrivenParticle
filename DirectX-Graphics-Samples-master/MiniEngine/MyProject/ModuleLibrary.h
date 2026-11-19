#pragma once
#include "ModuleDesc.h"

// Module 자료에 따른 동작 방식을 만드는 곳

// Emitter Update
ModuleDesc SpawnRate(float rate); // Rate에 따라 Spawn 개수를 계산하는 Module

// Particle Spawn
ModuleDesc InitializeParticle(Input<float> size, Input<float> mass, Input<float> lifeTime, Input<float3> pos, Input<float4> color); // Particle의 입자
ModuleDesc SphereLocation(Input<float> radius, bool surfaceOnly = false);
ModuleDesc InitializeSprite(Input<float2> spriteSize, Input<float> spriteRotation);   // SpriteSize (가로/세로 전체 길이), SpriteRotation (도)
ModuleDesc AddVelocityInCone(Input<float> speed, float coneAngleDeg, float3 coneAxis, float InnerConeAngle = 0.f, float speedfalloff = 0.f, float distribution = 1.f, bool localSpace = false);

// Particle Update
ModuleDesc ParticleState(bool killOnLifeTime = true);
ModuleDesc SolveForceAndVelocity(Input<float> speedLimit, Input<float> accelLimit);
ModuleDesc GravityForce(float3 gravity);
ModuleDesc SubUVAnimation(float startFrame, float endFrame, bool loop = false, float framePerSecond = 30.f);
ModuleDesc SpriteRotationRate(float degreesPerSecond);  // SpriteRotation += rate * dt
ModuleDesc ScaleColor(Input<float3> scaleRGB, Input<float> scaleAlpha);   // Color = Initial.Color * (ScaleRGB, ScaleAlpha). 수명에 따른 변화는 Curve<T>(...) 로
ModuleDesc CurlNoiseForce(Input<float> strength, Input<float> frequency, Input<float3> offset, Input<float3> pan);   // Acceleration += CurlNoise(Position * Frequency + Offset + Pan * Emitter.Age) * Strength / Mass

// ---- 데이터(에셋) 용 ----
// 저장 파일에 쓰는 모듈 이름. 바꾸면 기존 파일을 못 읽으므로 이름은 고정 (ModuleId 순서와는 무관)
const char* ModuleIdName(ModuleId id);
ModuleId    ModuleIdFromName(const std::string& name);   // 모르면 ModuleId::Unknown
// id 로 기본값 모듈을 만든다 (UI 의 Add module 목록과 같은 기본값). 모르는 id 면 id = Unknown
ModuleDesc  MakeDefaultModule(ModuleId id);
