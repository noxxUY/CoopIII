#include "money.h"

#include "leadcheck.h"
#include "ped.h"
#include "../client.h"
#include "../hook/hook.h"
#include "../log.h"

#include <cstring>
#include <intrin.h>

#include <windows.h>

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

// The bomb timer's two calls (addresses.h, BombTimer_*), taken together or not
// at all.
bool g_gateTaken    = false;
bool g_saidBomber   = false;

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

// ---- the bomb timer's pay gate (addresses.h, BombTimer_*) ------------------

// Another player's ped the car's bomb blames, when the gate is to ask about
// him (BombGateAsksBomber); null for the engine's own question.
void *OtherBomberOf(void *car) {
	if (!car)
		return nullptr;
	void *const bomber = Field<void *>(car, offs::VEH_BLOW_UP_ENTITY);
	uint16_t    netId  = INVALID_NETID;
	const bool  other  = bomber != nullptr && bomber != PlayerPed() &&
	                   RemotePlayerForPed(bomber, netId);
	return BombGateAsksBomber(g_inSession, g_rule, g_award.IsInstalled(), other) ? bomber
	                                                                               : nullptr;
}

// In place of FindPlayerVehicle() at 0x00551D42: the car the bomber sits in,
// read off his ped the way FindPlayerVehicle reads ours.
void *__fastcall GateVehicle(void *car) {
	void *const bomber = OtherBomberOf(car);
	if (!bomber)
		return PlayerVehicle();
	return Field<uint8_t>(bomber, offs::PED_IN_VEHICLE) != 0
	           ? Field<void *>(bomber, offs::PED_MY_VEHICLE)
	           : nullptr;
}

// In place of FindPlayerPed() at 0x00551D4B: the bomber.
void *__fastcall GatePed(void *car) {
	void *const bomber = OtherBomberOf(car);
	if (!bomber)
		return PlayerPed();
	if (!g_saidBomber) {
		g_saidBomber = true;
		Log("money: another player's bomb went off here; the bomb timer pays for model %d "
		    "as his own machine would, and it goes to him",
		    static_cast<int>(Field<int16_t>(car, offs::MODEL_INDEX)));
	}
	return bomber;
}

// The call sites' ebp is the car (addresses.h): handed on in ecx, and the
// return goes straight back to the gate's compare.
__declspec(naked) void GateVehicleFromCar() {
	__asm {
		mov ecx, ebp
		jmp GateVehicle
	}
}

__declspec(naked) void GatePedFromCar() {
	__asm {
		mov ecx, ebp
		jmp GatePed
	}
}

bool Redirect(uintptr_t site, uintptr_t from, uintptr_t to) {
	if (!RelCallAt(Ptr<uint8_t>(site), site, from))
		return false;
	DWORD old = 0;
	if (!VirtualProtect(reinterpret_cast<void *>(site), 5, PAGE_EXECUTE_READWRITE, &old))
		return false;
	const int32_t rel = static_cast<int32_t>(to - (site + 5));
	std::memcpy(reinterpret_cast<void *>(site + 1), &rel, sizeof rel);
	VirtualProtect(reinterpret_cast<void *>(site), 5, old, &old);
	FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void *>(site), 5);
	return true;
}

void TakeBombGate() {
	const uintptr_t vehicle = reinterpret_cast<uintptr_t>(&GateVehicleFromCar);
	const uintptr_t ped     = reinterpret_cast<uintptr_t>(&GatePedFromCar);
	if (!RelCallAt(Ptr<uint8_t>(BombTimer_FindPlayerPedCall), BombTimer_FindPlayerPedCall,
	               FindPlayerPed) ||
	    !Redirect(BombTimer_FindPlayerVehicleCall, FindPlayerVehicle, vehicle)) {
		Log("money: FAILED to take the bomb timer's pay gate at 0x%08X; another player's bomb "
		    "that goes off here pays nobody",
		    static_cast<unsigned>(BombTimer_FindPlayerVehicleCall));
		return;
	}
	if (!Redirect(BombTimer_FindPlayerPedCall, FindPlayerPed, ped)) {
		Redirect(BombTimer_FindPlayerVehicleCall, vehicle, FindPlayerVehicle);
		Log("money: FAILED to take the bomb timer's pay gate at 0x%08X; another player's bomb "
		    "that goes off here pays nobody",
		    static_cast<unsigned>(BombTimer_FindPlayerPedCall));
		return;
	}
	g_gateTaken = true;
	Log("money: the bomb timer's pay gate at 0x%08X asks about the bomber, whoever's he is",
	    static_cast<unsigned>(BombTimer_FindPlayerVehicleCall));
}

void GiveBombGateBack() {
	if (!g_gateTaken)
		return;
	Redirect(BombTimer_FindPlayerPedCall, reinterpret_cast<uintptr_t>(&GatePedFromCar),
	         FindPlayerPed);
	Redirect(BombTimer_FindPlayerVehicleCall, reinterpret_cast<uintptr_t>(&GateVehicleFromCar),
	         FindPlayerVehicle);
	g_gateTaken = false;
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
	TakeBombGate();
	return true;
}

void RemoveMoneyHook() {
	GiveBombGateBack();
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
