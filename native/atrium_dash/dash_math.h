// Small vector maths for the dash (right-handed, -Z forward, as OpenXR).
#pragma once
#include <cmath>
#include "atrium_dash_shared.h"

struct V3
{
	float x = 0, y = 0, z = 0;
	V3() = default;
	V3(float x_, float y_, float z_) : x(x_), y(y_), z(z_) {}
	V3 operator+(V3 o) const { return { x + o.x, y + o.y, z + o.z }; }
	V3 operator-(V3 o) const { return { x - o.x, y - o.y, z - o.z }; }
	V3 operator*(float s) const { return { x * s, y * s, z * s }; }
	float dot(V3 o) const { return x * o.x + y * o.y + z * o.z; }
	V3 cross(V3 o) const { return { y * o.z - z * o.y, z * o.x - x * o.z, x * o.y - y * o.x }; }
	float len() const { return std::sqrt(dot(*this)); }
	V3 norm() const { float l = len(); return l > 1e-6f ? *this * (1.0f / l) : V3(0, 0, 0); }
};

struct Q
{
	float x = 0, y = 0, z = 0, w = 1;
	Q() = default;
	Q(float x_, float y_, float z_, float w_) : x(x_), y(y_), z(z_), w(w_) {}
	static Q axisAngle(V3 a, float rad)
	{
		a = a.norm();
		float s = std::sin(rad * 0.5f);
		return { a.x * s, a.y * s, a.z * s, std::cos(rad * 0.5f) };
	}
	Q operator*(Q b) const
	{
		return { w * b.x + x * b.w + y * b.z - z * b.y, w * b.y - x * b.z + y * b.w + z * b.x,
		         w * b.z + x * b.y - y * b.x + z * b.w, w * b.w - x * b.x - y * b.y - z * b.z };
	}
	Q conj() const { return { -x, -y, -z, w }; }
	V3 rotate(V3 v) const
	{
		V3 u(x, y, z);
		V3 t = u.cross(v) * 2.0f;
		return v + t * w + u.cross(t);
	}
};

struct Pose
{
	V3 p;
	Q q;
	Pose() = default;
	Pose(V3 p_, Q q_) : p(p_), q(q_) {}
	explicit Pose(const AtriumDashPose& d) : p(d.px, d.py, d.pz), q(d.qx, d.qy, d.qz, d.qw) {}
	AtriumDashPose raw() const { return { p.x, p.y, p.z, q.x, q.y, q.z, q.w }; }
	V3 apply(V3 v) const { return p + q.rotate(v); }
	V3 forward() const { return q.rotate(V3(0, 0, -1)); }
};

// Column-major 4x4 (HLSL reads it as row-major when transposed; the dash uploads transposed).
struct M4
{
	float m[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
	float& at(int r, int c) { return m[c * 4 + r]; }
	float get(int r, int c) const { return m[c * 4 + r]; }
	M4 operator*(const M4& b) const
	{
		M4 o;
		for (int r = 0; r < 4; r++)
			for (int c = 0; c < 4; c++) {
				float s = 0;
				for (int k = 0; k < 4; k++) s += get(r, k) * b.get(k, c);
				o.at(r, c) = s;
			}
		return o;
	}
	static M4 fromPose(const Pose& p, V3 scale = V3(1, 1, 1))
	{
		M4 o;
		V3 X = p.q.rotate(V3(scale.x, 0, 0)), Y = p.q.rotate(V3(0, scale.y, 0)), Z = p.q.rotate(V3(0, 0, scale.z));
		o.at(0, 0) = X.x; o.at(1, 0) = X.y; o.at(2, 0) = X.z;
		o.at(0, 1) = Y.x; o.at(1, 1) = Y.y; o.at(2, 1) = Y.z;
		o.at(0, 2) = Z.x; o.at(1, 2) = Z.y; o.at(2, 2) = Z.z;
		o.at(0, 3) = p.p.x; o.at(1, 3) = p.p.y; o.at(2, 3) = p.p.z;
		return o;
	}
	// The view matrix of an eye at `eye` (inverse of its pose).
	static M4 view(const Pose& eye)
	{
		Q inv = eye.q.conj();
		V3 t = inv.rotate(eye.p * -1.0f);
		return fromPose(Pose(t, inv));
	}
	// OpenXR-style asymmetric projection to D3D clip space (z 0..1), reversed nothing.
	static M4 projection(const AtriumDashFov& f, float n, float fa)
	{
		float l = std::tan(f.left), r = std::tan(f.right), u = std::tan(f.up), d = std::tan(f.down);
		M4 o;
		o.at(0, 0) = 2 / (r - l);
		o.at(0, 2) = (r + l) / (r - l);
		o.at(1, 1) = 2 / (u - d);
		o.at(1, 2) = (u + d) / (u - d);
		o.at(2, 2) = -fa / (fa - n);
		o.at(2, 3) = -(fa * n) / (fa - n);
		o.at(3, 2) = -1;
		o.at(3, 3) = 0;
		return o;
	}
};
