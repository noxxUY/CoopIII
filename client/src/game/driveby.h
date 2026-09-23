// The drive-by: the decisions, and nothing that needs the game.
//
// addresses.h, "the drive-by", has the engine side. The short version is that
// a drive-by never goes through CWeapon::Fire, so none of the on-foot shot
// path ever saw one: nothing went on the wire, an observer's copy of the
// driver sat still, and a round that hit another player's ped was thrown away
// on the shooter's own machine, because the culprit the engine names for it is
// the car and not the player.
//
// Three halves, all small:
//
//   the round     combat.cpp detours CWeapon::FireFromCar and sends each round
//                 as C_Shot with weapon 19, the line the shooter's own engine
//                 drew. An observer draws it (flash, light, trail, report)
//                 and never runs the engine's fire path: every culprit and
//                 every event in it is FindPlayerPed(), which there is the
//                 observer.
//   the hit       the ped arm names the car, so "was this the local player's
//                 own doing" has to accept the car the local player is in.
//                 Only for cause 19, which only that one call site pushes.
//   the pose      the arm out of the window is an overlay on the driver's
//                 clump, so it rides animId2 like any other overlay. A seated
//                 ped ignores the pose stream, so ped.cpp drives it for the
//                 seat, the way DoDriveByShootings does for the player.
//
// No layout moves: see the history in protocol.h.
#pragma once

#include "addresses.h"

#include <coopiii/protocol.h>

#include <cmath>
#include <cstdint>

namespace coopiii::game {

inline bool IsDriveByAnim(uint16_t id) {
	return id == ANIM_STD_CAR_DRIVEBY_LEFT || id == ANIM_STD_CAR_DRIVEBY_RIGHT;
}

inline uint16_t DriveByOtherSide(uint16_t id) {
	return id == ANIM_STD_CAR_DRIVEBY_LEFT ? ANIM_STD_CAR_DRIVEBY_RIGHT
	                                       : ANIM_STD_CAR_DRIVEBY_LEFT;
}

// Is this association one DoDriveByShootings is holding? Its own test, with
// the same float compare: an id that is there with a blendDelta below zero
// is on its way out (the -1000 it writes when the look ends) and counts as
// not there.
inline bool DriveByHeld(bool present, float blendDelta) {
	return present && !(blendDelta < 0.0f);
}

// What goes in animId2 while the sender is in a car. The dominant partial is
// usually the drive-by already, but "usually" leans on blend amounts the
// steering animations also have, so a held drive-by is named outright.
inline uint16_t DriveByOverlayOnWire(uint16_t sampled, bool inVehicle, uint16_t held) {
	if (inVehicle && IsDriveByAnim(held))
		return held;
	return sampled;
}

// Which side a round went out of, off the car's own right row. The engine
// builds the left source at -(bbox.max.x + 0.2) along that row, so a round
// going the other way is the right window.
inline uint16_t DriveByAnimForShot(const Vec3 &dir, const Vec3 &carRight) {
	const float d = dir.x * carRight.x + dir.y * carRight.y + dir.z * carRight.z;
	return d < 0.0f ? ANIM_STD_CAR_DRIVEBY_LEFT : ANIM_STD_CAR_DRIVEBY_RIGHT;
}

// A round holds the arm out on its own for this long. The snapshot says it
// too, 25 times a second, but snapshots are unreliable and unordered and a
// round is not: without this a round can arrive between two snapshots that
// both predate the look, and the flash comes out of a driver with his hands
// on the wheel. Three rounds' worth.
constexpr uint32_t DRIVEBY_SHOT_HOLD_MS = 3 * DRIVEBY_ROUND_MS;

// The side a seated ped should be looking out of, or ANIM_NONE.
inline uint16_t DriveByPoseToHold(uint16_t wireAnim2, uint16_t shotAnim, uint32_t shotAgeMs) {
	if (IsDriveByAnim(wireAnim2))
		return wireAnim2;
	if (IsDriveByAnim(shotAnim) && shotAgeMs <= DRIVEBY_SHOT_HOLD_MS)
		return shotAnim;
	return ANIM_NONE;
}

// Is this hit the local player's own doing, for the forwarding rule?
//
// On foot the culprit is the player's ped. A drive-by round on a ped names
// the car instead (`push ebx` at 0x00562BDF), and without this the shooter's
// machine refused the hit on another player's ped and forwarded nothing:
// drive-bys hurt nobody but the shooter's own pedestrians. The car counts
// only for cause 19, because nothing else pushes 19 and a car is the culprit
// of plenty that is not a shot - running somebody over, for one, which is
// never forwarded.
inline bool HitIsOurs(bool culpritIsLocalPed, bool culpritIsOurCar, uint8_t cause) {
	return culpritIsLocalPed || (culpritIsOurCar && cause == WEAPONTYPE_UZI_DRIVEBY);
}

// One round as it goes on the wire. `length` rides ShotBody::speed, which a
// round that is not a projectile never used.
struct DriveByLine {
	Vec3  origin{};
	Vec3  dir{};
	float length = 0.0f;
};

// Off what the shooter's engine did with the round, in the order it is worth
// trusting. The trail first: it is drawn in both arms, from the source, and it
// is what the shooter saw. The ray second, whose far end is the weapon's range
// and not what was drawn, so its length is the one the no-victim arm would
// draw. False when neither is a direction.
inline bool DriveByLineFrom(int trails, const Vec3 &trailStart, const Vec3 &trailEnd,
                            int rays, const Vec3 &rayStart, const Vec3 &rayEnd,
                            DriveByLine &out) {
	auto line = [&out](const Vec3 &a, const Vec3 &b, bool keepLength) {
		const Vec3  d{b.x - a.x, b.y - a.y, b.z - a.z};
		const float len2 = d.x * d.x + d.y * d.y + d.z * d.z;
		if (!(len2 > 1.0e-8f) || !(len2 < 1.0e8f))
			return false;
		const float len = std::sqrt(len2);
		out.origin      = a;
		out.dir         = Vec3{d.x / len, d.y / len, d.z / len};
		out.length      = keepLength ? len : DRIVEBY_MISS_TRAIL;
		return true;
	};
	if (trails > 0 && line(trailStart, trailEnd, true))
		return true;
	return rays > 0 && line(rayStart, rayEnd, false);
}

// The longest trail an observer will draw. The engine's own is at most the
// uzi's range plus the scatter, and this only stops a corrupt float drawing a
// line across the map.
constexpr float DRIVEBY_MAX_TRAIL = 200.0f;

// How long a trail to draw for the length on the wire. Anything that is not a
// usable length is the no-victim arm's 30 m.
inline float DriveByTrailLength(float wire) {
	if (!(wire > 0.0f) || !(wire <= DRIVEBY_MAX_TRAIL))
		return DRIVEBY_MISS_TRAIL;
	return wire;
}

} // namespace coopiii::game
