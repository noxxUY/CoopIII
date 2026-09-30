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
//    replicas that decide nothing. The one police packet is for the case
//    that leaves out: a cop hosted by somebody else next to our wanted
//    player, handed to our machine (ShouldHandCopOver, at the bottom).
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

#include <cmath>
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
// knows. If it is higher, the engine moved it with a crime and the new number
// is this player's own. If it is lower - a death, a bust, a bribe pickup, a
// Pay'n'Spray - `own` can only come down with it, never up to it
// (docs/wanted.md §4.9).
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

// `cap` is the session's maxWanted (S_SessionRules): nobody holds more,
// earned, borrowed or reported, and the engine is brought down to it the way
// `off` brings it down to 0.
inline WantedPlan PlanWanted(uint8_t rule, uint8_t engine, uint8_t applied,
                             uint8_t own, uint8_t floor,
                             uint8_t cap = WANTED_LEVEL_CEILING) {
	if (cap == 0 || cap > WANTED_LEVEL_CEILING)
		cap = WANTED_LEVEL_CEILING;
	if (engine > WANTED_LEVEL_CEILING)
		engine = WANTED_LEVEL_CEILING;
	if (floor > cap)
		floor = cap;

	// The engine moved by itself since we last looked.
	//
	// Up is a crime, and the new level is this player's own. Down is a death,
	// a bust, a bribe, a spray, a script or the one-star decay, and it can
	// only ever take away: a borrowed level that the engine brought down a
	// star is still mostly borrowed, and reading the whole of what is left as
	// earned would turn one bribe into a level that outlives the player who
	// actually earned it.
	if (engine > applied)
		own = engine;
	else if (engine < applied && engine < own)
		own = engine;
	if (own > cap)
		own = cap;

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

// ---- coming down (docs/wanted.md §4.9) ------------------------------------
//
// The rule above only ever looks at the last snapshot each player sent, and a
// snapshot is 40 ms to a round trip old. So the moment our stars come down -
// a Pay'n'Spray, a bribe, a mission's CLEAR_WANTED_LEVEL, a death - every
// other player is still reporting the level they had a moment ago, and the
// floor puts ours straight back. That is the respray bug: the spray shop
// cleared the stars and the next tick raised them again off a car-mate, or in
// `shared` off whoever else held the session's level, before either had heard
// of the respray.
//
// The answer is a hold. Whoever was reporting more than we now have counts
// for no more than what we now have, until they report less than they did -
// they came down too - or until WANTED_HOLD_MS has passed, after which they
// are simply still wanted and count again. The second half is what keeps a
// hold from ever being a way out: a car-mate who did not come down raises us
// again three seconds later, which is the vehicle rule, and in `shared` a
// player who earned stars of their own still gives them to the session.
//
// A cap rather than a plain "does not count", because a bribe takes one star
// and not all of them: after one, the session's four is a three everywhere,
// and a player still reporting four for a few more ticks has to go on
// counting as that three.

constexpr uint32_t WANTED_HOLD_MS = 3000;

// Our level has just come down to `cap`; start or restart the hold on a
// player whose last report was `reported`. Nothing to hold when they already
// have no more than we do.
inline void HoldWantedPeer(WantedHold &hold, uint8_t reported, uint8_t cap,
                           uint32_t nowMs) {
	if (reported <= cap) {
		hold = WantedHold{};
		return;
	}
	hold.above   = reported;
	hold.cap     = cap;
	hold.sinceMs = nowMs;
}

// What this player's report counts for right now, ending the hold when its
// reason has gone: they reported less than when it began, or it has run out.
inline uint8_t CountedWantedLevel(WantedHold &hold, uint8_t reported,
                                  uint32_t nowMs) {
	if (hold.above == 0)
		return reported;
	if (reported < hold.above ||
	    static_cast<uint32_t>(nowMs - hold.sinceMs) >= WANTED_HOLD_MS) {
		hold = WantedHold{};
		return reported;
	}
	return reported < hold.cap ? reported : hold.cap;
}

// Does another player's Pay'n'Spray or bribe reach us?
//
//   shared    - yes, wherever they are. The session holds one level, so what
//               takes it away takes it away for everybody.
//   perplayer - only if we are in the car they were in. The vehicle rule says
//               a car's occupants share the highest level among them, so a
//               respray that cleared only the driver would be undone by the
//               passenger on the next tick - the reported bug, in this rule.
//               Anyone else's stars were never theirs to take.
//   off       - there is nothing to take.
//
// INVALID_NETID for either car means on foot, or a car the session never
// heard of, and matches nothing.
inline bool WantedEventReaches(uint8_t rule, uint16_t ourCarNetId,
                               uint16_t theirCarNetId) {
	switch (rule) {
	case WANTED_RULE_SHARED:
		return true;
	case WANTED_RULE_PERPLAYER:
		return ourCarNetId != INVALID_NETID && ourCarNetId == theirCarNetId;
	default:
		return false;
	}
}

// Where our engine should be after the events that reached us: zero for a
// respray, one star fewer per bribe, never below zero.
inline uint8_t WantedAfterEvents(uint8_t engine, bool clear, uint8_t starsOff) {
	if (clear)
		return 0;
	return engine > starsOff ? static_cast<uint8_t>(engine - starsOff) : uint8_t(0);
}

// ---- police beside somebody else's stars (protocol.h, C_CopHandover) --------
//
// Sentence 2 at the top has a hole the size of a street: a cop chases
// FindPlayerPed() on the machine that hosts him, so one of ours standing next
// to a wanted player whose stars live on another machine does nothing about
// him. So the host gives that cop, or that police car with its cops, to the
// wanted player's machine, which builds a real CCopPed for it and whose
// engine then sets it on him.
//
// Handed over when a wanted player is within COP_HANDOVER_NEAR_M of it and:
// our own player is not wanted, or is further from it than that player by more
// than COP_HANDOVER_MARGIN_M. The margin and COP_HANDOVER_HOLD_MS - how long a
// cop handed to us is ours before we may hand it on - are what stop a cop
// between two wanted players changing hands every tick: coming back needs the
// two to swap places by twice the margin, and not before the hold is up.
constexpr float    COP_HANDOVER_NEAR_M   = 30.0f;
constexpr float    COP_HANDOVER_MARGIN_M = 25.0f;
constexpr uint32_t COP_HANDOVER_HOLD_MS  = 10000;

// The nearest of `viewers` with any stars to (x, y), flat, or INVALID_PLAYER.
// `d` is its distance. A player with no stars is nobody a cop wants.
inline uint8_t NearestWantedViewer(float x, float y, const WantedViewer *viewers,
                                   uint32_t count, float &d) {
	uint8_t best   = INVALID_PLAYER;
	float   bestD2 = 0.0f;
	for (uint32_t i = 0; i < count; ++i) {
		if (viewers[i].level == 0 || viewers[i].playerId == INVALID_PLAYER)
			continue;
		const float dx = viewers[i].pos.x - x, dy = viewers[i].pos.y - y;
		const float d2 = dx * dx + dy * dy;
		if (!(d2 == d2))
			continue;
		if (best == INVALID_PLAYER || d2 < bestD2) {
			best   = viewers[i].playerId;
			bestD2 = d2;
		}
	}
	d = best == INVALID_PLAYER ? 0.0f : std::sqrt(bestD2);
	return best;
}

// The decision for one cop, or one police car, of ours. `localD` is the flat
// distance to our own player, negative when we have none; `heldMs` how long
// it has been ours, from the handover that gave it to us (a cop our own
// engine made has always been ours: pass anything at or over the hold).
inline bool ShouldHandCopOver(bool localWanted, float localD, uint8_t target, float targetD,
                              uint32_t heldMs) {
	if (target == INVALID_PLAYER || heldMs < COP_HANDOVER_HOLD_MS)
		return false;
	if (!(targetD <= COP_HANDOVER_NEAR_M))
		return false;
	if (!localWanted || localD < 0.0f)
		return true;
	return targetD + COP_HANDOVER_MARGIN_M < localD;
}

// ---- which models are police -----------------------------------------------
//
// Read out of the retail image, not re3 (addresses in game/crowdaddr.h):
// CCopPed::CCopPed's jump table at 0x005F8268 gives the model for each
// eCopType, and CPopulation::AddPedInCar's at 0x005FA9A0 the eCopType for each
// police vehicle.
constexpr int COP_TYPE_NONE = -1;

// The eCopType a police ped model was built as, or COP_TYPE_NONE.
inline int CopTypeForPedModel(uint16_t model) {
	switch (model) {
	case 1: return 0;   // MI_COP   -> COP_STREET
	case 3: return 1;   // MI_FBI   -> COP_FBI
	case 2: return 2;   // MI_SWAT  -> COP_SWAT
	case 4: return 3;   // MI_ARMY  -> COP_ARMY
	default: return COP_TYPE_NONE;
	}
}

// The ped model CCopPed::CCopPed gives an eCopType.
inline uint16_t PedModelForCopType(int copType) {
	switch (copType) {
	case 0: return 1;
	case 1: return 3;
	case 2: return 2;
	case 3: return 4;
	default: return 0;
	}
}

// The eCopType CPopulation::AddPedInCar puts in a vehicle, or COP_TYPE_NONE
// for anything it fills with somebody else. The police helicopter is not one
// of them and is left alone on purpose.
inline int CopTypeForCarModel(uint16_t model) {
	switch (model) {
	case 116: return 0;   // MI_POLICE
	case 107: return 1;   // MI_FBICAR
	case 117: return 2;   // MI_ENFORCER
	case 122:             // MI_RHINO
	case 123: return 3;   // MI_BARRACKS
	default: return COP_TYPE_NONE;
	}
}

} // namespace coopiii::game
