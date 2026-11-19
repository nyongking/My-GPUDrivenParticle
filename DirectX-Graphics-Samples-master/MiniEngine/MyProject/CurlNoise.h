#pragma once
#include "AttributeType.h"

#include <cmath>
#include <cstdint>

// =====================================================================
// 컬 노이즈 (Curl Noise Force 의 CPU 경로)
//   ModuleCommon.hlsli 의 NoiseGradient / GradientNoiseDeriv / CurlNoise 와 같은 식. 같이 고친다
//   벡터 퍼텐셜 = salt 가 다른 3D 그래디언트 노이즈 3 개, 해석적 도함수로 curl 을 만든다 (발산 0)
// =====================================================================
namespace CurlNoiseDetail
{
	constexpr uint32_t kSaltX = 0x68E31DA4u;
	constexpr uint32_t kSaltY = 0xB5297A4Du;
	constexpr uint32_t kSaltZ = 0x1B56C4E9u;

	// ModuleCommon.hlsli 의 Hash 와 같다
	inline uint32_t Hash(uint32_t x)
	{
		x ^= x >> 16;
		x *= 0x7feb352dU;
		x ^= x >> 15;
		x *= 0x846ca68bU;
		x ^= x >> 16;
		return x;
	}

	struct V3 { float x, y, z; };
	inline V3    Add(V3 a, V3 b) { return { a.x + b.x, a.y + b.y, a.z + b.z }; }
	inline V3    Sub(V3 a, V3 b) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
	inline V3    Mul(V3 a, float s) { return { a.x * s, a.y * s, a.z * s }; }
	inline float Dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }

	inline V3 Gradient(int32_t x, int32_t y, int32_t z, uint32_t salt)
	{
		const uint32_t h = Hash(uint32_t(x) + Hash(uint32_t(y) + Hash(uint32_t(z) + salt)));
		constexpr float k = 2.f / 1023.f;
		return { float(h & 0x3FFu) * k - 1.f, float((h >> 10) & 0x3FFu) * k - 1.f, float((h >> 20) & 0x3FFu) * k - 1.f };
	}

	inline V3 GradientNoiseDeriv(V3 p, uint32_t salt)
	{
		const V3 f = { std::floor(p.x), std::floor(p.y), std::floor(p.z) };
		const int32_t ix = int32_t(f.x), iy = int32_t(f.y), iz = int32_t(f.z);
		const V3 w = Sub(p, f);
		const V3 u = { w.x * w.x * w.x * (w.x * (w.x * 6.f - 15.f) + 10.f),
		               w.y * w.y * w.y * (w.y * (w.y * 6.f - 15.f) + 10.f),
		               w.z * w.z * w.z * (w.z * (w.z * 6.f - 15.f) + 10.f) };
		const V3 du = { 30.f * w.x * w.x * (w.x * (w.x - 2.f) + 1.f),
		                30.f * w.y * w.y * (w.y * (w.y - 2.f) + 1.f),
		                30.f * w.z * w.z * (w.z * (w.z - 2.f) + 1.f) };

		const V3 ga = Gradient(ix, iy, iz, salt);
		const V3 gb = Gradient(ix + 1, iy, iz, salt);
		const V3 gc = Gradient(ix, iy + 1, iz, salt);
		const V3 gd = Gradient(ix + 1, iy + 1, iz, salt);
		const V3 ge = Gradient(ix, iy, iz + 1, salt);
		const V3 gf = Gradient(ix + 1, iy, iz + 1, salt);
		const V3 gg = Gradient(ix, iy + 1, iz + 1, salt);
		const V3 gh = Gradient(ix + 1, iy + 1, iz + 1, salt);

		const float va = Dot(ga, w);
		const float vb = Dot(gb, Sub(w, { 1.f, 0.f, 0.f }));
		const float vc = Dot(gc, Sub(w, { 0.f, 1.f, 0.f }));
		const float vd = Dot(gd, Sub(w, { 1.f, 1.f, 0.f }));
		const float ve = Dot(ge, Sub(w, { 0.f, 0.f, 1.f }));
		const float vf = Dot(gf, Sub(w, { 1.f, 0.f, 1.f }));
		const float vg = Dot(gg, Sub(w, { 0.f, 1.f, 1.f }));
		const float vh = Dot(gh, Sub(w, { 1.f, 1.f, 1.f }));

		const float kxy = va - vb - vc + vd;
		const float kyz = va - vc - ve + vg;
		const float kzx = va - vb - ve + vf;
		const float kxyz = -va + vb + vc - vd + ve - vf - vg + vh;

		// d = ga + u.x (gb-ga) + u.y (gc-ga) + u.z (ge-ga) + u.x u.y (ga-gb-gc+gd) + u.y u.z (ga-gc-ge+gg)
		//     + u.z u.x (ga-gb-ge+gf) + u.x u.y u.z (-ga+gb+gc-gd+ge-gf-gg+gh)
		V3 d = ga;
		d = Add(d, Mul(Sub(gb, ga), u.x));
		d = Add(d, Mul(Sub(gc, ga), u.y));
		d = Add(d, Mul(Sub(ge, ga), u.z));
		d = Add(d, Mul(Add(Sub(Sub(ga, gb), gc), gd), u.x * u.y));
		d = Add(d, Mul(Add(Sub(Sub(ga, gc), ge), gg), u.y * u.z));
		d = Add(d, Mul(Add(Sub(Sub(ga, gb), ge), gf), u.z * u.x));
		const V3 gxyz = Add(Sub(Sub(Add(Sub(Add(Sub(gb, ga), gc), gd), ge), gf), gg), gh);
		d = Add(d, Mul(gxyz, u.x * u.y * u.z));

		d.x += du.x * (vb - va + u.y * kxy + u.z * kzx + u.y * u.z * kxyz);
		d.y += du.y * (vc - va + u.z * kyz + u.x * kxy + u.z * u.x * kxyz);
		d.z += du.z * (ve - va + u.x * kzx + u.y * kyz + u.x * u.y * kxyz);
		return d;
	}
}

inline float3 CurlNoise(const float3& p)
{
	using namespace CurlNoiseDetail;
	const V3 x = { p.x, p.y, p.z };
	const V3 dPx = GradientNoiseDeriv(x, kSaltX);
	const V3 dPy = GradientNoiseDeriv(x, kSaltY);
	const V3 dPz = GradientNoiseDeriv(x, kSaltZ);
	return float3{ dPz.y - dPy.z, dPx.z - dPz.x, dPy.x - dPx.y };
}
