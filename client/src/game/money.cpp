#include "money.h"

#include "ped.h"
#include "../client.h"
#include "../hook/hook.h"
#include "../log.h"

#include <intrin.h>

namespace coopiii::game {

namespace {

Detour g_award;

// What Client last told us. Written from the game thread on a welcome, an
// S_Money and a disconnect; read from inside the engine on the same thread.
bool    g_inSession = false;
uint8_t g_rule      = MONEY_RULE_OFF;

// Awards waiting for Client. A burst of wrecks in one frame is a handful; the
// bound only keeps a runaway from growing it.
constexpr uint8_t MAX_AWARDS = 16;
LocalMoneyAward   g_awards[MAX_AWARDS];
uint8_t           g_awardCount = 0;

bool g_saidDropped   = false;
bool g_saidForwarded = false;
bool g_saidFull      = false;

using AwardFn = void(__thiscall *)(void *, void *);
using FindFn  = void *(__cdecl *)();

void *PlayerPed() { return Func<FindFn>(FindPlayerPed)(); }
void *PlayerVehicle() { return Func<FindFn>(FindPlayerVehicle)(); }

uintptr_t FocusPlayerInfo() {
	return CWorld__Players + Global<uint8_t>(CWorld__PlayerInFocus) * offs::PLAYERINFO_STRIDE;
}

// Who the engine names, and which player that is. Pointer compares only: the
// culprit fields are plain pointers that nothing clears when the entity goes.
AwardCulprit CulpritOf(void *culprit, LocalMoneyAward &out) {
	if (!culprit)
		return AwardCulprit::Nobody;
	if (culprit == PlayerPed() || culprit == PlayerVehicle())
		return AwardCulprit::Us;
	uint16_t netId = INVALID_NETID;
	if (RemotePlayerForPed(culprit, netId)) {
		out.toNetId = netId;
		return AwardCulprit::Them;
	}
	const uint8_t driver = RemoteDriverOf(culprit);
	if (driver != INVALID_PLAYER) {
		out.toPlayerId = driver;
		return AwardCulprit::Them;
	}
	return AwardCulprit::Nobody;
}

void Queue(const LocalMoneyAward &a) {
	if (g_awardCount == MAX_AWARDS) {
		if (!g_saidFull) {
			g_saidFull = true;
			Log("money: %u awards are already waiting; a wreck is not paid for",
			    static_cast<unsigned>(MAX_AWARDS));
		}
		return;
	}
	g_awards[g_awardCount++] = a;
}

// __thiscall void CPlayerInfo::AwardMoneyForExplosion(CVehicle *wreck), the
// usual __fastcall stand-in. Which caller it is comes off the return address.
void __fastcall HookedAward(void *self, void * /*edx*/, void *wreck) {
	const uintptr_t from = reinterpret_cast<uintptr_t>(_ReturnAddress());
	const bool      fire = from == AWARD_RETURN_FIRE_TIMER;
	const bool      bomb = from == AWARD_RETURN_BOMB_TIMER;
	if (!g_inSession || g_rule == MONEY_RULE_OFF || !wreck || (!fire && !bomb)) {
		g_award.Original<AwardFn>()(self, wreck);
		return;
	}

	LocalMoneyAward a;
	a.kind  = fire ? MONEY_AWARD_FIRE : MONEY_AWARD_BOMB;
	a.model = static_cast<uint16_t>(Field<int16_t>(wreck, offs::MODEL_INDEX));
	void *const handling = Field<void *>(wreck, offs::VEH_HANDLING);
	a.unit = handling ? ExplosionAwardUnit(
	                        Field<uint32_t>(handling, offs::HANDLING_MONETARY_VALUE))
	                  : 0;

	const AwardCulprit culprit = CulpritOf(
	    Field<void *>(wreck, fire ? offs::AUTO_SET_ON_FIRE_ENTITY : offs::VEH_BLOW_UP_ENTITY),
	    a);
	const WreckDecider decider = WhoDecidesWreckHere(wreck, a.key);
	const bool         keyed   = MoneyAwardKeyed(a.key);

	switch (RouteExplosionAward(decider, keyed, culprit)) {
	case AwardRoute::PayHere:
		g_award.Original<AwardFn>()(self, wreck);
		return;

	case AwardRoute::Forward:
		if (!IsSaneMoneyAward(a.unit))
			return;   // a car worth nothing; the engine would have added 0
		a.toUs = culprit == AwardCulprit::Us;
		Queue(a);
		if (!g_saidForwarded) {
			g_saidForwarded = true;
			Log("money: the %s timer paid for model %u here and it was not ours "
			    "to keep; $%d a car goes to %s",
			    fire ? "fire" : "bomb", a.model, a.unit,
			    a.toUs ? "us through the server, which pays a car once"
			           : "the player who did it");
		}
		return;

	case AwardRoute::Drop:
		if (!g_saidDropped) {
			g_saidDropped = true;
			Log("money: the %s timer paid for model %u here and nobody on this "
			    "machine earned it; not paid (%s)",
			    fire ? "fire" : "bomb", a.model,
			    decider == WreckDecider::Elsewhere
			        ? "another machine decides that car"
			        : "the machine of whoever did it pays them");
		}
		return;
	}
}

// ---- MoneyBridge ------------------------------------------------------------

void SetMoneySession(bool inSession, uint8_t rule) {
	g_inSession = inSession;
	g_rule      = SaneMoneyRule(rule);
	if (!inSession)
		g_awardCount = 0;
}

bool ReadMoney(int32_t &money, int32_t &life) {
	void *const ped = PlayerPed();
	if (!ped)
		return false;
	life  = Func<int32_t(__cdecl *)(void *)>(CPools__GetPedRef)(ped);
	money = Global<int32_t>(FocusPlayerInfo() + offs::PLAYERINFO_MONEY);
	return true;
}

// m_nMoney alone. CPlayerInfo::Process walks m_nVisibleMoney to it a step a
// frame, so the HUD rolls to a teammate's change the way it rolls to ours.
void WriteMoney(int32_t money) {
	if (!PlayerPed())
		return;
	Global<int32_t>(FocusPlayerInfo() + offs::PLAYERINFO_MONEY) = money;
}

uint8_t DrainMoneyAwards(LocalMoneyAward *out, uint8_t max) {
	const uint8_t n = g_awardCount < max ? g_awardCount : max;
	for (uint8_t i = 0; i < n; ++i)
		out[i] = g_awards[i];
	for (uint8_t i = n; i < g_awardCount; ++i)
		g_awards[i - n] = g_awards[i];
	g_awardCount = static_cast<uint8_t>(g_awardCount - n);
	return n;
}

// Somebody's machine decided a car our player wrecked. What the engine's own
// function would have done on our CPlayerInfo, minus the gString it prints
// into and the rand() it throws away: the chain, then the money.
void PayExplosionAward(int32_t unit) {
	if (!PlayerPed() || !IsSaneMoneyAward(unit))
		return;
	const uintptr_t info = FocusPlayerInfo();
	ExplosionChain  chain;
	chain.lastMs = Global<uint32_t>(info + offs::PLAYERINFO_LAST_EXPLOSION_MS);
	chain.count  = Global<int32_t>(info + offs::PLAYERINFO_EXPLOSION_CHAIN);
	const int32_t paid =
	    ChainExplosionAward(chain, Global<uint32_t>(CTimer__m_snTimeInMilliseconds), unit);
	Global<uint32_t>(info + offs::PLAYERINFO_LAST_EXPLOSION_MS) = chain.lastMs;
	Global<int32_t>(info + offs::PLAYERINFO_EXPLOSION_CHAIN)    = chain.count;
	Global<int32_t>(info + offs::PLAYERINFO_MONEY) += paid;
}

} // namespace

bool InstallMoneyHook() {
	g_inSession  = false;
	g_rule       = MONEY_RULE_OFF;
	g_awardCount = 0;
	if (!g_award.Install("CPlayerInfo::AwardMoneyForExplosion",
	                     reinterpret_cast<void *>(CPlayerInfo__AwardMoneyForExplosion),
	                     reinterpret_cast<void *>(&HookedAward))) {
		Log("money: FAILED to hook CPlayerInfo::AwardMoneyForExplosion at 0x%08X; "
		    "a wreck pays whoever's game watched it, whatever the server's money "
		    "rule", CPlayerInfo__AwardMoneyForExplosion);
		for (const auto &f : HookFailures())
			Log("money:   %s: %s", f.name.c_str(), f.reason.c_str());
		return false;
	}
	Log("money: hooked CPlayerInfo::AwardMoneyForExplosion at 0x%08X",
	    CPlayerInfo__AwardMoneyForExplosion);
	return true;
}

void RemoveMoneyHook() {
	g_award.Remove();
	g_inSession  = false;
	g_rule       = MONEY_RULE_OFF;
	g_awardCount = 0;
}

void AddMoneyToBridge(WorldBridge &bridge) {
	MoneyBridge &b = bridge.money;
	b.SetMoneySession = &SetMoneySession;
	b.ReadMoney       = &ReadMoney;
	b.WriteMoney      = &WriteMoney;
	if (g_award.IsInstalled()) {
		b.DrainMoneyAwards  = &DrainMoneyAwards;
		b.PayExplosionAward = &PayExplosionAward;
	}
}

} // namespace coopiii::game
