// The drive-by: the pure half of it, client/src/game/driveby.h.
//
// Neither detour can run here, and neither can the pose on a seated ped. What
// can is every decision they make: which side a round went out of, which side
// a driver should be leaning out of, what goes on the wire for a round, and
// which hits count as ours.

#include "game/combat.h"
#include "game/driveby.h"

#include <cmath>
#include <cstdio>
#include <limits>

using namespace coopiii;
using namespace coopiii::game;

namespace {

int g_driveByFailures = 0;

void Check(bool cond, const char *what) {
	std::printf("  [%s] %s\n", cond ? "ok" : "FAIL", what);
	if (!cond)
		++g_driveByFailures;
}

bool Near(float a, float b) { return std::fabs(a - b) < 1e-4f; }

void TestTheTwoSides() {
	std::printf("the two drive-by animations\n");
	Check(IsDriveByAnim(ANIM_STD_CAR_DRIVEBY_LEFT) && IsDriveByAnim(ANIM_STD_CAR_DRIVEBY_RIGHT),
	      "77h and 78h are the drive-by");
	Check(!IsDriveByAnim(ANIM_NONE) && !IsDriveByAnim(0x76) && !IsDriveByAnim(0x79),
	      "and their neighbours are not");
	Check(DriveByOtherSide(ANIM_STD_CAR_DRIVEBY_LEFT) == ANIM_STD_CAR_DRIVEBY_RIGHT &&
	          DriveByOtherSide(ANIM_STD_CAR_DRIVEBY_RIGHT) == ANIM_STD_CAR_DRIVEBY_LEFT,
	      "each one's other side is the other one");
}

void TestHeldTheWayTheEngineTestsIt() {
	std::printf("a drive-by the engine is still holding\n");
	const float nan = std::numeric_limits<float>::quiet_NaN();
	Check(DriveByHeld(true, 0.0f), "blendDelta 0, as AddAnimation leaves it");
	Check(DriveByHeld(true, 4.0f), "blending in");
	Check(!DriveByHeld(true, DRIVEBY_ANIM_DROP_DELTA), "the -1000 the look's end writes");
	Check(!DriveByHeld(true, -0.001f), "any negative delta");
	Check(DriveByHeld(true, nan),
	      "NaN is held, as the x87 compare at 0x005640F0 falls through to SetRun");
	Check(!DriveByHeld(false, 0.0f), "not on the clump at all");
}

void TestWhatGoesInAnimId2() {
	std::printf("the overlay on the wire while in a car\n");
	constexpr uint16_t STEER = 0x70;
	Check(DriveByOverlayOnWire(STEER, true, ANIM_STD_CAR_DRIVEBY_LEFT) ==
	          ANIM_STD_CAR_DRIVEBY_LEFT,
	      "a held drive-by beats whatever partial was dominant");
	Check(DriveByOverlayOnWire(STEER, true, ANIM_NONE) == STEER,
	      "no drive-by leaves the sampled partial alone");
	Check(DriveByOverlayOnWire(STEER, false, ANIM_STD_CAR_DRIVEBY_RIGHT) == STEER,
	      "on foot nothing is overridden");
	Check(DriveByOverlayOnWire(ANIM_NONE, true, 0x10) == ANIM_NONE,
	      "an id that is not a drive-by is never named");
}

void TestWhichWindow() {
	std::printf("which window a round went out of\n");
	const Vec3 right{1.0f, 0.0f, 0.0f};
	Check(DriveByAnimForShot(Vec3{-1.0f, 0.0f, 0.0f}, right) == ANIM_STD_CAR_DRIVEBY_LEFT,
	      "against the car's right row is the left window");
	Check(DriveByAnimForShot(Vec3{1.0f, 0.0f, 0.0f}, right) == ANIM_STD_CAR_DRIVEBY_RIGHT,
	      "along it is the right one");
	Check(DriveByAnimForShot(Vec3{-0.9f, 0.3f, -0.2f}, right) == ANIM_STD_CAR_DRIVEBY_LEFT,
	      "the scatter and the auto-aim do not move it to the other side");

	// A car facing east: forward (1,0,0), right (0,-1,0).
	const Vec3 eastRight{0.0f, -1.0f, 0.0f};
	Check(DriveByAnimForShot(Vec3{0.0f, 1.0f, 0.0f}, eastRight) == ANIM_STD_CAR_DRIVEBY_LEFT,
	      "a car facing east shoots north out of its left window");
	Check(DriveByAnimForShot(Vec3{0.0f, -1.0f, 0.0f}, eastRight) == ANIM_STD_CAR_DRIVEBY_RIGHT,
	      "and south out of its right one");
}

void TestWhichSideToHold() {
	std::printf("which side a seated driver leans out of\n");
	const uint16_t L = ANIM_STD_CAR_DRIVEBY_LEFT;
	const uint16_t R = ANIM_STD_CAR_DRIVEBY_RIGHT;
	Check(DriveByPoseToHold(L, ANIM_NONE, 0) == L, "the snapshot says left");
	Check(DriveByPoseToHold(R, L, 0) == R, "the snapshot wins over a round");
	Check(DriveByPoseToHold(ANIM_NONE, L, 0) == L,
	      "a round that beat the snapshot here holds the arm out");
	Check(DriveByPoseToHold(ANIM_NONE, R, DRIVEBY_SHOT_HOLD_MS) == R, "up to the hold");
	Check(DriveByPoseToHold(ANIM_NONE, R, DRIVEBY_SHOT_HOLD_MS + 1) == ANIM_NONE,
	      "and not past it");
	Check(DriveByPoseToHold(0x70, ANIM_NONE, 0) == ANIM_NONE,
	      "a steering partial on the wire is not a look out of the window");
	Check(DriveByPoseToHold(ANIM_NONE, ANIM_NONE, 0) == ANIM_NONE, "nothing, nothing");
	Check(DriveByPoseToHold(ANIM_NONE, L, 0xFFFFFFF0u) == ANIM_NONE,
	      "a round from before the clock wrapped is old, not new");
	Check(DRIVEBY_SHOT_HOLD_MS >= 2 * DRIVEBY_ROUND_MS,
	      "the hold outlasts the gap between two rounds");
}

void TestWhoseHitItIs() {
	std::printf("which hits are ours to forward\n");
	Check(HitIsOurs(true, false, WEAPONTYPE_UZI), "our ped, any cause");
	Check(HitIsOurs(true, false, WEAPONTYPE_UZI_DRIVEBY), "our ped with 19 too");
	Check(HitIsOurs(false, true, WEAPONTYPE_UZI_DRIVEBY),
	      "our car with 19: the drive-by's ped arm names the car (0x00562BDF)");
	Check(!HitIsOurs(false, true, WEAPONTYPE_RUNOVERBYCAR),
	      "our car running somebody over is not a shot");
	Check(!HitIsOurs(false, true, WEAPONTYPE_RAMMEDBYCAR), "nor ramming them");
	Check(!HitIsOurs(false, true, WEAPONTYPE_UZI), "nor anything else it is blamed for");
	Check(!HitIsOurs(false, false, WEAPONTYPE_UZI_DRIVEBY),
	      "somebody else's car with 19 is not ours");
	Check(IsForwardableDamage(WEAPONTYPE_UZI_DRIVEBY),
	      "and 19 is a cause the shooter decides, so it goes out");
	Check(!IsReplayableWeapon(WEAPONTYPE_UZI_DRIVEBY),
	      "while an older observer refuses a round of it rather than firing an uzi");
}

void TestTheLineOnTheWire() {
	std::printf("the line a round goes out as\n");
	const Vec3 a{10.0f, 20.0f, 5.0f};
	const Vec3 hit{10.0f, 32.0f, 5.0f};
	const Vec3 rayEnd{10.0f, 70.0f, 5.0f};
	DriveByLine l;

	Check(DriveByLineFrom(1, a, hit, 1, a, rayEnd, l), "a drawn trail is a line");
	Check(Near(l.origin.x, 10.0f) && Near(l.origin.y, 20.0f) && Near(l.origin.z, 5.0f),
	      "from where it was drawn");
	Check(Near(l.dir.x, 0.0f) && Near(l.dir.y, 1.0f) && Near(l.dir.z, 0.0f), "along it");
	Check(Near(l.length, 12.0f), "and as long as it was drawn");

	Check(DriveByLineFrom(0, Vec3{}, Vec3{}, 1, a, rayEnd, l), "no trail, a ray");
	Check(Near(l.dir.y, 1.0f) && Near(l.length, DRIVEBY_MISS_TRAIL),
	      "and the ray's length is the range, so it draws the miss arm's 30 m");

	Check(DriveByLineFrom(1, a, a, 1, a, rayEnd, l) && Near(l.length, DRIVEBY_MISS_TRAIL),
	      "a trail of no length falls back to the ray");

	const float nan = std::numeric_limits<float>::quiet_NaN();
	Check(!DriveByLineFrom(1, a, Vec3{nan, 0.0f, 0.0f}, 0, Vec3{}, Vec3{}, l),
	      "a NaN trail and no ray is nothing");
	Check(!DriveByLineFrom(0, Vec3{}, Vec3{}, 0, Vec3{}, Vec3{}, l), "nothing is nothing");
}

void TestTheTrailAnObserverDraws() {
	std::printf("how long a trail an observer draws\n");
	const float nan = std::numeric_limits<float>::quiet_NaN();
	const float inf = std::numeric_limits<float>::infinity();
	Check(Near(DriveByTrailLength(12.5f), 12.5f), "what the wire says");
	Check(Near(DriveByTrailLength(DRIVEBY_MAX_TRAIL), DRIVEBY_MAX_TRAIL), "up to the cap");
	Check(Near(DriveByTrailLength(0.0f), DRIVEBY_MISS_TRAIL), "0 is the miss arm's 30 m");
	Check(Near(DriveByTrailLength(-3.0f), DRIVEBY_MISS_TRAIL), "so is a negative");
	Check(Near(DriveByTrailLength(nan), DRIVEBY_MISS_TRAIL), "and NaN");
	Check(Near(DriveByTrailLength(inf), DRIVEBY_MISS_TRAIL), "and infinity");
	Check(Near(DriveByTrailLength(DRIVEBY_MAX_TRAIL * 2.0f), DRIVEBY_MISS_TRAIL),
	      "and a line across the map");
}

void TestTheSniperRoundIsHeard() {
	std::printf("somebody else's sniper round\n");
	Check(!IsReplayableWeapon(WEAPONTYPE_SNIPERRIFLE),
	      "is not replayed - FireSniper would fire out of our own camera");
	Check(IsForwardableDamage(WEAPONTYPE_SNIPERRIFLE), "and its hit still comes from the shooter");
	Vec3 start, end;
	Check(SniperProbe(Vec3{10, 20, 30}, Vec3{0, 2, 0}, start, end) &&
	          Near(start.y, 20.0f + SNIPER_PROBE_SKIP_M) && Near(end.y, 20.0f + SNIPER_PROBE_M) &&
	          Near(start.x, 10.0f) && Near(end.z, 30.0f),
	      "its impact is looked for along the line sent, starting clear of the shooter");
	const float nan = std::numeric_limits<float>::quiet_NaN();
	Check(!SniperProbe(Vec3{0, 0, 0}, Vec3{0, 0, 0}, start, end) &&
	          !SniperProbe(Vec3{0, 0, 0}, Vec3{nan, 0, 1}, start, end),
	      "and a line that is not a direction is not played at all");
}

} // namespace

int RunDriveByTests() {
	g_driveByFailures = 0;
	TestTheTwoSides();
	TestHeldTheWayTheEngineTestsIt();
	TestWhatGoesInAnimId2();
	TestWhichWindow();
	TestWhichSideToHold();
	TestWhoseHitItIs();
	TestTheLineOnTheWire();
	TestTheTrailAnObserverDraws();
	TestTheSniperRoundIsHeard();
	return g_driveByFailures;
}
