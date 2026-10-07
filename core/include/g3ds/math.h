// Minimal math. Conventions follow ZenGin/Direct3D so game data needs no
// conversion: left-handed, Y up, +Z forward, units are centimetres.
// Matrices are column-major with column vectors (clip = P * V * M * v).
#pragma once
#include "g3ds/base.h"
#include <math.h>

namespace g3 {

struct Vec3 {
	float x, y, z;
};

inline Vec3 operator+(Vec3 a, Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline Vec3 operator-(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline Vec3 operator*(Vec3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
inline float dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline Vec3 cross(Vec3 a, Vec3 b) {
	return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
inline float length(Vec3 a) { return sqrtf(dot(a, a)); }
inline Vec3 normalize(Vec3 a) {
	float l = length(a);
	return l > 1e-12f ? a * (1.0f / l) : Vec3 {0, 0, 0};
}

struct Vec4 {
	float x, y, z, w;
};

struct Mat4 {
	float m[16]; // m[col * 4 + row]

	float& at(int row, int col) { return m[col * 4 + row]; }
	float at(int row, int col) const { return m[col * 4 + row]; }
};

inline Mat4 identity() {
	Mat4 r {};
	r.m[0] = r.m[5] = r.m[10] = r.m[15] = 1.0f;
	return r;
}

inline Mat4 mul(const Mat4& a, const Mat4& b) {
	Mat4 r;
	for (int c = 0; c < 4; ++c)
		for (int row = 0; row < 4; ++row) {
			float s = 0;
			for (int k = 0; k < 4; ++k) s += a.at(row, k) * b.at(k, c);
			r.at(row, c) = s;
		}
	return r;
}

inline Vec4 transform(const Mat4& m, Vec4 v) {
	return {
	    m.at(0, 0) * v.x + m.at(0, 1) * v.y + m.at(0, 2) * v.z + m.at(0, 3) * v.w,
	    m.at(1, 0) * v.x + m.at(1, 1) * v.y + m.at(1, 2) * v.z + m.at(1, 3) * v.w,
	    m.at(2, 0) * v.x + m.at(2, 1) * v.y + m.at(2, 2) * v.z + m.at(2, 3) * v.w,
	    m.at(3, 0) * v.x + m.at(3, 1) * v.y + m.at(3, 2) * v.z + m.at(3, 3) * v.w,
	};
}

// translate(origin) * scale(s): the per-chunk model matrix.
inline Mat4 translateScale(const float origin[3], float s) {
	Mat4 r = identity();
	r.at(0, 0) = r.at(1, 1) = r.at(2, 2) = s;
	r.at(0, 3) = origin[0];
	r.at(1, 3) = origin[1];
	r.at(2, 3) = origin[2];
	return r;
}

// Left-handed perspective, depth 0..1 (near..far), clip.w = view z.
inline Mat4 perspectiveLH(float fovY, float aspect, float zn, float zf) {
	float f = 1.0f / tanf(fovY * 0.5f);
	Mat4 r {};
	r.at(0, 0) = f / aspect;
	r.at(1, 1) = f;
	r.at(2, 2) = zf / (zf - zn);
	r.at(2, 3) = -zn * zf / (zf - zn);
	r.at(3, 2) = 1.0f;
	return r;
}

// yaw rotates around +Y (0 looks down +Z), pitch looks up for positive values.
inline Vec3 forwardFromYawPitch(float yaw, float pitch) {
	float cp = cosf(pitch);
	return {sinf(yaw) * cp, sinf(pitch), cosf(yaw) * cp};
}

inline Mat4 viewLH(Vec3 pos, float yaw, float pitch) {
	Vec3 f = forwardFromYawPitch(yaw, pitch);
	Vec3 r = normalize(cross({0, 1, 0}, f));
	Vec3 u = cross(f, r);
	Mat4 v = identity();
	v.at(0, 0) = r.x; v.at(0, 1) = r.y; v.at(0, 2) = r.z; v.at(0, 3) = -dot(r, pos);
	v.at(1, 0) = u.x; v.at(1, 1) = u.y; v.at(1, 2) = u.z; v.at(1, 3) = -dot(u, pos);
	v.at(2, 0) = f.x; v.at(2, 1) = f.y; v.at(2, 2) = f.z; v.at(2, 3) = -dot(f, pos);
	return v;
}

// Rendering relative to the eye. Chunk positions are whole steps of a
// world-wide lattice; subtracting a lattice point near the eye from them is
// exact, even in the 24-bit floats of the PICA200 vertex shader, as long as
// the result stays below 2^17 steps. A vertex shared by two chunks then
// reaches the view matrix as the very same number from both, and the seam
// between them cannot open. `anchor` is that lattice point; `view` is the
// view matrix for positions relative to it.
constexpr float EYE_LATTICE = 0.125f; // cm: the bake's finest position step

struct EyeFrame {
	Mat4 view;
	float anchor[3];
};

inline EyeFrame eyeFrame(const Mat4& view, Vec3 eye) {
	EyeFrame f;
	f.view = view;
	const float e[3] = {eye.x, eye.y, eye.z};
	float d[3];
	for (int a = 0; a < 3; ++a) {
		f.anchor[a] = floorf(e[a] / EYE_LATTICE + 0.5f) * EYE_LATTICE;
		d[a] = f.anchor[a] - e[a];
	}
	for (int r = 0; r < 3; ++r) f.view.at(r, 3) = view.at(r, 0) * d[0] + view.at(r, 1) * d[1] + view.at(r, 2) * d[2];
	return f;
}

struct Frustum {
	float p[6][4]; // inward-facing planes: ax + by + cz + d >= 0 is inside
};

// Gribb/Hartmann extraction for a 0..1 depth range projection.
inline Frustum frustumFromViewProj(const Mat4& vp) {
	Frustum f;
	auto row = [&](int r, int k) { return vp.at(r, k); };
	for (int k = 0; k < 4; ++k) {
		f.p[0][k] = row(3, k) + row(0, k); // left
		f.p[1][k] = row(3, k) - row(0, k); // right
		f.p[2][k] = row(3, k) + row(1, k); // bottom
		f.p[3][k] = row(3, k) - row(1, k); // top
		f.p[4][k] = row(2, k);             // near
		f.p[5][k] = row(3, k) - row(2, k); // far
	}
	return f;
}

inline bool aabbInFrustum(const Frustum& f, const float mn[3], const float mx[3]) {
	for (int i = 0; i < 6; ++i) {
		const float* pl = f.p[i];
		// Farthest corner along the plane normal.
		float x = pl[0] >= 0 ? mx[0] : mn[0];
		float y = pl[1] >= 0 ? mx[1] : mn[1];
		float z = pl[2] >= 0 ? mx[2] : mn[2];
		if (pl[0] * x + pl[1] * y + pl[2] * z + pl[3] < 0) return false;
	}
	return true;
}

} // namespace g3
