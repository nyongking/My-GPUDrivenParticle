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

OwnerTransform ComposeOwnerTransform(const OwnerTransform& parent, const OwnerTransform& child)
{
	const float4& a = parent.rotation;
	const float4& b = child.rotation;
	OwnerTransform out;
	out.rotation = {
		a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
		a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
		a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
		a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z };
	out.scale = { parent.scale.x * child.scale.x, parent.scale.y * child.scale.y, parent.scale.z * child.scale.z };
	out.position = TransformPoint(MakeLocalToWorld(parent), child.position);
	return out;
}

float4 QuatFromEulerDegrees(const float3& degrees)
{
	const float k = 3.14159265f / 180.f;
	OwnerTransform yaw, pitch, roll;
	yaw.rotation = QuatFromAxisAngle(float3{ 0.f, 1.f, 0.f }, degrees.y * k);
	pitch.rotation = QuatFromAxisAngle(float3{ 1.f, 0.f, 0.f }, degrees.x * k);
	roll.rotation = QuatFromAxisAngle(float3{ 0.f, 0.f, 1.f }, degrees.z * k);
	return ComposeOwnerTransform(yaw, ComposeOwnerTransform(pitch, roll)).rotation;
}

float3 EulerDegreesFromQuat(const float4& q)
{
	const float k = 180.f / 3.14159265f;
	float sinPitch = 2.f * (q.w * q.x - q.y * q.z);
	sinPitch = sinPitch > 1.f ? 1.f : (sinPitch < -1.f ? -1.f : sinPitch);
	const float pitch = std::asin(sinPitch);
	const float yaw = std::atan2(2.f * (q.w * q.y + q.x * q.z), 1.f - 2.f * (q.x * q.x + q.y * q.y));
	const float roll = std::atan2(2.f * (q.w * q.z + q.x * q.y), 1.f - 2.f * (q.x * q.x + q.z * q.z));
	return { pitch * k, yaw * k, roll * k };
}
