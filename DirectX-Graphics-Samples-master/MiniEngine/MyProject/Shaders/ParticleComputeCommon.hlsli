#ifndef PARTICLE_COMPUTE_COMMON_HLSLI
#define PARTICLE_COMPUTE_COMMON_HLSLI

#include "ParticleCommon.hlsli"

cbuffer SimConstants : register(b0)
{
    float3 EmitterPos;
    float DeltaTime;
    float3 Gravity;
    uint EmitCount;
    float MinLife;
    float MaxLife;
    float MinSpeed;
    float MaxSpeed;
    float ParticleSize;
    uint MaxParticles;
    uint RandomSeed;
    uint BaseColor;
}

RWStructuredBuffer<Particle>    Particles : register(u0);
RWStructuredBuffer<uint>        DeadList : register(u1);
RWByteAddressBuffer             Counters : register(u2);
RWStructuredBuffer<uint>        AliveIn : register(u3);
RWStructuredBuffer<uint>        AliveOut : register(u4);
RWByteAddressBuffer             IndirectArgs : register(u5); // 0~12 : simulate dispatch args, 16~32 : draw args

#endif
