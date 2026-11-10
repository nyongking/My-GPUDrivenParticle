#include "pch.h"
#include "OwnerTransform.h"

#include <cmath>

float4 QuatFromAxisAngle(const float3& axis, float radians)
{
	const float s = std::sin(radians * 0.5f);
	return { axis.x * s, axis.y * s, axis.z * s, std::cos(radians * 0.5f) };
}

float4x4 MakeLocalToWorld(const OwnerTransform& t)
{
	const float x = t.rotation.x, y = t.rotation.y, z = t.rotation.z, w = t.rotation.w;

	const float r[3][3] =
	{
		{ 1.f - 2.f * (y * y + z * z), 2.f * (x * y + z * w),       2.f * (x * z - y * w) },
		{ 2.f * (x * y - z * w),       1.f - 2.f * (x * x + z * z), 2.f * (y * z + x * w) },
		{ 2.f * (x * z + y * w),       2.f * (y * z - x * w),       1.f - 2.f * (x * x + y * y) },
	};
	const float s[3] = { t.scale.x, t.scale.y, t.scale.z };

	float4x4 m = Identity4x4();
	for (int row = 0; row < 3; ++row)
		for (int col = 0; col < 3; ++col)
			m.m[row][col] = r[row][col] * s[row];
	
	m.m[3][0] = t.position.x;
	m.m[3][1] = t.position.y;
	m.m[3][2] = t.position.z;
	return m;
}

float3 ComputeOwnerVelocity(const float3& now, const float3* prev, float deltaTime)
{
	if (!prev || deltaTime <= 0.f)
		return { 0.f, 0.f, 0.f };

	return { (now.x - prev->x) / deltaTime, (now.y - prev->y) / deltaTime, (now.z - prev->z) / deltaTime };
}
