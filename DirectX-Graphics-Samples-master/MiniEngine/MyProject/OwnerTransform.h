#pragma once
#include "AttributeType.h"

// 엔진 값의 후보로 설정될 수 있는 데이터,

struct OwnerTransform
{
	float3 scale = { 1.f, 1.f, 1.f };
	float4 rotation = { 0.f, 0.f, 0.f, 1.f };
	float3 position = { 0.f, 0.f, 0.f };
};

inline float4x4 Identity4x4()
{
	float4x4 m = {};
	m.m[0][0] = m.m[1][1] = m.m[2][2] = m.m[3][3] = 1.f;
	return m;
}

// 위치 변환
inline float3 TransformPoint(const float4x4& m, const float3& p)
{
	return {
		p.x * m.m[0][0] + p.y * m.m[1][0] + p.z * m.m[2][0] + m.m[3][0],
		p.x * m.m[0][1] + p.y * m.m[1][1] + p.z * m.m[2][1] + m.m[3][1],
		p.x * m.m[0][2] + p.y * m.m[1][2] + p.z * m.m[2][2] + m.m[3][2] };
}

// 축을 기준으로 각도만큼 회전했을 때 나타나는 quaternion
float4   QuatFromAxisAngle(const float3& axis, float radians);

// scale - rot - pos 순서의 행렬
float4x4 MakeLocalToWorld(const OwnerTransform& t);

// (지금 위치 - 지난 위치) / 실제 dt. 첫 프레임(prev == nullptr) 이나 dt <= 0 이면 0
float3   ComputeOwnerVelocity(const float3& now, const float3* prev, float deltaTime);