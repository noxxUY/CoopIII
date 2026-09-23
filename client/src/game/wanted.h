// The wanted level: who has stars, and how they spread.
//
// docs/wanted.md is the investigation and the design. What a reader of this
// file needs is four sentences.
//
// 1. **There is one CWanted and it is the local player's.** It hangs off
//    CPlayerPed at +0x53C, which is the end of CPed, and every remote player
//    and every replica in CoopIII is a CCivilianPed - exactly SIZEOF_PED
//    bytes, with nowhere to put a second one. So a player's stars are their
//    own machine's to decide, and an observer is only ever told the number.
//
// 2. **The police need nothing at all.** A cop ped is created by
//    CPopulation::AddPed as a RANDOM_CHAR and a police car by
//    CCarCtrl::GenerateOneRandomCar as a RANDOM_VEHICLE, so both already pass
//    game/population.cpp's host tests unchanged. The wanted player's own
//    engine makes them, CCopPed can only chase FindPlayerPed() so they chase
//    that player, and every other machine receives them as ordinary ambient
//    replicas that decide nothing. Not one byte of this feature is a police
//    packet.
//
// 3. **Crimes are already attributed correctly.** CEventList::RegisterEvent
//    ends with `if (criminal == FindPlayerPed())` before it reports anything,
//    so M3 replaying somebody else's shot here cannot move this machine's
//    stars. addresses.h carries the four instructions.
//
// 4. **All CoopIII does is write one number into that CWanted, sometimes.**
//    PlanWanted below is the whole decision, as arithmetic, deliberately with
//    no engine dependency so tools/clienttest can put the awkward cases
//    through it without a running game.
#pragma once

#include "client.h"

#include <coopiii/protocol.h>

#include <cstddef>
#include <cstdint>

namespace coopiii::game {

// Adds this file's two entries to the bridge ped.cpp built. Kept separate
// because it touches a different class and its absence is survivable: a build
// with these left null syncs everything else and leaves the wanted level
// per-machine, which is where this project was before today.
void InstallWantedBridge(WorldBridge &b);

// ---- the decision, as arithmetic ------------------------------------------

// Does this other player's level count toward our floor?
//
// The two rules ask different questions and the difference is the whole of
// why PF_WANTED_BORROWED exists:
//
//   perplayer - are they in the same car as us? Nothing else about them
//               matters, borrowed or not, driver or passenger. A player who
//               borrowed their stars from somebody who has since left is
//               still wanted, their engine is still making police, and a
//               newcomer getting into that car should still get the heat.
//
//   shared    - did they earn it? A borrowed level is an echo of somebody
//               else's, and counting echoes is what deadlocks the mode
//               (docs/wanted.md §4.5).
//
//   off       - nobody raises anybody.
//
// `ourCarNetId` and `theirCarNetId` are INVALID_NETID for a player on foot,
// and two players on foot are not in the same car - which is why the valid
// test is here rather than left to the equality.
inline bool WantedPeerRaisesFloor(uint8_t rule, uint16_t ourCarNetId,
                                  uint16_t theirCarNetId, bool theirsBorrowed) {
	switch (rule) {
	case WANTED_RULE_PERPLAYER:
		return ourCarNetId != INVALID_NETID && ourCarNetId == theirCarNetId;
	case WANTED_RULE_SHARED:
		return !theirsBorrowed;
	default:
		return false;
	}
}

// What CoopIII should do with the local player's stars this tick.
//
//   engine   what the engine's m_nWantedLevel reads right now
//   applied  what CoopIII last left it at (or last observed, when it wrote
//            nothing - the two are the same thing, see below)
//   own      the level this player reached without help
//   floor    the maximum over peers that WantedPeerRaisesFloor accepted
//
// Two things in here are load-bearing and neither is obvious.
//
// **`own` is only re-read from the engine when the engine has moved on its
// own.** That one comparison is the whole of the earned-versus-granted
// bookkeeping, and it needs no hook: if the level is exactly where CoopIII
// left it, CoopIII learned nothing this tick and must not overwrite what it
// knows. If it is anywhere else, the engine moved it - a crime, a death, a
// bust, a bribe pickup, a Pay'n'Spray - and that new number is this player's
// own by definition.
//
// **`write` is false when the target and the engine already agree**, and that
// is what makes the feature invisible to a player who is alone with their
// stars. CWanted::SetWantedLevel calls ClearQdCrimes and then resets m_nChaos
// to the *bottom* of the requested bracket - 820 for four stars. Writing it
// every tick with the level the player already has would throw their
// accumulated chaos away 25 times a second and stop them ever reaching the
// next star. In perplayer with nobody in your car, `write` is never true.
struct WantedPlan {
	uint8_t target   = 0;   // what the engine should hold after this tick
	bool    write    = false;
	uint8_t own      = 0;   // carry into the next tick
	uint8_t applied  = 0;   // carry into the next tick
	bool    borrowed = false;   // the bit to put on the wire beside `target`
};

inline WantedPlan PlanWanted(uint8_t rule, uint8_t engine, uint8_t applied,
                             uint8_t own, uint8_t floor) {
	if (engine > WANTED_LEVEL_CEILING)
		engine = WANTED_LEVEL_CEILING;
	if (floor > WANTED_LEVEL_CEILING)
		floor = WANTED_LEVEL_CEILING;

	// The engine moved by itself since we last looked.
	if (engine != applied)
		own = engine;
	if (own > WANTED_LEVEL_CEILING)
		own = WANTED_LEVEL_CEILING;

	WantedPlan plan;
	plan.target = (rule == WANTED_RULE_OFF) ? uint8_t(0)
	                                        : (own > floor ? own : floor);
	plan.write  = plan.target != engine;

	// A borrowed star becomes yours, in perplayer and only there.
	//
	// Getting out of the car does not take it away: the level is ended by the
	// four things GTA III ends a level with - death, arrest, a bribe pickup,
	// a Pay'n'Spray - and by nothing else. A level that evaporated when you
	// opened the door would put you on the pavement in front of four police
	// cars with no stars, and the engine would have those cars break off
	// mid-chase. docs/wanted.md §4.4 argues it; §1 is why it is a choice
	// rather than a copy, because the sources do not say what GTA Online does
	// here.
	//
	// shared must NOT adopt. Adopting is precisely the deadlock.
	//
	// off does not adopt either, and it does not keep the old value: with the
	// wanted level switched off nobody has any stars, including the ones they
	// had a moment ago, and leaving `own` at what the engine briefly reached
	// would mean reporting a level to the rest of the session that this
	// machine has already taken away again.
	plan.own = (rule == WANTED_RULE_OFF)         ? uint8_t(0)
	           : (rule == WANTED_RULE_PERPLAYER) ? plan.target
	                                             : own;
	plan.applied  = plan.target;
	plan.borrowed = plan.target > plan.own;
	return plan;
}

} // namespace coopiii::game
