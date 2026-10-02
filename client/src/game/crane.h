// The cranes on every machine at once (docs/protocol.md 1.62).
//
// CCranes runs on every machine, each crane over that machine's own copies of
// the cars. Two things went wrong with that in a session, both seen in Dead
// Skunk In The Trunk at the Portland crusher:
//
// - **The crane could not lift the car.** Only the machine holding a session
//   car may let its crane see it (game/carremoval.h), and that machine's crane
//   did go for it. But a car nobody drives and nobody settles is pinned by
//   every machine to the session's last transform after every frame's physics
//   (protocol.md 1.21), the holder's included. The hook reached the car,
//   CCrane::Update set it on the hook, and the correction put it back on the
//   ground the same frame, every frame.
// - **Nobody else saw it.** Every other machine's crane stood idle over a car
//   it was not allowed to see, so the hook never moved on their screens, and a
//   mission owner whose helper's crane lifted the car never heard
//   IS_CAR_PICKED_UP_BY_CRANE or IS_CAR_CRUSHED say yes.
//
// So one machine works a crane while it is busy: the one whose crane took a
// car, which is the car's holder. The moment its crane goes for a car it asks
// for the car's custody, with the shove's C_VehicleHit (protocol.md 1.21.5),
// and keeps it while the crane has the car and for the settle after the drop.
// Its engine then moves the car as in single player, and the custody stream
// carries the car to everybody else. It also sends its crane (C_CraneState):
// the hook's angle, reach, height and swing, the state, and which car. Every
// other machine's crane follows that instead of running its own Update - the
// hook moves on every screen, and CCrane::m_pVehiclePickedUp names that
// machine's own copy of the car, so IS_CAR_PICKED_UP_BY_CRANE asked by the
// mission owner's script says yes on the owner's machine as the car goes up.
// And the crushed car's S_VehicleRemoved writes CGarages::CrushedCarId on every
// other machine before the car goes, so the owner's IS_CAR_CRUSHED says yes
// whichever machine's crusher took it.
//
// The crane's Update is reached through its one call site in
// CCranes::UpdateCranes (addresses.h, CRANE_UPDATE_CALL), redirected, never
// detoured.
#pragma once

#include <cstdint>

#include "addresses.h"
#include "../client.h"
#include "coopiii/protocol.h"

namespace coopiii::game {

// The call site's new target, installed with the car removal sites
// (game/carremoval.cpp): follows another machine's crane, or runs ours and
// keeps it off cars it may no longer take.
void __fastcall CraneUpdateHere(void *crane, void *edx);

// Adds SampleCranes, ApplyCraneState, CraneHasVehicle and
// NoteCarCrushedElsewhere to a bridge.
void AddCranesToBridge(WorldBridge &bridge);

// Every crane back to the engine's own, as the hooks come off.
void ForgetCranes();

// ---- the parts that do not need a running game ----------------------------

// A crane with something to do: going for a car, carrying it, or dropping
// it. An idle crane with no car is every machine's own again.
inline bool CraneBusy(uint8_t state, bool hasCar) {
	return state != CRANE_IDLE || hasCar;
}

// The states in which the car hangs from the hook (IsThisCarBeingCarriedBy-
// AnyCrane's 2..4). Taking a car out of the crane in one of them leaves it
// with its collision off, which the crane only gives back at the drop.
inline bool CraneCarries(uint8_t state) {
	return state == CRANE_LIFTING_TARGET || state == CRANE_GOING_TOWARDS_TARGET_ONLY_HEIGHT ||
	       state == CRANE_ROTATING_TARGET;
}

// Another machine's crane has not been heard from for too long: its worker
// left or its end was lost, and ours is ours again.
inline bool CraneFollowLapsed(uint32_t heardAtMs, uint32_t nowMs) {
	return nowMs - heardAtMs >= CRANE_FOLLOW_TIMEOUT_MS;
}

// Whether a busy state from `from`, sent at `sentMs` on its clock, is older
// than something already heard from the same machine about that crane: its
// end (which travels on the reliable channel and can overtake the stream),
// or a newer state.
inline bool CraneStateIsOld(uint8_t from, uint32_t sentMs, bool haveEnd, uint8_t endFrom,
                            uint32_t endSentMs, bool following, uint8_t followFrom,
                            uint32_t followSentMs) {
	if (haveEnd && endFrom == from && static_cast<int32_t>(sentMs - endSentMs) <= 0)
		return true;
	return following && followFrom == from && static_cast<int32_t>(sentMs - followSentMs) < 0;
}

} // namespace coopiii::game
