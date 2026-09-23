// Rampages: one objective, one kill count, for the whole session.
//
// docs/roadmap.md §5.10 decided this and docs/pickups.md §6 argued it. What a
// reader of this file needs is five sentences.
//
// 1. **The frenzy already starts everywhere and no packet does it.** Every
//    machine runs `rampage.sc`; the pickup work pushes a remote collection
//    into every machine's own `CPickups::aPickUpsCollected`; each machine's
//    own script then calls `CDarkel::StartFrenzy` with the same ten arguments
//    in the same frame. The weapon, the 120 seconds, the four target models,
//    the HUD, the start message and the "Murder ~1~ Diablos" line are all
//    already identical, for free.
//
// 2. **What does not survive is the counting.** `CDarkel::KillsNeeded` is
//    decremented by `CDarkel::RegisterKillByPlayer` (0x00420F60), and
//    `CPed::InflictDamage` only reaches that function when the damaging
//    entity is `FindPlayerPed()` or `FindPlayerVehicle()` - the test at
//    0x004EAD1A. In a session the pedestrian is hosted by one machine and
//    shot by another, so on the host the damager is a *replica* of the
//    shooter and the kill goes to `RegisterKillNotByPlayer`, which bumps one
//    statistic; and on the shooter's own machine `InflictDamage` returned
//    long before that line, because game/combat.cpp turned the hit into a
//    packet. A co-op kill counts for nobody.
//
// 3. **So the kill is what travels, as the engine's own three arguments.**
//    The victim's model index, the weapon, and the headshot bit - read out of
//    a detour on `RegisterKillByPlayer` itself, which means it is the
//    engine's judgement of "a kill that counts" and not CoopIII's, including
//    the five callers of that function that are not `InflictDamage` (a car, a
//    fire, a blast). A netId is deliberately *not* used: a player 200 m away
//    has no replica of that pedestrian and must still be able to judge the
//    kill.
//
// 4. **Nothing here reimplements CDarkel.** Every machine's engine goes on
//    running retail's own `Update` - its own countdown, its own tick sound,
//    its own weapon restore, its own HUD. The only two things CoopIII writes
//    into CDarkel are a number the session agreed (`KillsNeeded` at the
//    start) and a number the engine itself produced somewhere else (the same
//    counter, one kill at a time).
//
// 5. **The ending is arbitrated, and the seam is the script's view.**
//    `CDarkel::ReadStatus` (0x00420E50) has exactly one caller in the whole
//    image: the handler for script opcode 01FA, which is what `rampage.sc`
//    sits in a wait loop on. Detouring it lets every machine's script leave
//    that loop on the same value at the same moment, without touching the
//    HUD (which reads the global through `FrenzyOnGoing`) or the engine's own
//    ending. A machine that ends early reports what it reached; the first
//    report to the server is the session's verdict.
#pragma once

// addresses.h rather than a copy of the two sentinels, because they are read
// off the comparisons at 0x00420FC1 and 0x0042107F and belong next to the
// disassembly that proves them. It is constants only and costs a test nothing.
#include "addresses.h"
#include "client.h"

#include <coopiii/protocol.h>

#include <cstdint>

namespace coopiii::game {

// ---- the decision, as arithmetic ------------------------------------------

// Does this kill count toward this frenzy?
//
// A transcription of `CDarkel::RegisterKillByPlayer`'s two tests, 0x00420F7C
// to 0x00420FF9, and deliberately with no engine dependency so
// tools/clienttest can put the awkward cases through it.
//
// The five weapon aliases are the engine's, not CoopIII's, and they are the
// reason this is a function rather than `weapon == frenzyWeapon`:
//
//   0x00420F84  cmp eax,12h                  an explosion counts for every
//                                            rampage there is
//   0x00420F89  eax==13h && WeaponType==3    a drive-by uzi is an uzi
//   0x00420F97  eax==10h && WeaponType==11h  rammed counts as run over
//   0x00420FA5  eax==11h && WeaponType==10h  and the other way round
//   0x00420FB3  eax==9   && WeaponType==0Ah  a flamethrower counts for a
//                                            molotov rampage
//
// The model test is 0x00420FC1 to 0x00420FEC: FRENZY_ANY_PED, or any of the
// four ids the script passed. FRENZY_ANY_CAR never reaches here - that is
// `RegisterCarBlownUpByPlayer`'s sentinel on the same global - so a car
// rampage's `-2` matches nothing and no pedestrian is ever credited to one,
// which is what the engine does too.
//
// The headshot test is 0x00420FEE: `bNeedHeadShot` false, or the bit set.
struct FrenzyTargets {
	int32_t weapon = 0;              // CDarkel::WeaponType
	int32_t model1 = FRENZY_ANY_PED; // ModelToKill .. ModelToKill4
	int32_t model2 = FRENZY_ANY_PED;
	int32_t model3 = FRENZY_ANY_PED;
	int32_t model4 = FRENZY_ANY_PED;
	bool    needHeadShot = false;
};

inline bool RampageWeaponCounts(int32_t weapon, int32_t frenzyWeapon) {
	// The numbers are eWeaponType as retail holds it, pinned by
	// StartFrenzy's own `cmp eax,13h / mov ebx,3` at 0x004210E5: 3 is UZI and
	// 13h is UZI_DRIVEBY, which puts COLT45 at 2 and SNIPERRIFLE at 7.
	constexpr int32_t WT_FLAMETHROWER = 0x09;
	constexpr int32_t WT_MOLOTOV      = 0x0A;
	constexpr int32_t WT_RAMMEDBYCAR  = 0x10;
	constexpr int32_t WT_RUNOVERBYCAR = 0x11;
	constexpr int32_t WT_EXPLOSION    = 0x12;
	constexpr int32_t WT_UZI_DRIVEBY  = 0x13;
	constexpr int32_t WT_UZI          = 0x03;

	return weapon == frenzyWeapon || weapon == WT_EXPLOSION ||
	       (weapon == WT_UZI_DRIVEBY && frenzyWeapon == WT_UZI) ||
	       (weapon == WT_RAMMEDBYCAR && frenzyWeapon == WT_RUNOVERBYCAR) ||
	       (weapon == WT_RUNOVERBYCAR && frenzyWeapon == WT_RAMMEDBYCAR) ||
	       (weapon == WT_FLAMETHROWER && frenzyWeapon == WT_MOLOTOV);
}

inline bool RampageModelCounts(const FrenzyTargets &f, int32_t model) {
	return f.model1 == FRENZY_ANY_PED || f.model1 == model || f.model2 == model ||
	       f.model3 == model || f.model4 == model;
}

inline bool RampageKillCounts(const FrenzyTargets &f, int32_t model, int32_t weapon,
                              bool headshot) {
	if (!RampageWeaponCounts(weapon, f.weapon))
		return false;
	if (!RampageModelCounts(f, model))
		return false;
	return !f.needHeadShot || headshot;
}

// Does this car count toward this frenzy?
//
// `CDarkel::RegisterCarBlownUpByPlayer`'s test, 0x0042107F to 0x004210AA:
// FRENZY_ANY_CAR in the first slot, or the car's model in any of the four.
// There is no weapon test and no headshot, because the function reads
// neither - a rocket rampage counts a car that burned out from gunfire.
inline bool RampageCarCounts(const FrenzyTargets &f, int32_t model) {
	return f.model1 == FRENZY_ANY_CAR || f.model1 == model || f.model2 == model ||
	       f.model3 == model || f.model4 == model;
}

// ---- one car, one count ----------------------------------------------------
//
// The car register tests the same Status and the same four models and
// decrements the same counter as the kill register, but it isn't reached the
// same way. CPed::InflictDamage only calls RegisterKillByPlayer when the
// killer is this machine's own player (0x004EAD1A). RegisterCarBlownUpByPlayer
// is called by every CAutomobile::BlowUpCar, at 0x0053BF04, with no culprit
// test in the function at all. So every machine that has a copy of a car
// counts it when that copy blows up, including when CoopIII blows it up
// because the car's owner said so, and a machine without a copy never counts
// it.
//
// So what counts is decided per wreck, on this machine:
//
//  - a replay - BlowUpCar run by CoopIII for a wreck another machine decided -
//    goes through the register with the rampage branch kept out. The machine
//    that decided it reports it and the relay counts it here.
//  - a wreck our own engine decided is counted by the engine and reported.
//  - a parked car, or a session car nobody is driving, can be decided on
//    several machines at once, each by its own copy of the same blast. Those
//    carry a key and each key counts once per frenzy, whichever way this
//    machine heard of it first.
//
// The occupants of a replayed wreck go through RegisterKillByPlayer at
// 0x0053BDB6 and 0x0053BE17, and they're kept off a pedestrian rampage the
// same way: the machine that decided the wreck already counted them.
enum class CarWreckTally : uint8_t {
	Engine,     // no shared frenzy here, the register runs as retail
	Withhold,   // the register runs with the rampage branch kept out of it
	Count,      // the engine counts it, and if it did the session hears
};

inline CarWreckTally TallyCarWreck(bool sharedFrenzy, bool replay, bool alreadyCounted) {
	if (!sharedFrenzy)
		return CarWreckTally::Engine;
	if (replay || alreadyCounted)
		return CarWreckTally::Withhold;
	return CarWreckTally::Count;
}

// The named cars this machine has counted in the current frenzy. Cleared when
// a frenzy starts. An unkeyed car is never in it: only one machine can decide
// one, so there's nothing to count twice.
struct CountedCars {
	static constexpr uint8_t CAPACITY = 64;

	UnownedVehicleKey keys[CAPACITY] = {};
	uint8_t           count = 0;
	uint8_t           next  = 0;

	bool Has(const UnownedVehicleKey &key) const {
		if (!RampageCarKeyed(key))
			return false;
		for (uint8_t i = 0; i < count; ++i)
			if (SameUnownedKey(keys[i], key))
				return true;
		return false;
	}

	// False when the key was already there, or is not a key.
	bool Add(const UnownedVehicleKey &key) {
		if (!RampageCarKeyed(key) || Has(key))
			return false;
		keys[next] = key;
		next       = static_cast<uint8_t>((next + 1) % CAPACITY);
		if (count < CAPACITY)
			++count;
		return true;
	}

	void Clear() {
		count = 0;
		next  = 0;
	}
};

// A car off the wire moves this machine's counter only if a frenzy is running
// here, the engine's own test takes the model, and it's not a named car this
// machine already counted.
inline bool CarFromWireCounts(bool ongoing, const FrenzyTargets &f, int32_t model,
                              bool alreadyCounted) {
	return ongoing && !alreadyCounted && RampageCarCounts(f, model);
}

// ---- what the seam reports outwards ---------------------------------------
//
// Function pointers rather than a queue, and for game/pickup.cpp's reason:
// two of the three are produced from inside a detour, which has nowhere to
// keep state and no frame to wait for. `Started` in particular has to go out
// while the script is still inside its own `init_rampage` opcode.
struct RampageCallbacks {
	// Our own script just started a frenzy. Sent by every machine; the server
	// keeps the first and drops the rest, because they are the same numbers
	// from the same script.
	void (*Started)(const RampageStartBody &body) = nullptr;

	// Our own engine just counted a kill. `frenzyId` is left for Client to
	// fill in - the seam does not know the session's numbering and has no
	// business inventing one.
	void (*Kill)(uint16_t model, uint8_t weapon, bool headshot) = nullptr;

	// Our own engine just counted a car it decided to wreck. `key` is
	// RAMPAGE_CAR_UNKEYED unless the car is one several machines can decide.
	void (*CarDestroyed)(uint16_t model, const UnownedVehicleKey &key) = nullptr;

	// Our own CDarkel left ONGOING. One candidate verdict, not a decision.
	void (*Ended)(uint8_t outcome) = nullptr;

	// Wired is not connected, the same guard game/pickup.cpp takes: with no
	// session the detours pass straight through and a rampage is single
	// player's, which is what it has always been.
	bool (*HaveSession)() = nullptr;
};

void SetRampageCallbacks(const RampageCallbacks &callbacks);

// Four detours: CDarkel::StartFrenzy, CDarkel::RegisterKillByPlayer,
// CDarkel::RegisterCarBlownUpByPlayer and CDarkel::ReadStatus. Non-fatal as a
// group and individually - what each failure costs is logged at the install
// site.
bool InstallRampageHooks();
void RemoveRampageHooks();
bool RampageHooksInstalled();

// Adds this file's entries to the bridge ped.cpp builds.
void InstallRampageBridge(WorldBridge &b);

// ---- what arrives from the session ----------------------------------------
//
// These are the WorldBridge entries, declared here so the seam and the
// bridge cannot drift.

// The session's rule, out of S_Welcome's flags. Read in one place: the
// StartFrenzy detour, which under RAMPAGE_RULE_OFF stops reporting and the
// whole feature goes back to being per-machine.
void SetRampageRule(uint8_t rule);

// The session's frenzy is open: this many kills still wanted, and this long
// since it started on the server's clock. Two stores, into the engine's own
// CDarkel::KillsNeeded and CDarkel::TimeOfFrenzyStart, which is enough to put
// a machine that has just joined into a rampage that is already half over.
// Under the default rule, for the machine that started it, both are what the
// script already wrote and nothing happens.
void ApplyRampageOpen(uint16_t killsNeeded, uint32_t elapsedMs);

// One kill somebody else's engine counted. Judged here, against this
// machine's own CDarkel, with the engine's own test.
void CreditRampageKill(uint16_t model, uint8_t weapon, bool headshot);

// One car another machine decided to wreck and counted. Judged with the car
// register's own test; a named car this machine already counted is dropped.
void CreditRampageCar(uint16_t model, const UnownedVehicleKey &key);

// Not a bridge entry and not off the wire: the host of a pedestrian, having
// just applied somebody else's hit and watched its own engine kill the ped,
// putting that kill through the engine's own register.
//
// This is the hole. CPed::InflictDamage only reaches
// CDarkel::RegisterKillByPlayer when the damaging entity is FindPlayerPed()
// or FindPlayerVehicle() (0x004EAD1A), and a hit off the wire names the
// *replica* of the shooter, so the engine sends it to RegisterKillNotByPlayer
// instead - one statistic and no kill credit anywhere in the session.
// game/combat.cpp calls this from ApplyRemotePedDamage when the ped died.
//
// `piece` is the ePedPieceTypes the shooter's machine decided, and it is what
// the headshot bit is derived from - see the note at the definition.
void CreditRemotePedKill(void *ped, uint8_t weapon, uint8_t piece);

// The session's verdict. Released to the script; the engine's own CDarkel is
// not touched, because by the time this arrives it has either reached the
// same answer itself or is about to.
void ApplyRampageVerdict(uint8_t outcome);

// No session any more. The script sees its own engine again, which is single
// player behaving exactly as it always did.
void ResetRampage();

// For the log and for tools/clienttest's sake, not for behaviour.
struct RampageStats {
	uint32_t startsSeen   = 0;
	uint32_t killsSent    = 0;
	uint32_t killsTaken   = 0;
	uint32_t killsRefused = 0;   // arrived, did not match this frenzy
	uint32_t endsSent     = 0;
	uint32_t verdicts     = 0;
	uint32_t heldFrames   = 0;   // times the script was told ONGOING over an
	                             // ending its own engine had already reached
	uint32_t carsSent     = 0;
	uint32_t carsTaken    = 0;
	uint32_t carsRefused  = 0;   // arrived, and didn't count here
	uint32_t carsWithheld = 0;   // blew up here, counted by somebody else
	uint32_t killsWithheld = 0;  // occupants of a replayed wreck
};
const RampageStats &GetRampageStats();

} // namespace coopiii::game
