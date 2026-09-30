#include "darkel.h"

#include "addresses.h"
#include "leadcheck.h"
#include "mission.h"
#include "missioncombat.h"
#include "population.h"
#include "vehicle.h"
#include "../clock.h"
#include "../hook/hook.h"
#include "../log.h"

#include <cstring>

#include <windows.h>

namespace coopiii::game {

namespace {

RampageCallbacks g_cb;
RampageStats     g_stats;

Detour g_startFrenzy;
Detour g_registerKill;
Detour g_readStatus;
Detour g_registerCar;

// Named cars this machine has counted in the current frenzy, whether its own
// engine counted them or the wire did. darkel.h, "one car, one count".
CountedCars g_countedCars;

// The session's rule, out of S_Welcome. Written on connect and read in the
// three detours, all on the game thread.
uint8_t g_rule = RAMPAGE_RULE_SHARED;

// Our own script has a frenzy running. Set by the StartFrenzy detour and
// cleared when the script is handed a verdict, and it is what keeps
// ReadStatus out of the way of everything else in the game: the opcode this
// detour sits under is read by rampage.sc and by nothing else, but a status
// of NONE read between two rampages must still come back as NONE.
bool g_localFrenzy = false;

// Our own engine's ending has already been offered to the session. One
// report per frenzy: ReadStatus is called every frame the script is in its
// wait loop, and without this the server would get a hundred identical
// candidate verdicts while the round trip is in the air.
bool g_endReported = false;

// The session's verdict, once there is one.
bool    g_haveVerdict = false;
uint8_t g_verdict     = 0;

// When our own ending was offered, on the wall clock rather than on CTimer -
// the whole point of the wait is that this machine's game time may be the one
// that is wrong.
uint32_t g_endReportedAtMs = 0;

// How long rampage.sc is held waiting for a verdict before it is given our own
// engine's answer instead.
//
// This is not a tuning number, it is a hang guard. A lost C_RampageStart or a
// session that dropped mid-frenzy would otherwise leave the script in its
// `while status == 1` loop with $ONMISSION set for the rest of the game, which
// is far worse than the disagreement the hold exists to prevent. (A server
// from before this feature used to be the third case; version 25 refuses that
// pairing at the door.) Ten seconds is long enough that no round trip is ever
// mistaken for a failure.
constexpr uint32_t kVerdictWaitMs = 10000;

// Set for the length of CreditRemotePedKill's call into the register: whose
// kill it is, and the victim's stats-screen type, for the report.
uint8_t g_creditKiller  = INVALID_PLAYER;
uint8_t g_creditPedType = NO_PED_TYPE;

// The reward watch. rampage.sc pays its reward in the same pass of its thread
// that reads PASSED out of 01FA, with no wait in between (RAMPAGE_REWARDS,
// `0109 player money += $RAMPAGE_REWARD`), so the money that thread's pass
// adds after ReadStatus hands PASSED over is the reward and nothing else.
// Armed by that hand-over, read when the thread's Process returns.
constexpr uintptr_t SCRIPT_PROCESS_CALL = 0x004393DF;   // CTheScripts::Process
bool    g_scriptCallRedirected = false;
bool    g_inScriptPass         = false;   // inside a pass reached through it
bool    g_rewardArmed          = false;
int32_t g_rewardMoneyBefore    = 0;
bool    g_saidReward           = false;

bool g_saidStart   = false;
bool g_saidKillOut = false;
bool g_saidKillIn  = false;
bool g_saidHeld    = false;
bool g_saidCarOut  = false;
bool g_saidCarIn   = false;
bool g_saidCarHeld = false;
bool g_saidKillHeld = false;
bool g_saidKillStats = false;

bool SessionOn() { return g_cb.HaveSession && g_cb.HaveSession(); }

// Off is not "the feature is broken"; it is a session that has chosen to keep
// rampages per-machine, which is what this build did before today. Every
// detour passes straight through and nothing is sent.
bool Sharing() { return SessionOn() && g_rule != RAMPAGE_RULE_OFF; }

// What the script put in CDarkel when it started this frenzy. Read fresh
// every time rather than cached at the start: the engine owns these, a save
// game load rewrites them, and a cache that is one frenzy stale would credit
// kills against the wrong models.
FrenzyTargets ReadFrenzy() {
	FrenzyTargets f;
	f.weapon       = Global<int32_t>(CDarkel__WeaponType);
	f.model1       = Global<int32_t>(CDarkel__ModelToKill);
	f.model2       = Global<int32_t>(CDarkel__ModelToKill2);
	f.model3       = Global<int32_t>(CDarkel__ModelToKill3);
	f.model4       = Global<int32_t>(CDarkel__ModelToKill4);
	f.needHeadShot = Global<uint8_t>(CDarkel__bNeedHeadShot) != 0;
	return f;
}

bool FrenzyOngoing() {
	return Global<uint16_t>(CDarkel__Status) == KILLFRENZY_ONGOING;
}

// A frenzy this machine shares with the session and is still running. The
// two registers' detours only ever interfere under this.
bool SharedFrenzyHere() {
	return Sharing() && g_localFrenzy && FrenzyOngoing();
}

// Runs one of the two registers with the rampage branch kept out of it.
//
// Both open on `cmp word [0095CCB4],1` and jump past the counter and the
// sound when it fails - 0x00420F76 in the kill register, 0x0042107D in the
// car one - and both keep their statistics below that jump. So Status reads
// NONE for the length of the call and goes back after it. The statistics the
// engine keeps for something that happened on this machine still move; the
// rampage doesn't. Game thread only, and neither function reads Status twice.
template <typename Call> void WithFrenzyHidden(Call &&call) {
	uint16_t      &status = Global<uint16_t>(CDarkel__Status);
	const uint16_t was    = status;
	status = KILLFRENZY_NONE;
	call();
	status = was;
}

int32_t &PlayerMoney() {
	const uintptr_t player =
	    CWorld__Players + Global<uint8_t>(CWorld__PlayerInFocus) * offs::PLAYERINFO_STRIDE;
	return Global<int32_t>(player + offs::PLAYERINFO_MONEY);
}

// The four statistics RegisterKillByPlayer moves for one type of victim.
KillStats ReadKillStats(uint8_t pedType) {
	KillStats s;
	s.peopleKilled    = Global<int32_t>(CStats__PeopleKilledByPlayer);
	s.ofType          = pedType < NUM_PEDTYPES
	                        ? Ptr<int32_t>(CStats__PedsKilledOfThisType)[pedType]
	                        : 0;
	s.headsPopped     = Global<int32_t>(CStats__HeadsPopped);
	s.sinceCheckpoint = Global<int32_t>(CStats__KillsSinceLastCheckpoint);
	return s;
}

void WriteKillStats(uint8_t pedType, const KillStats &s) {
	Global<int32_t>(CStats__PeopleKilledByPlayer) = s.peopleKilled;
	if (pedType < NUM_PEDTYPES)
		Ptr<int32_t>(CStats__PedsKilledOfThisType)[pedType] = s.ofType;
	Global<int32_t>(CStats__HeadsPopped)              = s.headsPopped;
	Global<int32_t>(CStats__KillsSinceLastCheckpoint) = s.sinceCheckpoint;
}

uint8_t PedStatsType(void *ped) {
	return KillStatsType(
	    (Field<uint8_t>(ped, offs::PED_FLAGS_15B) & offs::PED_CHRIS_CRIMINAL_BIT) != 0,
	    Field<int32_t>(ped, offs::PED_TYPE));
}

using PlayFrontEndSoundFn = void(__thiscall *)(void *, uint16_t, uint32_t);

void PlayRampageSound(uint16_t sound) {
	Func<PlayFrontEndSoundFn>(CAudioEngine__PlayFrontEndSound)(
	    Ptr<void>(DMAudio_Object), sound, 0);
}

// ---------------------------------------------------------------------------
// CDarkel::StartFrenzy - learning that this machine's script started one
// ---------------------------------------------------------------------------
//
// __cdecl, ten stack arguments, caller-cleaned. Every argument is declared
// four bytes wide here even where the engine reads two or one out of the
// slot - `movzx eax,word [esp+14h]` for the kill count at 0x00421102, `mov
// al,[esp+2Ch]` for the two flags at 0x0042116E and 0x00421179 - because the
// slot is a dword either way and a narrower declaration invites the compiler
// to disagree with the caller about it.
//
// The original runs first. Nothing here changes what the engine was asked
// for; the session's own target arrives a round trip later on S_RampageOpen
// and is written over the top, which under the default rule is the same
// number the script just wrote.
using StartFrenzyFn = void(__cdecl *)(int32_t, int32_t, uint32_t, int32_t, void *,
                                      int32_t, int32_t, int32_t, uint32_t, uint32_t);

void __cdecl HookedStartFrenzy(int32_t weaponType, int32_t time, uint32_t kill,
                               int32_t modelId0, void *text, int32_t modelId2,
                               int32_t modelId3, int32_t modelId4,
                               uint32_t standardSound, uint32_t needHeadShot) {
	g_startFrenzy.Original<StartFrenzyFn>()(weaponType, time, kill, modelId0, text,
	                                        modelId2, modelId3, modelId4,
	                                        standardSound, needHeadShot);

	++g_stats.startsSeen;
	g_localFrenzy = true;
	g_endReported = false;
	g_haveVerdict = false;
	g_verdict     = 0;
	g_countedCars.Clear();

	if (!Sharing())
		return;

	RampageStartBody body;
	body.limitMs = time;
	body.target  = static_cast<uint16_t>(kill & 0xFFFFu);

	if (!g_saidStart) {
		g_saidStart = true;
		Log("rampage: our own script started a frenzy - weapon %d, %d ms, %u "
		    "kills, models %d/%d/%d/%d%s. Every machine's rampage.sc did the "
		    "same thing in the same frame off its own aPickUpsCollected; what "
		    "goes on the wire from here is kills",
		    weaponType, time, body.target, modelId0, modelId2, modelId3, modelId4,
		    needHeadShot ? ", headshots only" : "");
	}

	if (g_cb.Started)
		g_cb.Started(body);
}

// ---------------------------------------------------------------------------
// CDarkel::RegisterKillByPlayer - witnessing a kill that counted
// ---------------------------------------------------------------------------
//
// __cdecl(CPed *victim, eWeaponType weapon, bool headshot), three stack
// arguments, caller-cleaned.
//
// **The report condition is the engine's own counter, not a copy of the
// engine's test.** The original runs, and if KillsNeeded moved then this kill
// counted - by the engine's judgement, including the five weapon aliases at
// 0x00420F7C..0x00420FBF, the four model ids, the headshot rule and the
// `Status == ONGOING` gate. There is nothing here to get subtly wrong,
// and the five callers of this function that are not CPed::InflictDamage - a
// car, a fire, a blast - are covered without any of them being named.
//
// The victim's model index is the only thing about the victim that goes out,
// because it is the only thing the receiving engine's own test will look at
// (`movsx eax,word [ebp+5Ch]` at 0x00420FCA).
using RegisterKillFn = void(__cdecl *)(void *, int32_t, int32_t);

void __cdecl HookedRegisterKillByPlayer(void *victim, int32_t weapon,
                                        int32_t headshot) {
	// The occupants of a wreck CoopIII is replaying. BlowUpCar registers the
	// driver and every passenger as killed by the player (0x0053BDB6,
	// 0x0053BE17) with no culprit test, so each machine holding the car used
	// to count them and report them. The machine that decided the wreck has
	// already done both.
	if (TallyCarWreck(SharedFrenzyHere(), ReplayingVehicleBlast(), false) ==
	    CarWreckTally::Withhold) {
		WithFrenzyHidden([&] {
			g_registerKill.Original<RegisterKillFn>()(victim, weapon, headshot);
		});
		++g_stats.killsWithheld;
		if (!g_saidKillHeld) {
			g_saidKillHeld = true;
			Log("rampage: somebody inside a car we're blowing up on another "
			    "machine's word died here. Not counted: that machine counted "
			    "them and told the session");
		}
		return;
	}

	const bool    wasOngoing  = FrenzyOngoing();
	const int32_t killsBefore = Global<int32_t>(CDarkel__KillsNeeded);

	g_registerKill.Original<RegisterKillFn>()(victim, weapon, headshot);

	// The session's mission counts its kills on the owner's machine alone, so
	// a participant's go there (mission-audit.md R11). Not the occupants of a
	// wreck this machine is replaying: the machine that decided the wreck
	// registers those.
	if (victim && !ReplayingVehicleBlast())
		MissionKillRegistered(victim);

	if (!Sharing() || !g_localFrenzy || !victim || !wasOngoing)
		return;
	if (Global<int32_t>(CDarkel__KillsNeeded) == killsBefore)
		return;   // the engine did not count it, so neither does the session

	const uint16_t model =
	    static_cast<uint16_t>(Field<int16_t>(victim, offs::MODEL_INDEX));

	++g_stats.killsSent;
	if (!g_saidKillOut) {
		g_saidKillOut = true;
		Log("rampage: our first kill of this frenzy counted here - model %u with "
		    "weapon %d%s, %d left on our own counter. It is on its way to "
		    "everybody else, as the three arguments the engine's own register "
		    "takes",
		    model, weapon, headshot ? " (headshot)" : "",
		    Global<int32_t>(CDarkel__KillsNeeded));
	}

	if (g_cb.Kill)
		g_cb.Kill(model, static_cast<uint8_t>(weapon), headshot != 0, g_creditKiller,
		          g_creditPedType);
}

// ---------------------------------------------------------------------------
// CDarkel::RegisterCarBlownUpByPlayer - one car, one count
// ---------------------------------------------------------------------------
//
// __cdecl(CVehicle *vehicle), one stack argument, caller-cleaned: both call
// sites are `push reg / call 00421070 / pop ecx` (0x0053BF03, 0x0054A04E),
// and the body reads it at `mov ebx,[esp+8]` after its one push.
//
//   0x00421070  cmp word [0095CCB4],1       Status == ONGOING, else skip to
//   0x0042107D  jne 004210C0                the statistics
//   0x0042107F  cmp [008F2C78],-2           ModelToKill == FRENZY_ANY_CAR
//   0x00421088  movsx eax,word [ebx+5Ch]    .. or the car's model is one of
//   0x0042108C  ..0x004210AA                the four
//   0x004210B1  dec dword [008F1AB8]        KillsNeeded--
//   0x004210BB  PlayFrontEndSound(5Dh, 0)   SOUND_RAMPAGE_CAR_BLOWN
//   0x004210C4  inc word [eax*2+006EDBE0]   RegisteredKills[model], always
//   0x004210CC  inc dword [00941288]        CStats::CarsExploded, always
//
// The callers are CAutomobile::BlowUpCar (0x0053BF04, unconditional, no
// culprit anywhere near it) and CHeli::UpdateHelis (0x0054A04F). CBoat's
// BlowUpCar doesn't call it, so a boat never counts toward a rampage, in
// single player or here.
//
// Same report condition as the kill: the engine's own counter. What's added
// is deciding first whether this wreck is this machine's to count at all -
// darkel.h, "one car, one count".
using RegisterCarFn = void(__cdecl *)(void *);

void __cdecl HookedRegisterCarBlownUpByPlayer(void *vehicle) {
	const bool shared = vehicle && SharedFrenzyHere();
	const WreckForRampage wreck =
	    shared ? DescribeWreckForRampage(vehicle) : WreckForRampage{};

	switch (TallyCarWreck(shared, wreck.replay, g_countedCars.Has(wreck.key))) {
	case CarWreckTally::Engine:
		g_registerCar.Original<RegisterCarFn>()(vehicle);
		return;

	case CarWreckTally::Withhold:
		WithFrenzyHidden([&] { g_registerCar.Original<RegisterCarFn>()(vehicle); });
		++g_stats.carsWithheld;
		if (!g_saidCarHeld) {
			g_saidCarHeld = true;
			Log("rampage: a car blew up here that %s. Kept off our counter; the "
			    "machine that decided it counts it for the session",
			    wreck.replay ? "another machine decided"
			                 : "the session had already counted");
		}
		return;

	case CarWreckTally::Count:
		break;
	}

	const int32_t killsBefore = Global<int32_t>(CDarkel__KillsNeeded);
	g_registerCar.Original<RegisterCarFn>()(vehicle);
	if (Global<int32_t>(CDarkel__KillsNeeded) == killsBefore)
		return;   // not a car this frenzy wants

	g_countedCars.Add(wreck.key);
	const uint16_t model =
	    static_cast<uint16_t>(Field<int16_t>(vehicle, offs::MODEL_INDEX));

	++g_stats.carsSent;
	if (!g_saidCarOut) {
		g_saidCarOut = true;
		Log("rampage: our first car of this frenzy counted here - model %u%s, %d "
		    "left on our own counter. On its way to everybody else",
		    model,
		    wreck.key.kind == UNOWNED_PARKED    ? " (a parked car)"
		    : wreck.key.kind == UNOWNED_SESSION ? " (a session car nobody drove)"
		                                        : "",
		    Global<int32_t>(CDarkel__KillsNeeded));
	}

	if (g_cb.CarDestroyed)
		g_cb.CarDestroyed(model, wreck.key);
}

// ---------------------------------------------------------------------------
// CDarkel::ReadStatus - the script's view, which is the session's
// ---------------------------------------------------------------------------
//
// `mov ax,word [0095CCB4] / ret`, __cdecl, no arguments, and **one caller in
// the whole image**: 0x00442BE8, the handler for script opcode 01FA, which
// `movzx eax,ax` immediately afterwards. Nothing else in GTA III reads the
// status through this function - the HUD goes through FrenzyOnGoing
// (0x005062CE), CanBePickedUp through FrenzyOnGoing (0x00430E99 and
// 0x00431543) and CDarkel::Update reads the global itself.
//
// So this is the one place where "what this machine's engine decided" and
// "what the session decided" can be told apart without CoopIII owning any of
// the rampage. The engine keeps its HUD, its countdown, its tick sound and
// its weapon restore; the *script* - the thing that hands out the reward,
// prints RAMPAGE FAILED, puts the pickup back and clears $ONMISSION - is held
// on ONGOING until the session has one answer for everybody.
using ReadStatusFn = uint16_t(__cdecl *)();

uint16_t __cdecl HookedReadStatus() {
	const uint16_t engine = Global<uint16_t>(CDarkel__Status);

	if (!Sharing() || !g_localFrenzy)
		return engine;

	// The session has spoken. Hand it over and get out of the way; the next
	// frenzy starts this again from the StartFrenzy detour.
	if (g_haveVerdict) {
		g_localFrenzy = false;
		g_endReported = false;
		g_haveVerdict = false;
		// The script pays its reward before this pass of it ends. Watched
		// from the call into it, so what it paid can be told apart from
		// anything else that pays this frame.
		if (g_verdict == RAMPAGE_PASSED && g_inScriptPass) {
			g_rewardArmed       = true;
			g_rewardMoneyBefore = PlayerMoney();
		}
		return g_verdict;
	}

	if (engine == KILLFRENZY_ONGOING)
		return engine;

	// Our own engine has ended it and nobody else has yet. Offer what it
	// reached as one candidate verdict - once - and keep the script waiting.
	if (!g_endReported && IsRampageOutcome(static_cast<uint8_t>(engine))) {
		g_endReported     = true;
		g_endReportedAtMs = WallClock::NowMs();
		++g_stats.endsSent;
		Log("rampage: our own CDarkel ended this frenzy as %s. Reported to the "
		    "session as one candidate; the script waits on the verdict so every "
		    "machine leaves its wait loop on the same value",
		    engine == RAMPAGE_PASSED ? "passed" : "failed");
		if (g_cb.Ended)
			g_cb.Ended(static_cast<uint8_t>(engine));
	}

	// Held long enough. Nobody is coming, so the script gets what this
	// machine's own engine decided - which is single player's answer, and a
	// disagreement between two screens is a much smaller problem than a
	// script thread stuck on a mission flag.
	if (g_endReported && WallClock::NowMs() - g_endReportedAtMs > kVerdictWaitMs) {
		Log("rampage: waited %u ms for the session to agree an ending and heard "
		    "nothing. Handing rampage.sc our own engine's answer (%s) rather "
		    "than leaving the script waiting with $ONMISSION set",
		    kVerdictWaitMs, engine == RAMPAGE_PASSED ? "passed" : "failed");
		g_localFrenzy = false;
		g_endReported = false;
		return engine;
	}

	++g_stats.heldFrames;
	if (!g_saidHeld) {
		g_saidHeld = true;
		Log("rampage: holding rampage.sc on ONGOING while the session agrees an "
		    "ending. The HUD, the clock and the weapon are the engine's and are "
		    "not touched");
	}
	return KILLFRENZY_ONGOING;
}

// ---------------------------------------------------------------------------
// CDarkel::RegisterKillNotByPlayer - a blast somebody else threw
// ---------------------------------------------------------------------------
//
// missioncombat.h, KilledByRemoteBlast, says why and where. Reached by call
// redirection, at CPed::InflictDamage's one call to it (0x004EAD55).
constexpr uintptr_t KILL_NOT_BY_PLAYER_CALL = 0x004EAD55;
constexpr uintptr_t EXPLOSIONS              = 0x0064E208;   // gaExplosion
constexpr size_t    EXPLOSION_COUNT         = 0x30;
constexpr size_t    EXPLOSION_SIZE          = 0x3C;
constexpr size_t    EXPLOSION_POS           = 0x04;
constexpr size_t    EXPLOSION_RADIUS        = 0x10;
constexpr size_t    EXPLOSION_CREATOR       = 0x18;
constexpr size_t    EXPLOSION_ITERATION     = 0x24;

bool g_killNotRedirected = false;
bool g_saidBlastKill     = false;

using RegisterKillNotFn = void(__cdecl *)(void *, int32_t);

void __cdecl HookedRegisterKillNotByPlayer(void *victim, int32_t weapon) {
	Func<RegisterKillNotFn>(CDarkel__RegisterKillNotByPlayer)(victim, weapon);
	if (!victim || weapon != WEAPONTYPE_EXPLOSION)
		return;
	// Only a pedestrian this machine hosts: every other machine's copy of it
	// is its host's to judge, and one nobody shares would be judged by every
	// machine the blast went off on.
	bool named = false;
	if (!HostedPedFor(victim, named))
		return;
	BlastSeen blasts[EXPLOSION_COUNT];
	for (size_t i = 0; i < EXPLOSION_COUNT; ++i) {
		const uintptr_t row = EXPLOSIONS + i * EXPLOSION_SIZE;
		BlastSeen      &b   = blasts[i];
		b.active            = Global<uint8_t>(row + EXPLOSION_ITERATION) != 0;
		if (!b.active)
			continue;
		b.x            = Global<float>(row + EXPLOSION_POS);
		b.y            = Global<float>(row + EXPLOSION_POS + 4);
		b.z            = Global<float>(row + EXPLOSION_POS + 8);
		b.radius       = Global<float>(row + EXPLOSION_RADIUS);
		b.player       = RemotePlayerOfEntity(Global<void *>(row + EXPLOSION_CREATOR));
		b.remotePlayer = b.player != INVALID_PLAYER;
	}
	const float *at = &Field<float>(victim, offs::POSITION);
	if (!KilledByRemoteBlast(at[0], at[1], at[2], blasts, EXPLOSION_COUNT))
		return;
	if (!g_saidBlastKill) {
		g_saidBlastKill = true;
		Log("rampage: a pedestrian we host died in somebody else's blast; it counts for them "
		    "through our own register (mission-audit.md R11)");
	}
	CreditRemotePedKill(victim, WEAPONTYPE_EXPLOSION, 0,
	                    RemoteBlastKiller(at[0], at[1], at[2], blasts, EXPLOSION_COUNT));
}

bool RedirectRelCall(uintptr_t site, uintptr_t from, uintptr_t to) {
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

// ---------------------------------------------------------------------------
// CRunningScript::Process - one thread's pass, for the reward watch
// ---------------------------------------------------------------------------
//
// Reached by call redirection: CTheScripts::Process has one call to it, `call
// 00439440` at 0x004393DF, inside its walk over pActiveScripts with the
// thread in ecx. __thiscall, no arguments, so a __fastcall that ignores edx
// stands in for it.
//
// Every thread goes through here and nothing is done for any of them unless
// ReadStatus has just handed PASSED over. Then this is the thread that read
// it, still inside the same pass, and what the money did between the hand-over
// and the end of the pass is what rampage.sc paid.
using ScriptProcessFn = void(__thiscall *)(void *);

void __fastcall HookedScriptProcess(void *script, void * /*edx*/) {
	g_inScriptPass = true;
	Func<ScriptProcessFn>(CRunningScript__Process)(script);
	g_inScriptPass = false;
	if (!g_rewardArmed)
		return;
	g_rewardArmed      = false;
	const int32_t paid = PlayerMoney() - g_rewardMoneyBefore;
	if (paid <= 0)
		return;
	if (!g_saidReward) {
		g_saidReward = true;
		Log("rampage: our rampage.sc paid $%d for the session's pass", paid);
	}
	if (g_cb.RewardPaid)
		g_cb.RewardPaid(paid);
}

} // namespace

// ---------------------------------------------------------------------------
// Inbound
// ---------------------------------------------------------------------------

void SetRampageRule(uint8_t rule) {
	g_rule = rule > RAMPAGE_RULE_OFF ? uint8_t(RAMPAGE_RULE_SHARED) : rule;
}

void ApplyRampageOpen(uint16_t killsNeeded, uint32_t elapsedMs) {
	if (!FrenzyOngoing() || killsNeeded == 0)
		return;

	const int32_t beforeKills = Global<int32_t>(CDarkel__KillsNeeded);
	if (static_cast<int32_t>(killsNeeded) != beforeKills) {
		// One store into the engine's own counter, which is what the HUD
		// draws and what Update compares against zero. Reached under `scaled`,
		// and on a machine that joined a frenzy already in progress.
		Global<int32_t>(CDarkel__KillsNeeded) = static_cast<int32_t>(killsNeeded);
		Log("rampage: the session still wants %u kills, not the %d our own "
		    "script asked for", killsNeeded, beforeKills);
	}

	// How long the session's frenzy has been running, against how long ours
	// has. The engine keeps the *start* rather than the remaining time
	// (`FrameTime = TimeLimit - (CTimer::ms - TimeOfFrenzyStart)` at
	// 0x0042067B), so moving the start backwards is how a clock is fast
	// forwarded - and it is one store rather than a clock CoopIII would have
	// to keep running.
	//
	// A second of slack, because the ordinary case is a frenzy that started
	// here a round trip ago and a countdown nobody should see jump.
	constexpr uint32_t kSlackMs = 1000;
	const int32_t oursMs = Global<int32_t>(CTimer__m_snTimeInMilliseconds) -
	                       Global<int32_t>(CDarkel__TimeOfFrenzyStart);
	if (elapsedMs > static_cast<uint32_t>(oursMs < 0 ? 0 : oursMs) + kSlackMs) {
		Global<int32_t>(CDarkel__TimeOfFrenzyStart) =
		    Global<int32_t>(CTimer__m_snTimeInMilliseconds) -
		    static_cast<int32_t>(elapsedMs);
		Log("rampage: joined a frenzy that has been running %u ms. Our own "
		    "CDarkel was %d ms in, so its start has been moved back and the "
		    "HUD clock now says what everybody else's does",
		    elapsedMs, oursMs);
	}
}

void CreditRampageKill(uint16_t model, uint8_t weapon, bool headshot) {
	if (!FrenzyOngoing()) {
		++g_stats.killsRefused;
		return;
	}

	if (!RampageKillCounts(ReadFrenzy(), static_cast<int32_t>(model),
	                       static_cast<int32_t>(weapon), headshot)) {
		// Not a refusal of the sender - the same kill counted on their
		// machine and this one simply has a different frenzy running, or the
		// packet raced our own ending. The engine's own test decides, here as
		// there.
		++g_stats.killsRefused;
		return;
	}

	// Exactly the two writes and the one sound the engine makes for a kill
	// that counts, at 0x00421000, 0x0042100A and 0x00421019, and nothing
	// else. The statistics below those three lines are deliberately not
	// copied: CStats::PedsKilledOfThisType indexes off the victim's
	// m_nPedType and CStats::PeopleKilledByPlayer means "by the player at
	// this keyboard", and this machine has neither the ped nor the claim.
	Global<int32_t>(CDarkel__KillsNeeded) -= 1;
	PlayRampageSound(SOUND_RAMPAGE_KILL);

	// The bound is not optional. `inc word [eax*2+006EDBE0h]` has no check in
	// front of it, the array is 200 entries (the reset loop at 0x00421310
	// stops at 0C8h), and `model` arrived off a socket. Past the end of it is
	// CRadar's own neighbourhood - addresses.h has the note about the radar
	// overflow that lands in this same array from the other side.
	if (model < NUM_DEFAULT_MODELS)
		Ptr<uint16_t>(CDarkel__RegisteredKills)[model] += 1;

	++g_stats.killsTaken;
	if (!g_saidKillIn) {
		g_saidKillIn = true;
		Log("rampage: took our first kill off the wire - model %u, weapon %u%s. "
		    "%d to go, on this machine's own counter and its own HUD",
		    model, weapon, headshot ? " (headshot)" : "",
		    Global<int32_t>(CDarkel__KillsNeeded));
	}
}

void CreditRampageCar(uint16_t model, const UnownedVehicleKey &key) {
	// Without our own detour this machine still counts every copy it blows
	// up, replays included, which is what it did before cars were shared.
	// Adding the wire on top of that would count those cars twice.
	if (!g_registerCar.IsInstalled()) {
		++g_stats.carsRefused;
		return;
	}

	const bool counted = g_countedCars.Has(key);
	if (!CarFromWireCounts(FrenzyOngoing(), ReadFrenzy(), static_cast<int32_t>(model),
	                       counted)) {
		// Not running one, not a car this one wants, or a parked car whose
		// copy already blew up and counted here.
		++g_stats.carsRefused;
		return;
	}

	g_countedCars.Add(key);

	// The decrement and the sound at 0x004210B1 and 0x004210BB, and not the two
	// lines under them. RegisteredKills[model] and CStats::CarsExploded move
	// when a car blows up on this machine, whoever blew it up, and a car this
	// machine held already moved them when its copy was replayed. Bumping them
	// again here would count that car twice on the stats screen.
	Global<int32_t>(CDarkel__KillsNeeded) -= 1;
	PlayRampageSound(SOUND_RAMPAGE_CAR_BLOWN);

	++g_stats.carsTaken;
	if (!g_saidCarIn) {
		g_saidCarIn = true;
		Log("rampage: took our first car off the wire - model %u. %d to go on "
		    "this machine's own counter", model, Global<int32_t>(CDarkel__KillsNeeded));
	}
}

void CreditRemotePedKill(void *ped, uint8_t weapon, uint8_t piece, uint8_t killer) {
	// During a rampage, and during the session's mission, whose kills Uzi
	// Rider and Bait count (mission-audit.md R11). Anywhere else a kill
	// somebody else made stays theirs, as it was.
	if (!ped || !(FrenzyOngoing() || MissionCreditsRemoteKills()))
		return;

	// The gap this closes is the one named at CDarkel__KILL_CREDIT_TEST: this
	// machine hosts the pedestrian, somebody else shot it, and the engine
	// sent the kill to RegisterKillNotByPlayer because the damaging entity
	// was a replica rather than FindPlayerPed(). Nobody counted it - not here
	// and not on the shooter's machine, whose own InflictDamage returned
	// before that line.
	//
	// So it is put through the engine's own register, which also puts it
	// through this file's detour, which is what sends it to everybody else.
	// One path for every rampage kill in the session.
	//
	// **headshot is derived rather than carried, and only here.** The
	// engine's own `headShot` is a stack local set at 0x004EA7F7 in the
	// bullet arm's PEDPIECE_HEAD case, and it is only reachable when the limb
	// came off - which for the pistol, the uzi and the shotgun is a
	// CGeneral::GetRandomNumber() roll two machines would not agree on. It
	// does not matter: all three headshot rampages in rampage.sc use
	// SNIPERRIFLE or M16, and those two weapons take the arm where the roll
	// is skipped entirely, so for them headshot is exactly
	// `pedPiece == PEDPIECE_HEAD`.
	//
	// **The statistics below the register's rampage branch are kept off this
	// screen.** PeopleKilledByPlayer, PedsKilledOfThisType, HeadsPopped and
	// KillsSinceLastCheckpoint mean the player at this keyboard, who didn't
	// make this kill, and the engine has already counted the death once here,
	// under PeopleKilledByOthers. So they're put back after the call, and the
	// report the detour makes names `killer` so they land on his screen
	// instead (CreditKillStats). RegisteredKills[model] is the rampage's and
	// the missions', not the stats screen's, and is left as the engine moves it.
	const bool    headshot = piece == PEDPIECE_HEAD;
	const uint8_t pedType  = PedStatsType(ped);
	const KillStats before = ReadKillStats(pedType);

	g_creditKiller  = killer;
	g_creditPedType = pedType;
	Func<void(__cdecl *)(void *, int32_t, int32_t)>(CDarkel__RegisterKillByPlayer)(
	    ped, static_cast<int32_t>(weapon), headshot ? 1 : 0);
	g_creditKiller  = INVALID_PLAYER;
	g_creditPedType = NO_PED_TYPE;

	WriteKillStats(pedType, before);
}

void CreditKillStats(uint8_t pedType, bool headshot) {
	KillStats s = ReadKillStats(pedType);
	AddKillStats(s, pedType, headshot);
	WriteKillStats(pedType, s);
	if (!g_saidKillStats) {
		g_saidKillStats = true;
		Log("rampage: a kill of ours that another machine's engine counted is on our "
		    "stats screen now (type %u%s)", pedType, headshot ? ", headshot" : "");
	}
}

void ApplyRampageVerdict(uint8_t outcome) {
	if (!IsRampageOutcome(outcome))
		return;

	++g_stats.verdicts;
	g_verdict     = outcome;
	g_haveVerdict = true;

	// Our own engine has already ended it, one way or the other. Nothing to
	// do: it has done its own weapon restore and its own sound, and the next
	// ReadStatus hands the session's answer to the script.
	if (!FrenzyOngoing())
		return;

	// Somebody else finished first. Rather than write Status - which would
	// skip the weapon restore, the m_AllRandomPedsThisType reset and the
	// sound, all of which live in CDarkel::Update's two arms - move the
	// engine's own inputs so that Update reaches the same ending by itself on
	// the next frame. Replicate the cause, let the engine produce the effect;
	// it is the same rule the fires and the pickups are built on.
	if (outcome == RAMPAGE_PASSED) {
		// 0x004207FE: `cmp [008F1AB8h],0 / jg out`, then Status = 2.
		Global<int32_t>(CDarkel__KillsNeeded) = 0;
	} else {
		// 0x0042067B: FrameTime = TimeLimit - (CTimer::ms - TimeOfFrenzyStart),
		// and the fail arm is taken when that is <= 0 and TimeLimit >= 0. Both
		// stores are needed: a rampage with a negative TimeLimit never fails on
		// time, and rampage.sc's own no-limit frenzies are exactly that case.
		//
		// KillsNeeded is left alone on purpose. The fail arm falls through to
		// the pass test at 0x004207FE, so a counter already at zero would turn
		// this into a pass - but a counter already at zero means our own engine
		// passed it a frame ago, and the `!FrenzyOngoing()` return above has
		// already sent us home.
		Global<int32_t>(CDarkel__TimeLimit) = 0;
		Global<int32_t>(CDarkel__TimeOfFrenzyStart) =
		    Global<int32_t>(CTimer__m_snTimeInMilliseconds);
	}

	Log("rampage: the session says this frenzy %s. Our own CDarkel was still "
	    "running it, so its inputs have been moved and its own Update will end "
	    "it on the next frame - weapon restore, sound and all",
	    outcome == RAMPAGE_PASSED ? "passed" : "failed");
}

void ResetRampage() {
	g_localFrenzy = false;
	g_endReported = false;
	g_haveVerdict = false;
	g_verdict     = 0;
	g_rule        = RAMPAGE_RULE_SHARED;
	g_rewardArmed = false;
	g_countedCars.Clear();
}

// ---------------------------------------------------------------------------

void SetRampageCallbacks(const RampageCallbacks &callbacks) { g_cb = callbacks; }

const RampageStats &GetRampageStats() { return g_stats; }

bool InstallRampageHooks() {
	ResetRampage();

	// Four detours and four different failures, so four lines rather than
	// one. None of them is fatal, and between them they degrade in the right
	// order: without ReadStatus the scripts can disagree about an ending,
	// without RegisterKillByPlayer this machine's kills stay its own, without
	// RegisterCarBlownUpByPlayer its cars do, and without StartFrenzy the
	// session never hears that a rampage began at all.
	const bool status =
	    g_readStatus.Install("CDarkel::ReadStatus", Ptr<void>(CDarkel__ReadStatus),
	                         reinterpret_cast<void *>(&HookedReadStatus));
	const bool kill = g_registerKill.Install(
	    "CDarkel::RegisterKillByPlayer", Ptr<void>(CDarkel__RegisterKillByPlayer),
	    reinterpret_cast<void *>(&HookedRegisterKillByPlayer));
	const bool car = g_registerCar.Install(
	    "CDarkel::RegisterCarBlownUpByPlayer",
	    Ptr<void>(CDarkel__RegisterCarBlownUpByPlayer),
	    reinterpret_cast<void *>(&HookedRegisterCarBlownUpByPlayer));
	const bool start =
	    g_startFrenzy.Install("CDarkel::StartFrenzy", Ptr<void>(CDarkel__StartFrenzy),
	                          reinterpret_cast<void *>(&HookedStartFrenzy));
	// Not part of the answer: without it a pedestrian somebody else's grenade
	// or rocket kills here counts for nobody, as it always has.
	if (!g_killNotRedirected) {
		g_killNotRedirected =
		    RedirectRelCall(KILL_NOT_BY_PLAYER_CALL, CDarkel__RegisterKillNotByPlayer,
		                    reinterpret_cast<uintptr_t>(&HookedRegisterKillNotByPlayer));
		if (!g_killNotRedirected)
			Log("CoopIII: the call at 0x%08X is not CPed::InflictDamage's to "
			    "CDarkel::RegisterKillNotByPlayer; a kill by somebody else's blast here counts "
			    "for nobody",
			    static_cast<unsigned>(KILL_NOT_BY_PLAYER_CALL));
	}
	// Not part of the answer either: without it a shared wallet tells our own
	// rampage reward from other money by its size alone (moneysync.h).
	if (!g_scriptCallRedirected) {
		g_scriptCallRedirected =
		    RedirectRelCall(SCRIPT_PROCESS_CALL, CRunningScript__Process,
		                    reinterpret_cast<uintptr_t>(&HookedScriptProcess));
		if (!g_scriptCallRedirected)
			Log("CoopIII: the call at 0x%08X is not CTheScripts::Process's to "
			    "CRunningScript::Process; a rampage reward that lands in the same frame "
			    "as other money may go into a shared wallet twice",
			    static_cast<unsigned>(SCRIPT_PROCESS_CALL));
	}

	if (!start)
		Log("CoopIII: the session will never hear that a rampage started, so "
		    "nobody's kills will be shared");
	if (!kill)
		Log("CoopIII: this machine's rampage kills stay on this machine");
	if (!car)
		Log("CoopIII: a vehicle rampage counts only the cars that blow up on "
		    "this machine, and cars from the wire are ignored so none counts "
		    "twice");
	if (!status)
		Log("CoopIII: a rampage can end differently on different machines - one "
		    "player gets the reward while another is told it failed");

	return start && kill && status && car;
}

void RemoveRampageHooks() {
	if (g_killNotRedirected) {
		RedirectRelCall(KILL_NOT_BY_PLAYER_CALL,
		                reinterpret_cast<uintptr_t>(&HookedRegisterKillNotByPlayer),
		                CDarkel__RegisterKillNotByPlayer);
		g_killNotRedirected = false;
	}
	if (g_scriptCallRedirected) {
		RedirectRelCall(SCRIPT_PROCESS_CALL, reinterpret_cast<uintptr_t>(&HookedScriptProcess),
		                CRunningScript__Process);
		g_scriptCallRedirected = false;
	}
	g_rewardArmed = false;
	g_startFrenzy.Remove();
	g_registerKill.Remove();
	g_readStatus.Remove();
	g_registerCar.Remove();
	ResetRampage();
}

bool RampageHooksInstalled() {
	return g_startFrenzy.IsInstalled() && g_registerKill.IsInstalled() &&
	       g_readStatus.IsInstalled() && g_registerCar.IsInstalled();
}

void InstallRampageBridge(WorldBridge &b) {
	b.SetRampageRule     = &SetRampageRule;
	b.ApplyRampageOpen   = &ApplyRampageOpen;
	b.CreditRampageKill  = &CreditRampageKill;
	b.CreditKillStats    = &CreditKillStats;
	b.CreditRampageCar   = &CreditRampageCar;
	b.ApplyRampageVerdict = &ApplyRampageVerdict;
	b.ResetRampage       = &ResetRampage;
}

} // namespace coopiii::game
