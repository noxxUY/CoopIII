// A passenger's gun: aimed freely, out of any window, with the on-foot mouse
// camera and its crosshair. docs/protocol.md 1.42 is the design; addresses.h,
// "a passenger's free aim", has the engine side.
//
// GTA III's own drive-by is the driver's: DoDriveByShootings runs for the car
// the local pad drives, the uzi only, left or right with the look keys. A
// passenger has nothing. So a passenger borrows what the player has on foot:
//
//   the camera   the engine's own MODE_FOLLOWPED mouse camera, handed the
//                passenger by CCamera::TakeControl while the aim key is held
//                and given back with RestoreWithJumpCut. Its crosshair is
//                CHud's own, which draws for that mode on its own. Four call
//                sites inside it are taken so its clip ignores the car the
//                passenger sits in; without that it pulls itself inside.
//   the round    CWeapon::Fire on the passenger's own gun, with
//                CWorld::pIgnoreEntity on the car: the on-foot fire path end
//                to end, the crosshair's line, every sampler and the C_Shot
//                that already goes out for a round on foot. The occupants are
//                out of the line already: a ped in a seat has bUsesCollision
//                clear.
//   the pose     DRIVEBY_L or DRIVEBY_R, whichever window faces the aim. They
//                are the only two animations in ped.ifp that hold a gun out
//                of a car; the arm and the head go out of the window that side,
//                and the flash, the trail and the hit come off the real line.
//                They ride animId2 like the driver's, so every screen already
//                draws them (ped.cpp, ApplySeatedDriveBy).
//
// Everything above the line is arithmetic, and tools/clienttest walks it.
#pragma once

#include "addresses.h"
#include "driveby.h"

#include <cmath>
#include <cstdint>

namespace coopiii::game {

// ---- decisions ------------------------------------------------------------

// Which guns a passenger may use. The one-handed pistol and the uzi, as San
// Andreas has it: the only pose there is holds the gun in one hand out of a
// window. The shotgun and the rifles are two-handed on foot and have no pose
// here; the thrown and launched weapons would start inside the car (the rocket
// arm of AddProjectile ignores the fire source), the sniper fires along its own
// camera and the flamethrower sets the car alight.
inline bool PassengerWeaponAllowed(uint8_t weapon) {
	return weapon == WEAPONTYPE_COLT45 || weapon == WEAPONTYPE_UZI;
}

// The gun to aim with when the one in hand is not allowed: the uzi first, the
// way RemoveWeaponWhenEnteringVehicle picks it for the player, then the
// pistol. `carried` has bit w set for each weapon w with ammunition.
inline uint8_t PassengerWeaponToUse(uint8_t current, uint16_t carried) {
	if (PassengerWeaponAllowed(current) && (carried & (1u << current)))
		return current;
	if (carried & (1u << WEAPONTYPE_UZI))
		return WEAPONTYPE_UZI;
	if (carried & (1u << WEAPONTYPE_COLT45))
		return WEAPONTYPE_COLT45;
	return WEAPONTYPE_UNARMED;
}

// How long between two rounds. The uzi at the drive-by's own 70 ms; anything
// else once per loop of its firing animation, which is what CPed::FireGun
// fires on: m_fAnimLoopEnd - m_fAnimLoopStart, already in seconds
// (LoadWeaponData divides the frame counts by 30).
constexpr uint32_t PASSENGER_ROUND_MIN_MS     = 100;
constexpr uint32_t PASSENGER_ROUND_MAX_MS     = 1000;
constexpr uint32_t PASSENGER_ROUND_DEFAULT_MS = 300;

inline uint32_t PassengerRoundIntervalMs(uint8_t weapon, float loopStart, float loopEnd) {
	if (weapon == WEAPONTYPE_UZI)
		return DRIVEBY_ROUND_MS;
	const float s = (loopEnd - loopStart) * 1000.0f;
	if (!(s > 0.0f) || !(s < 100000.0f))
		return PASSENGER_ROUND_DEFAULT_MS;
	const uint32_t ms = static_cast<uint32_t>(s);
	return ms < PASSENGER_ROUND_MIN_MS   ? PASSENGER_ROUND_MIN_MS
	       : ms > PASSENGER_ROUND_MAX_MS ? PASSENGER_ROUND_MAX_MS
	                                     : ms;
}

// Whether the aim may start, or go on, this frame. Everything the engine
// would have to be asked, asked first:
//   passenger     in a car, not at its wheel, and seated (PED_DRIVING)
//   gun           a passenger gun with ammunition
//   mouseCamera   CCamera::m_bUseMouse3rdPerson; without it MODE_FOLLOWPED is
//                 the pad camera, which has no crosshair
//   camera        nobody has the camera (0), or it is ours already
//   cutscene      the widescreen bars are up
//   controls      CPad's DisablePlayerControls
enum class AimGate : uint8_t {
	Go,
	NotPassenger,
	NoGun,
	NoMouseCamera,
	CameraTaken,
	Cutscene,
	ControlsOff,
};

inline AimGate PassengerAimGate(bool passenger, bool gun, bool mouseCamera, int32_t whoControls,
                                bool cameraIsOurs, bool cutscene, bool controlsOff) {
	if (!passenger)
		return AimGate::NotPassenger;
	if (!gun)
		return AimGate::NoGun;
	if (!mouseCamera)
		return AimGate::NoMouseCamera;
	if (cutscene)
		return AimGate::Cutscene;
	if (controlsOff)
		return AimGate::ControlsOff;
	if (!cameraIsOurs && whoControls != CAMCONTROL_NOBODY)
		return AimGate::CameraTaken;
	return AimGate::Go;
}

// Is the camera still the one we handed the passenger? A script, or the unique
// jump's shot, may take it while we aim, and then it is theirs to give back.
inline bool AimCameraStillOurs(int32_t whoControls, const void *target, const void *ped) {
	return whoControls == CAMCONTROL_SCRIPT && target != nullptr && target == ped;
}

// A round that goes off from a seat, on the observer's side. The driver's is
// weapon 19 and drawn (driveby.h); a passenger's is a passenger gun, and is
// replayed through the engine like a round on foot, with the car left out of
// its line. Anything else from a seat is a round on foot that raced the
// seating, and is refused as it always was.
enum class SeatedRound : uint8_t {
	NotSeated,
	Passenger,
	Refuse,
};

inline SeatedRound ClassifySeatedRound(bool inVehicle, bool isDriver, uint8_t weapon) {
	if (!inVehicle)
		return SeatedRound::NotSeated;
	if (!isDriver && PassengerWeaponAllowed(weapon))
		return SeatedRound::Passenger;
	return SeatedRound::Refuse;
}

// Whose hand an observer puts in a seated player's drive-by pose: the gun on
// the wire when it is a passenger gun, the uzi otherwise, which is the only
// gun a driver's drive-by ever fires.
inline uint8_t SeatedPoseWeapon(uint8_t wireWeapon) {
	return PassengerWeaponAllowed(wireWeapon) ? wireWeapon : WEAPONTYPE_UZI;
}

// A hit a passenger's round found on somebody in the same car. The line leaves
// the car and its occupants out, so this is the backstop: never forwarded,
// never applied. `victimCar` is null for a victim on foot.
inline bool SameCarHit(const void *shooterCar, const void *victimCar) {
	return shooterCar != nullptr && shooterCar == victimCar;
}

// Where the round leaves from: the hand, a little way along the aim so the
// flash is at the muzzle rather than in the palm.
constexpr float PASSENGER_MUZZLE_AHEAD = 0.3f;

inline Vec3 PassengerMuzzle(const Vec3 &hand, const Vec3 &aim) {
	return Vec3{hand.x + aim.x * PASSENGER_MUZZLE_AHEAD, hand.y + aim.y * PASSENGER_MUZZLE_AHEAD,
	            hand.z + aim.z * PASSENGER_MUZZLE_AHEAD};
}

// ---- the engine seam ------------------------------------------------------

// The four call sites in the mouse camera. Each is checked to still call what
// the retail image calls there; all four or none, because a camera that
// ignores the car in one test and not the next still ends up inside it. False
// leaves the feature off, logged.
bool InstallPassengerAim();
void RemovePassengerAim();

// Once a frame, before CCamera::Process (game/ridecam.cpp): the gate, the
// camera, the pose and the rounds. Cheap and silent for anybody not riding.
void PassengerAimBeforeCamera();

// True while the local player's passenger round is inside CWeapon::Fire, and
// the car it is being fired from. vehicle.cpp refuses that car's damage for
// the length of it.
void *PassengerRoundCar();

} // namespace coopiii::game
