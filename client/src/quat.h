// Rotation on the wire.
//
// A ped only needs a heading, so PlayerStateBody carries one float. A
// vehicle doesn't get off that easy: it pitches over kerbs, rolls onto its
// roof, lands upside down, and a yaw-only sync would make all of that just
// disappear. So VehicleStateBody carries a quaternion instead
// (docs/protocol.md §1.7), and this is the conversion.
//
// The engine side is a CMatrix, whose first three rows are the right,
// forward and up axes (re3 Matrix.h: rx/ry/rz, fx/fy/fz, ux/uy/uz). GTA III
// is right-handed and Z-up.
//
// The only thing that actually has to hold is that a matrix survives the
// round trip - both ends of the wire are CoopIII, the sender writes what the
// receiver reads, and no third party cares about the convention. The forward
// and inverse below are the standard mutually-inverse pair, and basetest
// round-trips them over rotations picked to hit all four branches of the
// reconstruction, including the ones only an upside-down car reaches.
#pragma once

#include <coopiii/protocol.h>

#include <cmath>

namespace coopiii {

// Rows of the rotation matrix, in CMatrix's order.
inline Quat QuatFromAxes(const Vec3 &right, const Vec3 &forward, const Vec3 &up) {
	// m[row][col]; row 0 is right, row 1 is forward, row 2 is up.
	const float m00 = right.x,   m01 = right.y,   m02 = right.z;
	const float m10 = forward.x, m11 = forward.y, m12 = forward.z;
	const float m20 = up.x,      m21 = up.y,      m22 = up.z;

	const float trace = m00 + m11 + m22;
	Quat        q{};

	// Four branches, picking whichever denominator is largest. Using the
	// trace alone loses all precision as it approaches -1, and that's not
	// some corner case here - that's just a car on its roof.
	if (trace > 0.0f) {
		const float s = std::sqrt(trace + 1.0f) * 2.0f;
		q.w = 0.25f * s;
		q.x = (m21 - m12) / s;
		q.y = (m02 - m20) / s;
		q.z = (m10 - m01) / s;
	} else if (m00 > m11 && m00 > m22) {
		const float s = std::sqrt(1.0f + m00 - m11 - m22) * 2.0f;
		q.w = (m21 - m12) / s;
		q.x = 0.25f * s;
		q.y = (m01 + m10) / s;
		q.z = (m02 + m20) / s;
	} else if (m11 > m22) {
		const float s = std::sqrt(1.0f + m11 - m00 - m22) * 2.0f;
		q.w = (m02 - m20) / s;
		q.x = (m01 + m10) / s;
		q.y = 0.25f * s;
		q.z = (m12 + m21) / s;
	} else {
		const float s = std::sqrt(1.0f + m22 - m00 - m11) * 2.0f;
		q.w = (m10 - m01) / s;
		q.x = (m02 + m20) / s;
		q.y = (m12 + m21) / s;
		q.z = 0.25f * s;
	}
	return q;
}

// The inverse. Normalises first - a quaternion that's been through the wire
// as four floats isn't exactly unit length any more, and feeding a non-unit
// one through here scales the basis, which the engine reads as a scaled car.
inline void AxesFromQuat(const Quat &q, Vec3 &right, Vec3 &forward, Vec3 &up) {
	const float len2 = q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w;
	if (!(len2 > 1e-12f)) {
		// Degenerate case: hand back identity instead of NaNs. A car briefly
		// at identity orientation is a bug you can spot; a car with a NaN
		// matrix takes the renderer down with it.
		right   = Vec3{1.0f, 0.0f, 0.0f};
		forward = Vec3{0.0f, 1.0f, 0.0f};
		up      = Vec3{0.0f, 0.0f, 1.0f};
		return;
	}
	const float inv = 1.0f / std::sqrt(len2);
	const float x = q.x * inv, y = q.y * inv, z = q.z * inv, w = q.w * inv;

	// Rows, not columns. Write the columns instead and you get the
	// transpose, which is the inverse rotation - it round-trips identity and
	// anything symmetric fine, so it looks correct right up until a car
	// actually turns.
	right   = Vec3{1.0f - 2.0f * (y * y + z * z), 2.0f * (x * y - w * z),
	             2.0f * (x * z + w * y)};
	forward = Vec3{2.0f * (x * y + w * z), 1.0f - 2.0f * (x * x + z * z),
	               2.0f * (y * z - w * x)};
	up      = Vec3{2.0f * (x * z - w * y), 2.0f * (y * z + w * x),
	          1.0f - 2.0f * (x * x + y * y)};
}

} // namespace coopiii
