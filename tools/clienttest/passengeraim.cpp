// A passenger's gun: the pure half of it, client/src/game/passengeraim.h.
//
// The camera, the pose and the round are engine calls and none of them can
// run here. What can is every decision around them: which guns, which gun
// when the one in hand is not one, how fast, when the aim may start or must
// end, which window, and which rounds from a seat an observer replays.

#include "game/passengeraim.h"

#include <cstdio>
#include <limits>

using namespace coopiii;
using namespace coopiii::game;

namespace {

int g_passengerAimFailures = 0;

void Check(bool cond, const char *what) {
	std::printf("  [%s] %s\n", cond ? "ok" : "FAIL", what);
	if (!cond)
		++g_passengerAimFailures;
}

void TestWhichGuns() {
	std::printf("which guns a passenger fires\n");
	Check(PassengerWeaponAllowed(WEAPONTYPE_COLT45) && PassengerWeaponAllowed(WEAPONTYPE_UZI),
	      "the pistol and the uzi");
	Check(!PassengerWeaponAllowed(WEAPONTYPE_UNARMED) &&
	          !PassengerWeaponAllowed(WEAPONTYPE_SHOTGUN) &&
	          !PassengerWeaponAllowed(WEAPONTYPE_AK47) && !PassengerWeaponAllowed(WEAPONTYPE_M16),
	      "not fists, nor the two-handed guns");
	Check(!PassengerWeaponAllowed(WEAPONTYPE_SNIPERRIFLE) &&
	          !PassengerWeaponAllowed(WEAPONTYPE_ROCKETLAUNCHER) &&
	          !PassengerWeaponAllowed(WEAPONTYPE_MOLOTOV) &&
	          !PassengerWeaponAllowed(WEAPONTYPE_GRENADE) &&
	          !PassengerWeaponAllowed(WEAPONTYPE_FLAMETHROWER),
	      "nor the sniper, anything thrown or launched, or the flamethrower");
	Check(!PassengerWeaponAllowed(WEAPONTYPE_UZI_DRIVEBY), "and not the driver's drive-by cause");
}

void TestWhichGunInHand() {
	std::printf("the gun a passenger aims with\n");
	const uint16_t both   = (1u << WEAPONTYPE_COLT45) | (1u << WEAPONTYPE_UZI);
	const uint16_t pistol = 1u << WEAPONTYPE_COLT45;
	Check(PassengerWeaponToUse(WEAPONTYPE_COLT45, both) == WEAPONTYPE_COLT45,
	      "the pistol in hand stays the pistol");
	Check(PassengerWeaponToUse(WEAPONTYPE_SHOTGUN, both) == WEAPONTYPE_UZI,
	      "a shotgun in hand gives way to the uzi first");
	Check(PassengerWeaponToUse(WEAPONTYPE_M16, pistol) == WEAPONTYPE_COLT45,
	      "and to the pistol when there is no uzi");
	Check(PassengerWeaponToUse(WEAPONTYPE_UZI, pistol) == WEAPONTYPE_COLT45,
	      "an uzi with no ammunition is not one");
	Check(PassengerWeaponToUse(WEAPONTYPE_UNARMED, 0) == WEAPONTYPE_UNARMED,
	      "with neither, nothing to aim");
}

void TestHowFast() {
	std::printf("how often a round goes\n");
	Check(PassengerRoundIntervalMs(WEAPONTYPE_UZI, 0.0f, 1.0f) == DRIVEBY_ROUND_MS,
	      "the uzi at the drive-by's 70 ms");
	Check(PassengerRoundIntervalMs(WEAPONTYPE_COLT45, 0.1f, 0.4333f) == 333,
	      "the pistol once per loop of its animation");
	Check(PassengerRoundIntervalMs(WEAPONTYPE_COLT45, 0.1f, 0.12f) == PASSENGER_ROUND_MIN_MS,
	      "never faster than the floor");
	Check(PassengerRoundIntervalMs(WEAPONTYPE_COLT45, 0.0f, 5.0f) == PASSENGER_ROUND_MAX_MS,
	      "never slower than the ceiling");
	const float nan = std::numeric_limits<float>::quiet_NaN();
	Check(PassengerRoundIntervalMs(WEAPONTYPE_COLT45, nan, 1.0f) == PASSENGER_ROUND_DEFAULT_MS &&
	          PassengerRoundIntervalMs(WEAPONTYPE_COLT45, 0.5f, 0.2f) ==
	              PASSENGER_ROUND_DEFAULT_MS,
	      "and a loop that is not one falls back");
}

void TestTheGate() {
	std::printf("when the aim may start\n");
	Check(PassengerAimGate(true, true, true, CAMCONTROL_NOBODY, false, false, false) ==
	          AimGate::Go,
	      "a passenger with a gun, the mouse camera and a free camera");
	Check(PassengerAimGate(false, true, true, 0, false, false, false) == AimGate::NotPassenger,
	      "never at the wheel or on foot");
	Check(PassengerAimGate(true, false, true, 0, false, false, false) == AimGate::NoGun,
	      "never without a passenger gun");
	Check(PassengerAimGate(true, true, false, 0, false, false, false) == AimGate::NoMouseCamera,
	      "never without the mouse camera, which has the only crosshair");
	Check(PassengerAimGate(true, true, true, CAMCONTROL_SCRIPT, false, false, false) ==
	          AimGate::CameraTaken,
	      "never over a camera a script holds");
	Check(PassengerAimGate(true, true, true, CAMCONTROL_SCRIPT, true, false, false) ==
	          AimGate::Go,
	      "but the one we took ourselves goes on");
	Check(PassengerAimGate(true, true, true, 0, false, true, false) == AimGate::Cutscene &&
	          PassengerAimGate(true, true, true, 0, false, false, true) == AimGate::ControlsOff,
	      "nor in a cutscene or with the controls off");

	int ped = 0, other = 0;
	Check(AimCameraStillOurs(CAMCONTROL_SCRIPT, &ped, &ped), "the camera on our ped is ours");
	Check(!AimCameraStillOurs(CAMCONTROL_SCRIPT, &other, &ped) &&
	          !AimCameraStillOurs(CAMCONTROL_NOBODY, &ped, &ped) &&
	          !AimCameraStillOurs(CAMCONTROL_SCRIPT, nullptr, nullptr),
	      "on anything else, or given back, it is not");
}

void TestWhichWindow() {
	std::printf("which window the arm goes out of\n");
	const Vec3 right{1.0f, 0.0f, 0.0f};
	Check(DriveByAnimForShot(Vec3{0.7f, 0.7f, 0.0f}, right) == ANIM_STD_CAR_DRIVEBY_RIGHT,
	      "ahead and to the right, the right window");
	Check(DriveByAnimForShot(Vec3{-0.2f, -0.9f, 0.3f}, right) == ANIM_STD_CAR_DRIVEBY_LEFT,
	      "behind and a little left, the left one");
}

void TestSeatedRounds() {
	std::printf("a round from a seat, on an observer's screen\n");
	Check(ClassifySeatedRound(false, false, WEAPONTYPE_M16) == SeatedRound::NotSeated,
	      "on foot is not a seat's business");
	Check(ClassifySeatedRound(true, false, WEAPONTYPE_UZI) == SeatedRound::Passenger &&
	          ClassifySeatedRound(true, false, WEAPONTYPE_COLT45) == SeatedRound::Passenger,
	      "a passenger's pistol or uzi is replayed");
	Check(ClassifySeatedRound(true, true, WEAPONTYPE_UZI) == SeatedRound::Refuse,
	      "a driver's round that is not the drive-by is not");
	Check(ClassifySeatedRound(true, false, WEAPONTYPE_SHOTGUN) == SeatedRound::Refuse,
	      "nor a passenger's shotgun, a round on foot that raced the seat");
	Check(SeatedPoseWeapon(WEAPONTYPE_COLT45) == WEAPONTYPE_COLT45 &&
	          SeatedPoseWeapon(WEAPONTYPE_UZI) == WEAPONTYPE_UZI &&
	          SeatedPoseWeapon(WEAPONTYPE_M16) == WEAPONTYPE_UZI,
	      "the pose holds the passenger's gun, and the uzi otherwise");
}

void TestTheSameCar() {
	std::printf("nobody in the shooter's car\n");
	int car = 0, other = 0;
	Check(SameCarHit(&car, &car), "a victim in the same car is refused");
	Check(!SameCarHit(&car, &other) && !SameCarHit(&car, nullptr),
	      "one in another car or on foot is not");
	Check(!SameCarHit(nullptr, nullptr), "and a round not from a seat is nobody's business here");

	const Vec3 m = PassengerMuzzle(Vec3{1.0f, 2.0f, 3.0f}, Vec3{0.0f, 1.0f, 0.0f});
	Check(m.x == 1.0f && m.y == 2.0f + PASSENGER_MUZZLE_AHEAD && m.z == 3.0f,
	      "the muzzle is ahead of the hand along the aim");
}

} // namespace

int RunPassengerAimTests() {
	g_passengerAimFailures = 0;
	TestWhichGuns();
	TestWhichGunInHand();
	TestHowFast();
	TestTheGate();
	TestWhichWindow();
	TestSeatedRounds();
	TestTheSameCar();
	return g_passengerAimFailures;
}
