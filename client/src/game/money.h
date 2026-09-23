// Money, engine side. moneysync.h is the session side and protocol.h's
// MoneyRule is the design; addresses.h, "AwardMoneyForExplosion" and
// "CPlayerInfo", has every address this uses and how each was read.
//
// One detour, on CPlayerInfo::AwardMoneyForExplosion (0x004A15F0). The engine
// pays for a car in exactly two places and they disagree about who earned it:
//
//   the fire timer    CAutomobile::ProcessControl, 0x0053479B. Pays with no
//                     test at all, so whoever's engine watches a car burn out
//                     is paid, whoever lit it - and a parked car pays every
//                     machine that saw it.
//   the bomb timer    CVehicle::ProcessDelayedExplosion, 0x00551D6C. Pays only
//                     the local player, and only when he's the culprit.
//
// With the rule off every call goes straight through, which is all this file
// does then. Otherwise the call is answered here, in the order the rules
// below spell out: a machine that doesn't decide this wreck pays nobody, and
// one that does pays the culprit - its own player here, anybody else through
// their own machine.
//
// The rules are pure functions so tools/clienttest checks them without a game.
#pragma once

#include "addresses.h"
#include "vehicle.h"

#include <cstdint>

namespace coopiii {
struct WorldBridge;
}

namespace coopiii::game {

// Who the engine names for this wreck: m_pSetOnFireEntity for the fire timer,
// m_pBlowUpEntity for the bomb. A remote player's ped or a car they drive is
// Them; an NPC, nothing, or anything else is Nobody.
enum class AwardCulprit : uint8_t {
	Nobody,
	Us,
	Them,
};

enum class AwardRoute : uint8_t {
	PayHere,   // let the engine pay, the way single player does
	Forward,   // C_MoneyAward to whoever earned it, possibly us
	Drop,      // pay nobody from this machine
};

// A car only this machine decides is the single-player case, and keeps its
// single-player answer for anybody but another player. A car every machine
// that has it decides can only be paid for once, so it goes through the
// server under its name when it has one - to the culprit, or to nobody when
// there isn't one, since "whoever watched" is exactly the payment that would
// be made once per machine. Without a name nobody can dedupe it, so each
// machine pays only its own player, and only when he did it: his own copy of
// the car is the one that pays anybody else.
inline AwardRoute RouteExplosionAward(WreckDecider decider, bool keyed,
                                      AwardCulprit culprit) {
	switch (decider) {
	case WreckDecider::Elsewhere:
		return AwardRoute::Drop;
	case WreckDecider::OnlyHere:
		return culprit == AwardCulprit::Them ? AwardRoute::Forward : AwardRoute::PayHere;
	case WreckDecider::Everywhere:
		if (keyed)
			return culprit == AwardCulprit::Nobody ? AwardRoute::Drop : AwardRoute::Forward;
		return culprit == AwardCulprit::Us ? AwardRoute::PayHere : AwardRoute::Drop;
	}
	return AwardRoute::Drop;
}

// One car's worth: nMonetaryValue * 0.002f, truncated (0x004A1642..0x004A1661).
inline int32_t ExplosionAwardUnit(uint32_t monetaryValue) {
	return static_cast<int32_t>(static_cast<double>(monetaryValue) *
	                            static_cast<double>(EXPLOSION_REWARD_FACTOR));
}

// The chain AwardMoneyForExplosion keeps on the paying CPlayerInfo (+0x104,
// +0x108): another award inside six seconds of the last is one more link,
// anything later starts again at one. Returns what the engine would add to
// m_nMoney: the unit once, then once more for every link past the first.
struct ExplosionChain {
	uint32_t lastMs = 0;
	int32_t  count  = 0;
};

inline int32_t ChainExplosionAward(ExplosionChain &chain, uint32_t nowMs, int32_t unit) {
	if (nowMs - chain.lastMs < EXPLOSION_CHAIN_MS)
		++chain.count;
	else
		chain.count = 1;
	chain.lastMs = nowMs;
	return chain.count > 1 ? unit * chain.count : unit;
}

// Installs the detour. False if it failed, in which case money is each
// machine's own whatever the server says, and the log says so.
bool InstallMoneyHook();
void RemoveMoneyHook();

// Wires MoneyBridge. The reads and writes are always offered; the awards only
// with the detour in.
void AddMoneyToBridge(WorldBridge &bridge);

} // namespace coopiii::game
