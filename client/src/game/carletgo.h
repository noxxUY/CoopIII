// A traffic car of ours that our engine drops next to somebody else.
//
// CCarCtrl::PossiblyRemoveVehicle measures every traffic car against this
// machine's player and camera and nobody else's:
//
//   - stopped in traffic (STATUS_SIMPLE, or PHYSICS with the stop-for-cars
//     driving style), off our screen, more than 25 m from our player, five
//     seconds after its mission began - gone;
//   - off our screen and past 50 m, or on it and past 130 m (times 1.5 for
//     bExtendedRange) - gone, or faded out first when it is in view;
//   - a wreck a minute old, off our screen and past 7.5 m - gone.
//
// A car belongs to the machine that made it (docs/population.md §1.3), so the
// car one player is looking at is quite often one the other player's engine
// is measuring against the other player. When that engine dropped it, its
// host said C_CarDespawn and the car vanished from in front of the player who
// could see it. The 25 m rule alone does it to almost every car that falls
// behind the host while its guest is watching.
//
// So the two calls into PossiblyRemoveVehicle are taken, and a car of ours
// that it takes out of the world while another player is within
// AMBIENT_CAR_KEEP_RADIUS_M of it goes out as C_CarLetGo: the session hands it
// to the nearest such player, whose own engine then keeps or drops it by his
// own player and his own camera (protocol.h, C_CarLetGo). Every other way a
// car of ours leaves the world is a despawn, as before.
//
// Pure: what the engine half (game/population.cpp) decides with, and every
// address it relies on, which tools/clienttest/carletgo.cpp reads back out of
// the exe when it has one.
#pragma once

#include "addresses.h"

#include <coopiii/protocol.h>

#include <cstddef>
#include <cstdint>

namespace coopiii::game {

// The engine half, game/carletgo.cpp. Takes PossiblyRemoveVehicle's two calls
// so that CarBeingReaped names the car it is deciding about for the length of
// the call. Not fatal: without it every car our engine drops is a despawn, as
// it always was. Remove puts both calls back.
bool InstallCarLetGo();
void RemoveCarLetGo();
// The car CCarCtrl::PossiblyRemoveVehicle is deciding about right now, or null.
void *CarBeingReaped();

// ---- the reaper -------------------------------------------------------------
//
// __cdecl void CCarCtrl::PossiblyRemoveVehicle(CVehicle *), a plain `ret` at
// 0x00418618 and each of its other exits; its callers pop the argument. It
// opens with IsThisVehicleInteresting (0x0041F780), bIsLocked (`[ebx+1F5h]`
// bit 3), CanBeDeleted (0x005511B0) and the crane test (0x005451E0), then:
//
//   00418477  [ebx+1F7h] bit 3 / GetClumpAlpha (00528F70) == 0
//   00418493  call CWorld::Remove                 faded out, and gone
//   004184B0  push [0095CD61] / call 004A1170     FindPlayerCentreOfWorld(focus)
//   004184F2  fsqrt                               the 2D distance
//   00418504  call 00474CC0                       GetIsOnScreen
//   00418571  fld [005EC920] 130 / fmul [006FADE8] on screen, or looking about
//   00418580  fld [005EC92C] 50                   otherwise
//   0041859B  fmul [005EC970] 1.5                 bExtendedRange
//   004185D9  call 00474CC0 / call 004AAA00       in view: set bFadeOut, keep
//   00418601  call CWorld::Remove                 out of view: gone
//   00418620  status 2, or 3 with [ebx+159h] 0    SIMPLE, or stopping for cars
//   00418644  [00885B48] - [ebx+150h] > 1388h     5 s since its mission began
//   0041865C  call 00474CC0 / jne keep            off screen
//   0041869B  fcomp [005EC974] 25                 past 25 m
//   004186D4  call 00455350 (push 1) / 00456460   not held by a light or a bridge
//   0041870E  call CWorld::Remove                 gone
//   00418726  the wreck branch (addresses.h has it whole)
//   004187E7  call CWorld::Remove                 gone
constexpr uintptr_t CCarCtrl__PossiblyRemoveVehicle = 0x00418430;

// Its only two callers, both `push reg / call / pop ecx`:
//
//   00418374  push ebx / call 00418430 / ... / 00418380 pop ecx
//             CCarCtrl::RemoveDistantCars (0x00418320), over the whole
//             vehicle pool, once a frame from CGame::Process (0x0048CA03)
//   004F3680  push edx / fstp st(0) / 004F3683 call 00418430 / pop ecx
//             CPlayerPed::KeepAreaAroundPlayerClear (0x004F3460), over the
//             cars CWorld::FindObjectsInRange found around our player
constexpr uintptr_t POSSIBLY_REMOVE_VEHICLE_CALLS[2] = {0x00418375, 0x004F3683};

// The four CWorld::Remove calls in it, by where each returns to - which is the
// return address the CWorld::Remove detour is entered with.
constexpr uintptr_t REAP_RETURN_FADED   = 0x00418498;
constexpr uintptr_t REAP_RETURN_FAR     = 0x00418606;
constexpr uintptr_t REAP_RETURN_STOPPED = 0x00418713;
constexpr uintptr_t REAP_RETURN_WRECK   = 0x004187EC;

// The thresholds, as floats in .rdata.
constexpr uintptr_t REAP_OFFSCREEN_M_AT  = 0x005EC92C;   // 50.0f
constexpr uintptr_t REAP_ONSCREEN_M_AT   = 0x005EC920;   // 130.0f
constexpr uintptr_t REAP_EXTENDED_AT     = 0x005EC970;   // 1.5f
constexpr uintptr_t REAP_STOPPED_M_AT    = 0x005EC974;   // 25.0f
constexpr uintptr_t REAP_WRECK_M2_AT     = 0x005EC978;   // 56.25f, 7.5 m squared

enum class CarReap : uint8_t {
	Other,     // not from PossiblyRemoveVehicle: a mission, a crusher, the pool
	Faded,     // faded out in view past 130 m
	Far,       // off screen past 50 m (130 on it)
	Stopped,   // stopped in traffic, off screen, past 25 m
	Wreck,     // a wreck a minute old
};

inline CarReap ReapFromReturn(uintptr_t ret) {
	switch (ret) {
	case REAP_RETURN_FADED:   return CarReap::Faded;
	case REAP_RETURN_FAR:     return CarReap::Far;
	case REAP_RETURN_STOPPED: return CarReap::Stopped;
	case REAP_RETURN_WRECK:   return CarReap::Wreck;
	default:                  return CarReap::Other;
	}
}

inline const char *ReapName(CarReap r) {
	switch (r) {
	case CarReap::Faded:   return "faded out in our view past 130 m";
	case CarReap::Far:     return "past 50 m off our screen, or 130 m on it";
	case CarReap::Stopped: return "stopped in traffic off our screen past 25 m";
	case CarReap::Wreck:   return "a wreck a minute old, off our screen";
	case CarReap::Other:   break;
	}
	return "taken by something other than the distance reaper";
}

// Only the distance reaper's own drops are handed on. A wreck is not: nothing
// is left to run, and every machine's engine clears a shell a minute after it
// burns. Neither is anything else - a mission clearing an area, the crusher,
// a full pool - because those are reasons, not distances, and they hold on
// every screen.
inline bool ReapIsByDistance(CarReap r) {
	return r == CarReap::Faded || r == CarReap::Far || r == CarReap::Stopped;
}

// Is another player near enough for the session to try, given the squared 2D
// distance to the nearest (negative for nobody)? The session asks it again
// with its own positions and the players who let go of the car lately left
// out; this only saves a round trip for a car nobody could keep.
inline bool AnybodyToHandTo(float nearestD2) {
	return nearestD2 >= 0.0f &&
	       nearestD2 <= AMBIENT_CAR_KEEP_RADIUS_M * AMBIENT_CAR_KEEP_RADIUS_M;
}

// The decision, whole. `named`: the session knows it, so there is a netId to
// hand on. `mission`: the session's mission's own, which never leaves its
// owner this way.
inline bool ShouldLetGo(CarReap reap, bool named, bool mission, float nearestD2) {
	return named && !mission && ReapIsByDistance(reap) && AnybodyToHandTo(nearestD2);
}

// Squared 2D distance from (x, y) to the nearest of `count` points, or -1.
inline float NearestFlatD2(float x, float y, const Vec3 *pts, size_t count) {
	float best = -1.0f;
	for (size_t i = 0; i < count; ++i) {
		const float dx = pts[i].x - x, dy = pts[i].y - y;
		const float d2 = dx * dx + dy * dy;
		if (!(d2 == d2))
			continue;
		if (best < 0.0f || d2 < best)
			best = d2;
	}
	return best;
}

} // namespace coopiii::game
