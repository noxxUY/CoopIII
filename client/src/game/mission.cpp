#include "mission.h"

#include "anyplace.h"
#include "carauthority.h"
#include "carremoval.h"
#include "chat.h"
#include "cutscene.h"
#include "cutsceneskip.h"
#include "effectshape.h"
#include "fuzzball.h"
#include "gates.h"
#include "getaway.h"
#include "leadcheck.h"
#include "missioncombat.h"
#include "missionworld.h"
#include "nearchar.h"
#include "object.h"
#include "outfit.h"
#include "pause.h"
#include "ped.h"
#include "pedanim.h"
#include "pickup.h"
#include "population.h"
#include "rampagevote.h"
#include "radar.h"
#include "replay.h"
#include "seat.h"
#include "seatplan.h"
#include "sidejob.h"
#include "standapart.h"
#include "standin.h"
#include "teardown.h"
#include "vehicle.h"

#include "../client.h"
#include "../clock.h"
#include "../hook/hook.h"
#include "../log.h"

#include <cstdio>
#include <cstring>
#include <initializer_list>

#include <windows.h>

namespace coopiii::game {

namespace {

using namespace scripts;

using RangeFn   = int8_t(__fastcall *)(void *script, void *edx, int32_t command);
using CollectFn = void(__thiscall *)(void *script, uint32_t *ip, int16_t count);
using PlayerFn  = void *(__cdecl *)();
using OneFn     = int8_t(__thiscall *)(void *script);
using GetPedFn  = void *(__cdecl *)(int32_t);

Client *g_client    = nullptr;
bool    g_installed = false;

// The mission this machine's script is running, as the session sees it.
struct OwnMission {
	bool     running = false;
	uint16_t number  = MISSION_NONE;
	bool     passed  = false;
	// Its MISSION_HAS_FINISHED has run: the engine has cleaned up after it,
	// and its script is on its way out. The mission ends when the script does.
	bool     finishing = false;
};
OwnMission g_own;

// A participant's death or arrest the owner's mission has not been failed
// for yet: its script was between its gosubs when the order came. The
// server orders a mission's failure once, so it is kept here and tried every
// frame until it takes or the mission is over (FailMission).
bool    g_failPending = false;
uint8_t g_failReason  = MISSION_FAIL_DIED;

// The cars the owner's mission made (CREATE_CAR) and has not deleted or given
// to the main script (DONT_REMOVE_CAR). When it fails, what of them nobody sits
// in goes from the street, and from everybody else's with it, so the next try
// does not start beside the last one's cars (the owner's run on 2026-09-24:
// the Kuruma of a failed Give Me Liberty stayed at the bridge, a wreck on the
// screen whose copy the engine never clears, and the retry put a second one
// beside it).
constexpr size_t MAX_MISSION_CARS = 32;
int32_t          g_missionCars[MAX_MISSION_CARS];
size_t           g_missionCarCount = 0;
// Since when each of them has been on nobody's session here, 0 while it is,
// and whether that it could not be put there has been said (KeepMissionCars).
uint32_t g_missionCarMissingMs[MAX_MISSION_CARS];
bool     g_missionCarRefusedSaid[MAX_MISSION_CARS];
// A car of the mission hosted by nobody this long is put on the session: the
// frames between a claim and its answer are not worth a second netId.
constexpr uint32_t MISSION_CAR_GRACE_MS = 500;
// How many of the mission's cars have been said made, this mission.
uint32_t g_missionCarsSaid = 0;
// A failed mission's cars, still to go. One the session still names as a car
// somebody claimed waits for the server to let go of it, which it does for
// every car the mission made that nobody sits in (server.h,
// ReleaseMissionCars); one still named after LEFT_CAR_WAIT_MS is somebody's.
struct LeftCar {
	int32_t  handle  = -1;
	uint32_t sinceMs = 0;
};
constexpr uint32_t LEFT_CAR_WAIT_MS = 10000;
LeftCar            g_leftCars[MAX_MISSION_CARS];
size_t             g_leftCarCount = 0;

// Who of the participants got out of the car the owner's mission wants its
// player in (mission.h, GetBackInWatch), and what the mission says to them.
GetBackInWatch g_getBackIn;
uint8_t        g_getBackInLabel[TEXT_LABEL] = {};

// The blue markers the owner's mission draws (mission.h, OwnMarkers), and on
// a participant the ones the owner's machine says are up.
OwnMarkers   g_ownMarkers;
ShownMarkers g_shownMarkers;
bool         g_saidMarkersFull = false;
// On a participant: the clothes the owner's mission put its player in, for
// ours to follow (game/outfit.h). Kept past the mission's end.
OutfitChange g_outfit;
bool         g_saidOutfitSent    = false;
bool         g_saidOutfitRefused = false;
// A seat it can't be made in has been said, for this change.
bool         g_saidOutfitSeat    = false;

// What the mission has up, as it went to everybody, for somebody who comes in
// later (mission.h, StandingEffects). Kept as it was recorded, with this
// machine's own handles, and named again when it is handed over.
StandingEffects g_standing;
bool            g_saidStandingFull = false;

// Each enemy of the mission that has copies, and its copies (missions.md 10.5).
EnemyGroups g_groups;

// The mission's floating packages on this machine (mission-audit.md R2): the
// owner's handle for each, this machine's own, and whether it has gone.
struct FloatingPackage {
	int32_t owner = -1;
	int32_t ours  = -1;
	bool    gone  = false;
};
constexpr size_t MAX_FLOATING   = 16;
FloatingPackage  g_floating[MAX_FLOATING];
size_t           g_floatingCount = 0;

// The mission's LOAD_ALL_MODELS_NOW, numbered from 1, for everybody to say
// they have run it too (protocol.h, C_MissionReady), and whether the wait on
// the last one has been said.
uint16_t g_readySeq      = 0;
bool     g_saidReadyWait = false;

uint32_t   g_grantedKey = 0;   // the gate the session let through last
// The trigger between the session's grant and its START_MISSION: its fade and
// the mission's title are the first things everybody sees of it.
void      *g_launching  = nullptr;
uint16_t   g_launchHint = MISSION_NONE;

// This machine's player is kept waiting for the session: at a start gate the
// session holds, or out of its own mission into somebody else's. The intro
// ends faded out to the loading screen and leaves it to Give Me Liberty to
// fade back in, so without a word from us the wait is spent on that screen.
bool g_showWorld = false;

// On a participant: which of this machine's blips and cutscene objects
// stands for which of the owner's (game/replay.h).
replay::BlipMap g_blipMap;
replay::BlipMap g_objectMap;
replay::BlipMap g_pickupMap;
replay::BlipMap g_fireMap;
replay::BlipMap g_sphereMap;

// On the owner: what of its mission's instructions goes out, and when
// (game/effectshape.h), and the coronas it draws every frame. On a
// participant: the owner's coronas this machine draws.
shape::EffectShaper  g_shaper;
shape::BlipAliases   g_aliases;
shape::OwnCoronas    g_ownCoronas;
shape::ShownCoronas  g_shownCoronas;
bool                 g_saidCoronasFull = false;

// On a participant: what the streets were before the owner's mission changed
// them here (game/missionworld.h), for when it ends without putting them back.
// The densities are nobody's save and always go back; the zones and the gangs
// are the campaign's, and go back only when the mission's own cleanup never
// reached this machine.
struct WorldBefore {
	bool    pedDensity = false, carDensity = false;
	float   ped = 1.0f, car = 1.0f;
	bool    zones = false;
	uint8_t zoneInfo[world::ZONE_INFO_BYTES];
	bool    gang[world::GANG_COUNT] = {};
	int32_t gangWeapons[world::GANG_COUNT][2] = {};
	bool     threat[world::PED_TYPES] = {};
	uint32_t threats[world::PED_TYPES] = {};
};
WorldBefore g_before;

// On the owner: the blips its mission made through the blip map and has not
// taken off again. A blip instruction naming a global that holds none of
// them names one the main script keeps there, a contact's marker, and goes by
// the global (replay.h, Encode's `blipByGlobal`): no participant's map has it.
constexpr size_t MAX_MADE_BLIPS = 64;
int32_t          g_madeBlips[MAX_MADE_BLIPS];
size_t           g_madeBlipCount = 0;

bool MadeBlip(int32_t handle) {
	for (size_t i = 0; i < g_madeBlipCount; ++i)
		if (g_madeBlips[i] == handle)
			return true;
	return false;
}

void NoteMadeBlip(int32_t handle) {
	if (MadeBlip(handle))
		return;
	if (g_madeBlipCount == MAX_MADE_BLIPS) {   // the oldest goes; the radar has 32
		for (size_t i = 1; i < g_madeBlipCount; ++i)
			g_madeBlips[i - 1] = g_madeBlips[i];
		--g_madeBlipCount;
	}
	g_madeBlips[g_madeBlipCount++] = handle;
}

void ForgetMadeBlip(int32_t handle) {
	for (size_t i = 0; i < g_madeBlipCount; ++i)
		if (g_madeBlips[i] == handle) {
			g_madeBlips[i] = g_madeBlips[--g_madeBlipCount];
			return;
		}
}

// On the owner, the same for the pickups its mission laid out through the
// pickup map. A REMOVE_PICKUP naming one that is none of them names one the
// main script keeps in a global, the out-of-stock sign Cipriani's Chauffeur
// takes down, or one the mission made there for the world to keep, and goes
// by the global (replay.h, `pickupByGlobal`).
constexpr size_t MAX_MADE_PICKUPS = 64;
int32_t          g_madePickups[MAX_MADE_PICKUPS];
size_t           g_madePickupCount = 0;

bool MadePickup(int32_t handle) {
	for (size_t i = 0; i < g_madePickupCount; ++i)
		if (g_madePickups[i] == handle)
			return true;
	return false;
}

void NoteMadePickup(int32_t handle) {
	if (MadePickup(handle) || g_madePickupCount == MAX_MADE_PICKUPS)
		return;
	g_madePickups[g_madePickupCount++] = handle;
}

void ForgetMadePickup(int32_t handle) {
	for (size_t i = 0; i < g_madePickupCount; ++i)
		if (g_madePickups[i] == handle) {
			g_madePickups[i] = g_madePickups[--g_madePickupCount];
			return;
		}
}

// A blip handle that names a blip on this machine's radar now: its slot in
// use, and still the one the handle was handed out for (the counter in its
// top half, GetActualBlipArrayIndex's test, addresses.h).
bool LiveBlip(int32_t handle) {
	const uint32_t slot = static_cast<uint32_t>(handle) & 0xFFFF;
	if (handle == -1 || slot >= NUM_RADAR_BLIPS)
		return false;
	const uint8_t *trace = Ptr<uint8_t>(CRadar__ms_RadarTrace) + slot * SIZEOF_RADAR_TRACE;
	uint16_t       index = 0;
	std::memcpy(&index, trace + TRACE_BLIP_INDEX, 2);
	return trace[TRACE_IN_USE] != 0 && index == (static_cast<uint32_t>(handle) >> 16);
}

// The objects the owner's mission made on this machine, each in the global
// the mission made it into: let go of when the mission ends, the way the
// mission's own cleanup lets go of its own on the owner's machine.
constexpr size_t MAX_MADE_OBJECTS = 48;
int32_t          g_madeObjects[MAX_MADE_OBJECTS];
uint16_t         g_madeObjectGlobals[MAX_MADE_OBJECTS];   // the global each went into
size_t           g_madeObjectCount = 0;

// The session's mission's stash on this machine (protocol.h, PICKUP_F_STASH):
// the weapons, armour, health and cash it laid out, which every player takes
// their own of. `ours` is this machine's handle, the owner's own on the
// owner's machine. The mission taking one away again is put off until the
// mission ends, so nobody's copy goes before they have had it.
struct StashPickup {
	int32_t owner = -1;
	int32_t ours  = -1;
	float   x = 0.0f, y = 0.0f;
	int16_t model = -1;   // -1: money, whose model is the engine's to pick
};
constexpr size_t MAX_STASH = 32;
StashPickup      g_stash[MAX_STASH];
size_t           g_stashCount = 0;

// Defined further down, where their neighbours are.
void   RunOurs(uint16_t opcode, std::initializer_list<int32_t> values);
bool   AskHere(uint16_t opcode, std::initializer_list<int32_t> values);
void   FlushShape(uint32_t nowMs, bool say);
int8_t Record(void *script, int32_t command, int8_t(__fastcall *original)(void *, void *, int32_t));
void   ClearStash();
void   ReleaseCopies(const int32_t *copies, size_t n);
bool   RunHere(const uint8_t *code, size_t length, int32_t *local0);
void   TickOutfit();
void   ForgetSeatOrders();
int8_t PassengerOrder(void *script, int32_t command, RangeFn original);
int8_t LeaderOrder(void *script, int32_t command, RangeFn original);
bool   InGetaway(void *script);
bool   GetawayCrewPrint(int32_t command, const uint8_t *code, size_t length);
bool   GetawayCrewBlip(int32_t handle, MissionEffectBody &body);
bool   GetawayBlipTarget(int32_t handle, int32_t command, MissionEffectBody &body);
void   ForgetGetaway();
bool   GetawayPlace(void *script, int32_t command, uint16_t andOr, bool notFlag, uint8_t before);
int8_t GetawayLeader(void *script, int32_t command, RangeFn original);

// What the owner's mission has done to a participant's screen, camera and
// controls, through the replay: whatever of it is still on when the session's
// mission ends, the owner gone mid-cutscene say, is put back (EndEffects).
struct Shown {
	bool     cutscene      = false;   // loaded or running
	bool     sceneSkipped  = false;   // one not loaded here (CutsceneModelsHere)
	bool     controlOff    = false;
	bool     widescreen    = false;
	bool     flashing      = false;   // a HUD item, FLASH_HUD_OBJECT
	bool     fadedOut      = false;
	bool     fixedCamera   = false;
	bool     invisible     = false;
	bool     ignoredByAll  = false;
	bool     ignoredByCops = false;
	bool     brakes        = false;
	bool     freeBombs     = false;
	bool     restartMoved  = false;
	bool     missionAudio  = false;   // a line loaded, or playing
	bool     credits       = false;
	uint16_t timer         = 0;       // the globals the HUD's widgets read
	uint16_t counter       = 0;
	// What the mission sets for a while (replay.h), each given back at the end.
	bool     madeSafe      = false;   // the launch's make-safe, no cutscene since
	bool     freeResprays  = false;
	bool     crimeEye      = false;   // SET_WANTED_MULTIPLIER, put back to 1.0
	bool     hospital      = false;   // a restart level overridden
	bool     policeStation = false;
	bool     worldHeld     = false;   // SWITCH_WORLD_PROCESSING 0
	bool     carsUnhurt    = false;   // SET_ALL_CARS_CAN_BE_DAMAGED 0
	bool     carsAtCamera  = false;   // SET_GENERATE_CARS_AROUND_CAMERA 1
	bool     nearClip      = false;   // one other than NEAR_CLIP_AFTER_MISSION
	bool     musicNoFade   = false;   // SET_MUSIC_DOES_FADE 0
	bool     endTune       = false;
	// The globals the mission's continuous sounds went into here.
	uint8_t  soundCount    = 0;
	uint16_t sounds[8]     = {};
	// The garages a replayed SET_TARGET_CAR_FOR_MISSION_GARAGE pointed at one
	// of our cars, one bit each: let go of at the end, as the owner's
	// cleanup does, whatever way the mission ended.
	uint32_t garageTargets = 0;
	// The Exchange's Catalina helicopter, flown here from the owner's start
	// (replay.h, WIRE_CATALINA_HELI), and a take-off or fly-away of the
	// owner's held until our copy is in its slot: both handlers write through
	// the slot with no null test.
	bool     catalina      = false;
	uint16_t catalinaPath  = 0;
	// The owner's power pills, written into our own CPacManPickups (replay.h,
	// PillCode) and never collected here.
	bool     pills         = false;
};
Shown g_shown;

// The mission Cessnas a participant's own engine is flying for the owner's
// mission (MISSION_SHOT_DOWN_*): set when the owner's start of one runs here,
// so a plane left down from an earlier try is never taken for this one's.
// And The Exchange's Catalina, the same way.
uint16_t g_planesWatched = 0;

// ---- The Exchange's Catalina helicopter (mission-audit.md R7) ----------------------
//
// Verified against the retail image. The four instructions have no operands
// and each handler is one call: START_CATALINA_HELI into 0x0054A980, which
// sets CatalinaHeliOn (0x0095CD85) and clears CatalinaHasBeenShotDown
// (0x0095CD56); CATALINA_HELI_TAKE_OFF into 0x0054A9B0, `mov eax,[0072CF5Ch] /
// mov byte [eax+2CAh],8`; CATALINA_HELI_FLY_AWAY into 0x0054A9C0, the same
// with 0Ch; REMOVE_CATALINA_HELI into 0x0054A9D0, which takes the slot's
// helicopter out of the world and deletes it. HAS_CATALINA_HELI_BEEN_SHOT_DOWN
// reads 0x0095CD56 (0x0054AA10), which UpdateHelis sets as it blows the slot's
// helicopter up. The slot is CHeli::pHelis[3], 0x0072CF5C.
constexpr int32_t   OP_START_CATALINA_HELI            = 0x03B2;
constexpr int32_t   OP_CATALINA_HELI_TAKE_OFF         = 0x03B3;
constexpr int32_t   OP_REMOVE_CATALINA_HELI           = 0x03B4;
constexpr int32_t   OP_HAS_CATALINA_HELI_BEEN_SHOT_DOWN = 0x03B5;
constexpr int32_t   OP_CATALINA_HELI_FLY_AWAY         = 0x03BE;
constexpr int32_t   OP_REMOVE_ALL_SCRIPT_FIRES        = 0x031A;
constexpr uint16_t  OP_ADD_BLIP_FOR_PICKUP            = 0x03DC;
constexpr uint16_t  OP_ADD_SPRITE_BLIP_FOR_PICKUP     = 0x03DD;
constexpr uintptr_t CATALINA_SLOT                     = CHeli__pHelis + 3 * 4;
constexpr uintptr_t CHeli__CatalinaHeliOn             = 0x0095CD85;

// Our own Catalina, in its slot, or null.
void *CatalinaHere() {
	void *const h = Global<void *>(CATALINA_SLOT);
	return h && Field<uintptr_t>(h, offs::VTABLE) == CHeli__vtable ? h : nullptr;
}

int32_t CatalinaRef() {
	void *const h = CatalinaHere();
	return h ? Func<int32_t(__cdecl *)(void *)>(CPools__GetVehicleRef)(h) : -1;
}

// ---- the power pills (replay.h, PillCode) --------------------------------------------

constexpr uintptr_t PM_PICKUPS        = 0x00731618;   // CPacManPickups::aPMPickUps
constexpr size_t    PM_PICKUP_SIZE    = 0x14;
constexpr size_t    PM_PICKUP_OBJECT  = 0x0C;
constexpr size_t    PM_PICKUP_TYPE    = 0x10;
constexpr uintptr_t PM_ACTIVE         = 0x0095CD6F;   // CPacManPickups::bPMActive
constexpr int32_t   OP_CLEAR_PACMAN   = 0x02C6;
constexpr uintptr_t PM_PICKUP_UPDATE      = 0x004331B0;   // CPacManPickup::Update
constexpr uintptr_t PM_PICKUP_UPDATE_CALL = 0x00432AA0;   // its one caller

// What the owner last told of each slot of its own table.
replay::PillRow g_sentPills[replay::PILL_SLOTS];
bool            g_pillCallRedirected = false;
void            ForgetSentPills();

// The owner's own widgets, whose values go to everybody (protocol.h,
// C_MissionWidget).
uint16_t g_ownTimer       = 0;
uint16_t g_ownCounter     = 0;
bool     g_ownTimerFrozen = false;

// The mission's enemies made tougher already (Toughen), each only once.
constexpr size_t MAX_TOUGHENED = 64;
int32_t          g_toughened[MAX_TOUGHENED];
size_t           g_toughenedCount = 0;

// What the owner's mission has written into main.scm's globals, with what
// each held before the first write, and the main-script threads it has
// started (mission.h, "what a mission leaves behind").
constexpr size_t            MAX_TRACKED = 512;
CampaignWrites<MAX_TRACKED> g_writes;
bool                        g_saidWritesFull = false;
struct Started {
	int32_t   label  = 0;
	uintptr_t script = 0;
};
Started g_started[CAMPAIGN_THREADS];
size_t  g_startedCount = 0;

// The instructions of the owner's mission whose effect the world keeps, in
// the order they ran (replay.h, Kind::World).
struct WorldOp {
	uint8_t length = 0;
	uint8_t code[MISSION_EFFECT_CODE];
};
constexpr size_t MAX_WORLD_OPS = 64;
WorldOp          g_worldOps[MAX_WORLD_OPS];
bool             g_saidWorldOpsFull = false;
size_t           g_worldOpCount = 0;

// The last area each thread passed a location check in, for a trigger's start
// gate: the marker is checked a few instructions before 03EE. Keyed by the
// script's address, and small, since only a handful of triggers run at once.
struct LastArea {
	uintptr_t   script = 0;
	MissionArea area   = {};
	uint32_t    frame  = 0;
};
constexpr size_t LAST_AREAS = 32;
LastArea         g_lastAreas[LAST_AREAS];
uint32_t         g_frame = 0;

// The mission script's coordinate blips.
constexpr size_t MAX_COORD_BLIPS = 32;
CoordBlip        g_blips[MAX_COORD_BLIPS];
size_t           g_blipCount = 0;

bool g_mirror = false;

// True while the session's mission, on this machine that owns it, runs one of
// its instructions: what that instruction puts into the world is the
// mission's, and population.cpp hosts it for everybody (docs/missions.md 5.3).
bool g_missionInstruction = false;

bool g_saidGate = false, g_saidCheckpoint = false, g_saidEffect = false, g_saidDropped = false;
bool g_saidNotHost = false;
bool g_saidRiderStart = false;
bool g_saidSequel     = false;

// ---- the script's memory ------------------------------------------------------

uint8_t *Space() { return Ptr<uint8_t>(SCRIPT_SPACE); }

struct Vec3f {
	float x, y, z;
};
// CWorld::GetIsLineOfSightClear (addresses.h). A bool: every exit sets al
// alone (`xor al,al` or `mov al,1`), so the rest of eax is whatever was there.
using LineOfSightFn = bool(__cdecl *)(const Vec3f *, const Vec3f *, int, int, int, int, int, int,
                                      int);

template <class T>
T &At(void *script, size_t offset) {
	return Field<T>(script, offset);
}

bool IsMissionScript(void *script) { return IsMissionSlotScript(script); }

float *Params() { return Ptr<float>(CTheScripts__ScriptParams); }

const int32_t *Locals(void *script) { return &At<int32_t>(script, layout::SCRIPT_LOCALS); }

// Where $ONMISSION is, once two witnesses agree on it: the engine's
// OnAMissionFlag (plugin-sdk's address, missionaddr.h) and main.scm's own
// DECLARE_MISSION_FLAG (0180), found in the script. 0 until both have been
// read and agree, and for good once they disagree: a wrong offset would write
// into some other global of the campaign.
uint32_t g_onMissionAt   = 0;
bool     g_onMissionBad  = false;

uint32_t DeclaredMissionFlag();

int32_t *OnMissionVar() {
	if (g_onMissionAt == 0 && !g_onMissionBad) {
		const uint32_t used = Global<uint32_t>(ON_A_MISSION_FLAG);
		if (used == 0)
			return nullptr;   // the main script has not declared it yet
		const uint32_t named = DeclaredMissionFlag();
		if (named != 0 && named == used && used + 4 <= MAIN_SCRIPT_SIZE) {
			g_onMissionAt = used;
			Log("missions: $ONMISSION is at %u in the script space, where main.scm's "
			    "DECLARE_MISSION_FLAG says", used);
		} else {
			g_onMissionBad = true;
			Log("missions: the engine says $ONMISSION is at %u and main.scm says %u - a "
			    "participant's $ONMISSION is left alone", used, named);
		}
	}
	return g_onMissionAt ? reinterpret_cast<int32_t *>(Space() + g_onMissionAt) : nullptr;
}

// Reads `count` operands at the script's instruction pointer without moving
// it: CollectParameters on a copy. They land in ScriptParams.
void PeekParams(void *script, int16_t count) {
	uint32_t ip = At<uint32_t>(script, layout::SCRIPT_IP);
	Func<CollectFn>(CTheScripts__CollectParameters)(script, &ip, count);
}

Vec3 LocalPlayerPos() {
	void *ped = Func<PlayerFn>(FindPlayerPed)();
	if (!ped)
		return {};
	const float *p = &Field<float>(ped, offs::POSITION);
	return {p[0], p[1], p[2]};
}

uint8_t LocalId() { return g_client ? g_client->LocalPlayerId() : INVALID_PLAYER; }

// ---- the remembered areas and blips ---------------------------------------------

void RememberArea(void *script, const MissionArea &area) {
	const uintptr_t key    = reinterpret_cast<uintptr_t>(script);
	LastArea       *oldest = &g_lastAreas[0];
	for (LastArea &a : g_lastAreas) {
		if (a.script == key) {
			a.area  = area;
			a.frame = g_frame;
			return;
		}
		if (a.frame < oldest->frame)
			oldest = &a;
	}
	oldest->script = key;
	oldest->area   = area;
	oldest->frame  = g_frame;
}

bool LastAreaOf(void *script, MissionArea *out) {
	const uintptr_t key = reinterpret_cast<uintptr_t>(script);
	for (const LastArea &a : g_lastAreas)
		if (a.script == key && g_frame - a.frame < 600) {   // ten seconds at 60 fps
			*out = a.area;
			return true;
		}
	return false;
}

void AddBlip(int32_t handle, float x, float y) {
	for (size_t i = 0; i < g_blipCount; ++i)
		if (g_blips[i].handle == handle) {
			g_blips[i] = {handle, x, y};
			return;
		}
	if (g_blipCount < MAX_COORD_BLIPS)
		g_blips[g_blipCount++] = {handle, x, y};
}

void DropBlip(int32_t handle) {
	for (size_t i = 0; i < g_blipCount; ++i)
		if (g_blips[i].handle == handle) {
			g_blips[i] = g_blips[--g_blipCount];
			return;
		}
}

// ---- a scene the script frames (mission.h, IsScriptedScene) --------------------------

bool    g_cutsceneScene = false;   // one of the mission's cutscenes plays here
bool    g_sceneHidden   = false;   // the other players are hidden for a scene
void   *g_safePed       = nullptr; // our player, made proof for the scene
uint8_t g_safeB = 0, g_safeC = 0;  // and the proofs this set on it, to take back

// The proofs go back on the ped they were put on, and only the ones this
// put there: whatever the mission set itself stays.
void TakeSafetyBack() {
	if (g_safePed && g_safePed == Func<PlayerFn>(FindPlayerPed)()) {
		Field<uint8_t>(g_safePed, offs::ENTITY_FLAGS_B) &= static_cast<uint8_t>(~g_safeB);
		Field<uint8_t>(g_safePed, offs::ENTITY_FLAGS_C) &= static_cast<uint8_t>(~g_safeC);
	}
	g_safePed = nullptr;
	g_safeB = g_safeC = 0;
}

// Once a frame, and whenever a cutscene starts or ends here. Widescreen
// counts only inside the session's mission, owned or helped: outside one the
// bars are this game's own business.
void UpdateScene() {
	const bool inMission =
	    g_client && (g_own.running || g_client->Missions().ParticipantHere(LocalId()));
	const bool widescreen = inMission && Global<uint8_t>(TheCamera + CAMERA_WIDESCREEN_ON) != 0;
	const bool hidden     = IsScriptedScene(widescreen, g_cutsceneScene);
	if (hidden != g_sceneHidden) {
		g_sceneHidden = hidden;
		SetRemotePlayersHidden(hidden);
	}
	// A participant stands frozen where the scene found it, with nobody's
	// script watching over it: nothing may hurt it until the scene is over.
	// The owner's player is its own script's, as in single player.
	void *const me   = Func<PlayerFn>(FindPlayerPed)();
	const bool  safe = hidden && inMission && !g_own.running && me;
	if (safe && g_safePed != me) {
		TakeSafetyBack();
		uint8_t &b = Field<uint8_t>(me, offs::ENTITY_FLAGS_B);
		uint8_t &c = Field<uint8_t>(me, offs::ENTITY_FLAGS_C);
		g_safeB    = static_cast<uint8_t>(offs::ENTITY_EXPLOSION_PROOF & ~b);
		g_safeC    = static_cast<uint8_t>((offs::ENTITY_BULLET_PROOF | offs::ENTITY_FIRE_PROOF |
		                                   offs::ENTITY_COLLISION_PROOF | offs::ENTITY_MELEE_PROOF) &
		                                  ~c);
		b |= g_safeB;
		c |= g_safeC;
		g_safePed = me;
		Log("missions: a scene of the owner's mission plays here; our player can't be hurt "
		    "until it's over");
	} else if (!safe && g_safePed) {
		TakeSafetyBack();
	}
}

void SetCutsceneScene(bool on) {
	g_cutsceneScene = on;
	UpdateScene();
}

// ---- what each intercepted opcode does ----------------------------------------------

// A trigger whose 03EE is answered no waits for its player to walk out of the
// marker before it asks again (the `goto_if_false` branch of every contact's
// trigger loops on the locate until it fails). So while its player is still
// in the marker it is held at the question instead, and asks it again next
// frame: the instruction pointer goes back to the 03EE and the handler says
// "no more this frame", as WAIT does. Once everybody is there it launches
// with nobody having to step out and back in.
int8_t StartGate(void *script, int32_t command, RangeFn original) {
	const uint32_t key      = At<uint32_t>(script, layout::SCRIPT_IP);
	const uint16_t andOr    = At<uint16_t>(script, layout::SCRIPT_AND_OR);
	const bool     mayForce = MayForceCondition(andOr, At<uint8_t>(script, layout::SCRIPT_NOT) != 0);
	const int8_t   r        = original(script, nullptr, command);
	if (!g_client || !mayForce || At<uint8_t>(script, layout::SCRIPT_COND_RESULT) == 0)
		return r;

	// A save point asks the same question, and nothing launches after it.
	// Only a gate this recognises as a launch is held: a main.scm that is not
	// the stock one may ask it anywhere, and a start that is not held is
	// what the game did anyway, while a save point held by mistake is not.
	if (ClassifyStartGate(Space(), MAIN_SCRIPT_SIZE, key) != GateKind::Launch)
		return r;
	const uint16_t hint = FindLaunchAhead(Space(), MAIN_SCRIPT_SIZE, key);
	MissionArea area{};
	const bool  marker = LastAreaOf(script, &area);
	if (!marker)
		area = MissionAreaAround(LocalPlayerPos());

	// The session's campaign is the host's save, not this one: whatever this
	// save has open is the host's to start, or nobody's. Answered no and not
	// held, so the trigger waits for its player to walk out as it does for
	// any no.
	if (g_client->Missions().FollowsHostsCampaign(LocalId())) {
		if (!g_saidNotHost) {
			g_saidNotHost = true;
			Log("missions: %s's start in this game's own save is not the session's: the host's "
			    "campaign is",
			    MissionName(hint));
		}
		At<uint8_t>(script, layout::SCRIPT_COND_RESULT) = 0;
		return r;
	}

	if (g_client->Missions().AskStartGate(key, MISSION_KIND_STORY, hint, area, LocalId(),
	                                      WallClock::NowMs())) {
		g_grantedKey  = key;
		g_launching   = script;
		g_launchHint  = hint;
		return r;
	}
	if (!g_saidGate) {
		g_saidGate = true;
		Log("missions: %s's start is held for the session", MissionName(hint));
	}
	if (g_client->Missions().StartHeld(key, WallClock::NowMs()))
		g_showWorld = true;
	// Held only while the start waits for players. Somebody else's mission
	// running is the trigger's to find out again, from $ONMISSION. Our own
	// still running is only the server's word on it not being here yet: a
	// mission that ends with our player in the next one's marker asks before
	// the session has heard the end, and let go then, the trigger would wait
	// for him to walk out and back in.
	const MissionSync &ms         = g_client->Missions();
	const Vec3         here       = LocalPlayerPos();
	const bool         somebodyIn = InMissionArea(here, area, 0.5f) ||
	                        ms.OtherPlayerAtOurContact(area, here, LocalId());
	if (StartGateHolds(marker, andOr == ANDOR_NONE, somebodyIn, ms.Running(),
	                   ms.Owner() == LocalId())) {
		// The locate is not run again while the gate is held, and the area it
		// left is what the claim and this hold go by: kept fresh, or a start
		// that waits longer than LastAreaOf remembers would be let go, and
		// the trigger would wait for its player to walk out and back in.
		RememberArea(script, area);
		At<uint32_t>(script, layout::SCRIPT_IP) = key - 2;   // back to the opcode
		return 1;
	}
	At<uint8_t>(script, layout::SCRIPT_COND_RESULT) = 0;
	return r;
}

// A launch that did not ask CAN_PLAYER_START_MISSION first: the odd jobs'
// button, the RC van's spot, the 4x4 and Mayhem cars (mission-audit.md C1).
// Its gate is START_MISSION itself, asked again every frame until the session
// grants it, with the area its trigger last passed (the RC van's spot) or the
// owner (the rest, whose car is the start). Given up, and the trigger left to
// go round again, when the player gets out of the car the launch was for or
// drives off the spot, or when somebody else holds the session's mission.
enum class LaunchVerdict { Go, Hold, GiveUp };

LaunchVerdict LaunchGate(void *script, int32_t number) {
	if (script == g_launching || IsMissionScript(script) || number <= 2 || number >= MISSION_COUNT)
		return LaunchVerdict::Go;
	const uint8_t  kind = MissionKindOf(static_cast<uint16_t>(number));
	const uint32_t key  = At<uint32_t>(script, layout::SCRIPT_IP);
	// A story mission this game's own save would start, while the session's
	// campaign is the host's (StartGate).
	if (kind == MISSION_KIND_STORY && g_client->Missions().FollowsHostsCampaign(LocalId()))
		return LaunchVerdict::GiveUp;
	if (kind != MISSION_KIND_STORY) {
		// A vehicle's start is its driver's (game/sidejob.h).
		const bool riding = LocalIsPassenger();
		switch (VehicleStartGate(Func<PlayerFn>(FindPlayerVehicle)() != nullptr, riding,
		                         g_client->Missions().Running())) {
		case VehicleStart::GiveUp:
			return LaunchVerdict::GiveUp;
		case VehicleStart::Hold:
			if (!g_saidRiderStart) {
				g_saidRiderStart = true;
				Log("missions: %s would start from our passenger seat; it is the driver's, so "
				    "it waits unclaimed until we are out",
				    MissionName(static_cast<uint16_t>(number)));
			}
			return LaunchVerdict::Hold;
		case VehicleStart::Ask:
			break;
		}
	}
	MissionArea    area{};
	const bool     spot = kind == MISSION_KIND_RC && LastAreaOf(script, &area);
	if (!spot)
		area = MissionAreaAround(LocalPlayerPos());
	if (g_client->Missions().AskStartGate(key, kind, static_cast<uint16_t>(number), area, LocalId(),
	                                      WallClock::NowMs())) {
		g_grantedKey = key;
		g_launchHint = static_cast<uint16_t>(number);
		return LaunchVerdict::Go;
	}
	const bool inCar  = Func<PlayerFn>(FindPlayerVehicle)() != nullptr;
	const bool onSpot = !spot || InMissionArea(LocalPlayerPos(), area, 0.5f);
	if ((kind != MISSION_KIND_STORY && (!inCar || !onSpot)) || g_client->Missions().Running())
		return LaunchVerdict::GiveUp;
	if (!g_saidGate) {
		g_saidGate = true;
		Log("missions: %s's start is held for the session", MissionName(static_cast<uint16_t>(number)));
	}
	return LaunchVerdict::Hold;
}

int8_t Launch(void *script, int32_t command, RangeFn original) {
	PeekParams(script, 1);
	const int32_t number = *reinterpret_cast<const int32_t *>(Params());
	if (g_client && g_client->Missions().Shared()) {
		const uint32_t ip = At<uint32_t>(script, layout::SCRIPT_IP);
		switch (LaunchGate(script, number)) {
		case LaunchVerdict::Hold:
			At<uint32_t>(script, layout::SCRIPT_IP) = ip - 2;   // START_MISSION again next frame
			return 1;
		case LaunchVerdict::GiveUp: {
			// Past the instruction, as though the trigger had not got this
			// far, and round its loop again.
			uint32_t past = ip;
			Func<CollectFn>(CTheScripts__CollectParameters)(script, &past, 1);
			// And the odd job's "begun" flag after it (game/sidejob.h).
			const uint32_t sequel = GivenUpSequelLength(Space(), SCRIPT_SPACE_SIZE, past, number);
			if (sequel != 0) {
				past += sequel;
				if (!g_saidSequel) {
					g_saidSequel = true;
					Log("missions: %s's start was given up; the trigger's flag after it is "
					    "skipped too, so it can offer the job again",
					    MissionName(static_cast<uint16_t>(number)));
				}
			}
			At<uint32_t>(script, layout::SCRIPT_IP) = past;
			return 0;
		}
		case LaunchVerdict::Go:
			break;
		}
	}
	const int8_t  r      = original(script, nullptr, command);
	// The intro and the two info scenes are every machine's own (missions.md 13).
	if (!g_client || number <= 2 || number >= MISSION_COUNT)
		return r;
	// What the trigger showed went as it was; the mission's own starts afresh.
	FlushShape(WallClock::NowMs(), false);
	g_own          = OwnMission{true, static_cast<uint16_t>(number), false, false};
	g_failPending  = false;   // the last one's, which is over
	g_blipCount    = 0;
	g_missionCarCount = 0;
	g_missionCarsSaid = 0;
	g_getBackIn.Clear();
	ForgetSeatOrders();
	g_ownMarkers.Clear();
	GetBackInLabelOf(Space() + MAIN_SCRIPT_SIZE, SCRIPT_SPACE_SIZE - MAIN_SCRIPT_SIZE, g_getBackInLabel);
	g_launching    = nullptr;
	g_writes.Clear();
	g_saidWritesFull = false;
	g_startedCount = 0;
	g_worldOpCount = 0;
	g_saidWorldOpsFull = false;
	g_toughenedCount = 0;
	g_ownTimer       = 0;
	g_ownCounter     = 0;
	g_ownTimerFrozen = false;
	g_standing.Clear();
	g_saidStandingFull = false;
	g_readySeq         = 0;
	g_saidReadyWait    = false;
	g_groups.Clear();
	g_floatingCount    = 0;
	ForgetSentPills();
	g_client->Missions().Launched(g_grantedKey, static_cast<uint16_t>(number), WallClock::NowMs());
	return r;
}

bool ScriptStillRunning(uintptr_t script) {
	for (uintptr_t s = Global<uintptr_t>(ACTIVE_SCRIPTS); s != 0;
	     s = *reinterpret_cast<uintptr_t *>(s + layout::SCRIPT_NEXT))
		if (s == script)
			return true;
	return false;
}

bool ThreadNamed(const char (&name)[8]) {
	for (uintptr_t s = Global<uintptr_t>(ACTIVE_SCRIPTS); s != 0;
	     s = *reinterpret_cast<uintptr_t *>(s + layout::SCRIPT_NEXT))
		if (std::strncmp(reinterpret_cast<const char *>(s + layout::SCRIPT_NAME), name, 8) == 0)
			return true;
	return false;
}

// main.scm's code, the same on every machine that may share a campaign.
uint32_t ScriptHash() {
	static uint32_t hash = 0;
	if (hash == 0)
		hash = ScriptCodeHash(Space(), MAIN_SCRIPT_SIZE);
	return hash;
}

// What the owner's mission leaves behind, sent as it ends and before the end
// is: the server takes it only from the running mission's owner. A thread
// that will never name itself is named after its label here, so a delta that
// comes back to this machine, after a load, finds it.
void SendCampaignDelta() {
	CampaignThread threads[CAMPAIGN_THREADS];
	size_t         threadCount = 0;
	for (size_t i = 0; i < g_startedCount; ++i) {
		if (!ScriptStillRunning(g_started[i].script))
			continue;
		CampaignThread &t = threads[threadCount++];
		t                 = CampaignThread{};
		t.label           = g_started[i].label;
		if (!ThreadNameAt(Space(), MAIN_SCRIPT_SIZE, t.label, t.name)) {
			char tag[8];
			ThreadTag(t.label, tag);
			std::memcpy(reinterpret_cast<char *>(g_started[i].script + layout::SCRIPT_NAME), tag, 8);
		}
	}
	for (size_t i = 0; i < g_worldOpCount; ++i)
		g_client->Missions().SendCampaignDelta(
		    CampaignOpPart(g_own.number, g_worldOps[i].code, g_worldOps[i].length, ScriptHash()),
		    WallClock::NowMs());
	uint16_t changed[MAX_TRACKED];
	const size_t changedCount = g_writes.Changed(Space(), changed);
	// The story's latches among them (mission.h, IsLatch): taken from 0 to 1,
	// and so used in main.scm and in this mission's code, which is loaded
	// until its script ends.
	uint16_t flags[MAX_TRACKED] = {};
	size_t   latches            = 0;
	for (size_t i = 0; i < changedCount; ++i) {
		int32_t before = 0, now = 0;
		std::memcpy(&now, Space() + changed[i], 4);
		if (!g_writes.Before(changed[i], &before) || before != 0 || now != 1)
			continue;
		GlobalUses uses;
		CountGlobalUses(Space(), GlobalsEnd(Space(), MAIN_SCRIPT_SIZE), MAIN_SCRIPT_SIZE, changed[i],
		                uses, true);
		CountGlobalUses(Space(), MAIN_SCRIPT_SIZE, SCRIPT_SPACE_SIZE, changed[i], uses, false);
		if (IsLatch(uses)) {
			flags[i] = CAMPAIGN_VALUE_LATCH;
			++latches;
		}
	}
	CampaignDeltaBody parts[MAX_TRACKED / CAMPAIGN_VALUES + 1];
	const size_t      n = BuildCampaignDelta(g_own.number, changed, changedCount, Space(), threads,
	                                         threadCount, ScriptHash(), parts,
	                                         sizeof parts / sizeof parts[0], flags);
	if (latches != 0)
		Log("missions: %s set %u of the story's latches", MissionName(g_own.number),
		    static_cast<unsigned>(latches));
	for (size_t i = 0; i < n; ++i)
		g_client->Missions().SendCampaignDelta(parts[i], WallClock::NowMs());
	Log("missions: %s leaves %u global%s, %u thread%s and %u change%s to the world behind for "
	    "everybody",
	    MissionName(g_own.number), static_cast<unsigned>(changedCount),
	    changedCount == 1 ? "" : "s", static_cast<unsigned>(threadCount),
	    threadCount == 1 ? "" : "s", static_cast<unsigned>(g_worldOpCount),
	    g_worldOpCount == 1 ? "" : "s");
}

void FlushEffects(uint32_t nowMs, bool final);

// MISSION_HAS_FINISHED, the engine's cleanup of the mission's entities. The
// first one the owner's mission runs is where it starts to end; a failure
// runs it again in the cleanup after it, which finds nothing left to do.
int8_t Finished(void *script, int32_t command, RangeFn original) {
	const bool first = IsMissionScript(script) && g_own.running && !g_own.finishing;
	// The enemies' groups go before the engine's own cleanup runs, so it lets
	// go of each original and not of a copy standing for it.
	int32_t      copies[MAX_ENEMY_GROUPS * MISSION_ENEMY_COPIES_MAX];
	const size_t copyCount = first ? g_groups.AllCopies(copies, sizeof copies / 4) : 0;
	if (first)
		g_groups.Clear();
	const int8_t r = original(script, nullptr, command);
	ReleaseCopies(copies, copyCount);
	if (g_client && first) {
		g_own.finishing = true;
		Log("missions: %s %s, and ends for everybody when its script does", MissionName(g_own.number),
		    g_own.passed ? "was passed" : "failed");
	}
	return r;
}

// A failed mission's cars (g_missionCars) start to go.
void LeaveCarsBehind(uint32_t nowMs) {
	for (size_t i = 0; i < g_missionCarCount && g_leftCarCount < MAX_MISSION_CARS; ++i)
		g_leftCars[g_leftCarCount++] = LeftCar{g_missionCars[i], nowMs};
	g_missionCarCount = 0;
}

// The owner's mission is over: its script is ending, or already gone. What it
// left in the campaign and how it went go out, and the session's mission
// with them.
void EndOwnMission(uint32_t nowMs) {
	FlushShape(nowMs, true);
	FlushEffects(nowMs, true);
	g_standing.Clear();
	g_floatingCount = 0;
	ClearStash();
	SendCampaignDelta();
	const bool passed = g_own.passed;
	g_client->Missions().Ended(g_own.number, passed ? MISSION_OUTCOME_PASSED : MISSION_OUTCOME_FAILED,
	                           nowMs);
	// What the mission made that is still here goes out as ordinary crowd, and
	// the session takes the rest away (game/missionclear.h).
	NoteOwnMissionOver(nowMs);
	if (passed)
		g_missionCarCount = 0;
	else
		LeaveCarsBehind(nowMs);
	g_getBackIn.Clear();
	ForgetSeatOrders();
	g_ownMarkers.Clear();
	g_own            = OwnMission{};
	g_blipCount      = 0;
	g_madeBlipCount  = 0;
	g_madePickupCount = 0;
	g_ownTimer       = 0;
	g_ownTimerFrozen = false;
	g_ownCounter     = 0;
	SetCutsceneScene(false);
}

// TERMINATE_THIS_SCRIPT: the mission slot's script ending is the end of the
// owner's mission, told before the engine lets go of it.
int8_t Terminate(void *script, int32_t command, RangeFn original) {
	if (g_client && g_own.running && IsMissionScript(script))
		EndOwnMission(WallClock::NowMs());
	return original(script, nullptr, command);
}

// ---- entity handles, this machine's and the session's -----------------------------

// A pedestrian or car handle of this machine's engine, as the session names
// it: one it hosts (the mission's, or traffic), or a session car. NAME_PENDING
// for one it hosts that the session has not named yet, -1 for one it never
// will (the player's own ped, say).
constexpr int32_t NAME_PENDING = -2;

int32_t NetIdForChar(int32_t handle) {
	void *const ped = Func<GetPedFn>(CPools__GetPed)(handle);
	bool        named = false;
	if (!ped || !HostedPedFor(ped, named))
		return -1;
	uint16_t netId = INVALID_NETID;
	return named && HostedPedNetIdFor(ped, netId) ? netId : NAME_PENDING;
}

int32_t NetIdForCar(int32_t handle) {
	void *const car = Func<GetPedFn>(CPools__GetVehicle)(handle);
	if (!car)
		return -1;
	bool named = false;
	if (HostedCarFor(car, named)) {
		uint16_t netId = INVALID_NETID;
		return named && HostedCarNetIdFor(car, netId) ? netId : NAME_PENDING;
	}
	const uint16_t netId = g_client ? g_client->SessionCarNetIdOf(handle) : INVALID_NETID;
	return netId != INVALID_NETID ? netId : -1;
}

// And the other way, on a participant: our replica of what the session
// names, or the car we are driving. -1 for one we have none of.
int32_t CharForNetId(int32_t netId) {
	if (netId == WIRE_OWN_PLAYER) {
		void *const me = Func<PlayerFn>(FindPlayerPed)();
		return me ? Func<int32_t(__cdecl *)(void *)>(CPools__GetPedRef)(me) : -1;
	}
	const RemoteAmbientPed *p =
	    g_client && netId >= 0 ? g_client->AmbientPed(static_cast<uint16_t>(netId)) : nullptr;
	return p && p->poolHandle >= 0 ? p->poolHandle : -1;
}

int32_t CarForNetId(int32_t netId) {
	if (netId == replay::WIRE_CATALINA_HELI)
		return CatalinaRef();
	if (!g_client || netId < 0)
		return -1;
	if (const RemoteAmbientCar *c = g_client->AmbientCar(static_cast<uint16_t>(netId))) {
		if (c->poolHandle >= 0)
			return c->poolHandle;
		// A mission car that is one of our session cars (game/missiontake.h).
		if (c->sameAsVehicle != INVALID_NETID)
			return g_client->SessionCarHandleOf(c->sameAsVehicle);
	}
	return g_client->SessionCarHandleOf(static_cast<uint16_t>(netId));
}

// ---- the mission's cars -------------------------------------------------------------

void NoteMissionCar(int32_t handle) {
	for (size_t i = 0; i < g_missionCarCount; ++i)
		if (g_missionCars[i] == handle)
			return;
	if (g_missionCarCount < MAX_MISSION_CARS) {
		g_missionCarMissingMs[g_missionCarCount]   = 0;
		g_missionCarRefusedSaid[g_missionCarCount] = false;
		g_missionCars[g_missionCarCount++]         = handle;
	}
}

void ForgetMissionCar(int32_t handle) {
	for (size_t i = 0; i < g_missionCarCount; ++i)
		if (g_missionCars[i] == handle) {
			--g_missionCarCount;
			g_missionCars[i]           = g_missionCars[g_missionCarCount];
			g_missionCarMissingMs[i]   = g_missionCarMissingMs[g_missionCarCount];
			g_missionCarRefusedSaid[i] = g_missionCarRefusedSaid[g_missionCarCount];
			return;
		}
}

bool IsMissionCar(int32_t handle) {
	for (size_t i = 0; i < g_missionCarCount; ++i)
		if (g_missionCars[i] == handle)
			return true;
	return false;
}

// Whether one of the mission's cars is on the session: hosted here as the
// mission's, or a session car somebody has claimed since.
bool MissionCarOnSession(int32_t handle, void *car) {
	bool named = false;
	return HostedCarFor(car, named) ||
	       (g_client && g_client->SessionCarNetIdOf(handle) != INVALID_NETID);
}

// One of the mission's cars put on the session now, with the reason it was
// not. False when it cannot be; said once per car.
bool PutMissionCarOnSession(size_t index, void *car, const char *reason) {
	bool        hostedNow = false;
	const char *why       = "";
	const int32_t handle  = g_missionCars[index];
	const unsigned model  = static_cast<unsigned>(Field<uint32_t>(car, offs::MODEL_INDEX) & 0xFFFF);
	if (HostMissionCar(car, hostedNow, why)) {
		if (hostedNow)
			Log("missions: %s's car %d (model %u) %s; everybody is told of it now",
			    MissionName(g_own.number), handle, model, reason);
		g_missionCarRefusedSaid[index] = false;
		return true;
	}
	if (!g_missionCarRefusedSaid[index]) {
		g_missionCarRefusedSaid[index] = true;
		Log("missions: %s's car %d (model %u) %s, and cannot be put on the session yet: %s",
		    MissionName(g_own.number), handle, model, reason, why);
	}
	return false;
}

// CREATE_CAR from the owner's mission: its handle is what the handler stored.
// The car is the session's from here, whether or not the CWorld::Add detour
// took it up inside the instruction: Give Me Liberty's Kuruma was on the
// owner's screen and on nobody else's, with no line in either log.
int8_t CarMade(void *script, int32_t command, RangeFn original) {
	const bool   mission = g_own.running && IsMissionScript(script);
	const int8_t r       = original(script, nullptr, command);
	if (!mission)
		return r;
	const int32_t handle = *reinterpret_cast<const int32_t *>(Params());
	NoteMissionCar(handle);
	void *const car = Func<GetPedFn>(CPools__GetVehicle)(handle);
	if (!car || !g_client)
		return r;
	bool named = false;
	const bool hosted = HostedCarFor(car, named);
	if (g_missionCarsSaid < 16) {
		++g_missionCarsSaid;
		Log("missions: %s made car %d (model %u)%s", MissionName(g_own.number), handle,
		    static_cast<unsigned>(Field<uint32_t>(car, offs::MODEL_INDEX) & 0xFFFF),
		    hosted ? ", hosted here for everybody" : "");
	}
	if (!hosted)
		for (size_t i = 0; i < g_missionCarCount; ++i)
			if (g_missionCars[i] == handle)
				PutMissionCarOnSession(i, car, "was not taken up when it was added to the world");
	return r;
}

// Once a frame on the owner: each of the mission's cars stays on the session.
// The engine can take a car out of the world and put it back outside any of
// the mission's instructions, and a car can go past every hosting test for a
// reason nobody has seen yet; either way it would be on this screen alone.
void KeepMissionCars(uint32_t nowMs) {
	if (!g_own.running || LocalId() == INVALID_PLAYER)
		return;
	for (size_t i = 0; i < g_missionCarCount; ++i) {
		const int32_t handle = g_missionCars[i];
		void *const   car    = Func<GetPedFn>(CPools__GetVehicle)(handle);
		if (!car || MissionCarOnSession(handle, car)) {
			g_missionCarMissingMs[i] = 0;
			continue;
		}
		if (g_missionCarMissingMs[i] == 0) {
			g_missionCarMissingMs[i] = nowMs != 0 ? nowMs : 1;
			continue;
		}
		const uint32_t missing = nowMs - g_missionCarMissingMs[i];
		if (missing < MISSION_CAR_GRACE_MS)
			continue;
		char reason[64];
		std::snprintf(reason, sizeof reason, "was on nobody's session for %u ms",
		              static_cast<unsigned>(missing));
		PutMissionCarOnSession(i, car, reason);
		g_missionCarMissingMs[i] = 0;
	}
}

// DELETE_CAR, or DONT_REMOVE_CAR handing the car to the main script: not the
// mission's to take away when it fails.
int8_t CarLetGo(void *script, int32_t command, RangeFn original) {
	if (g_own.running && IsMissionScript(script)) {
		PeekParams(script, 1);
		ForgetMissionCar(*reinterpret_cast<const int32_t *>(Params()));
	}
	return original(script, nullptr, command);
}

// Anybody at all in it, a player, a participant's copy or a pedestrian: then
// it is theirs, and stays.
bool Occupied(void *car) {
	if (Field<void *>(car, offs::VEH_DRIVER) != nullptr)
		return true;
	for (size_t seat = 0; seat < offs::VEH_MAX_PASSENGERS; ++seat)
		if (Field<void *>(car, offs::VEH_PASSENGERS + seat * 4) != nullptr)
			return true;
	return false;
}

// What of a failed mission's cars can go, gone: out of this machine's world
// through the engine's own DELETE_CAR, and out of everybody else's with it,
// since the population sweep finds a car it hosts gone and the session hears
// of it. A car the session still names as somebody's claim waits for the
// server's release of it.
void ClearLeftCars(uint32_t nowMs) {
	size_t kept = 0, cleared = 0;
	for (size_t i = 0; i < g_leftCarCount; ++i) {
		const LeftCar c   = g_leftCars[i];
		void *const   car = VehicleAt(c.handle);
		if (!car || Occupied(car))
			continue;
		// Nor one our player is opening the door of or climbing out of: no
		// seat says so, only his m_pMyVehicle and his state, and DELETE_CAR
		// would leave him mid-animation with that pointer nulled
		// (game/teardown.h refuses the same car for the same reason). Asked
		// again next time: once he is in it, it is his. The car he merely
		// drove last is not his for that, and used to be left standing for
		// the retry (game/missionclear.h).
		if (LocalPlayerAboard(car)) {
			g_leftCars[kept++] = c;
			continue;
		}
		if (g_client->SessionCarNetIdOf(c.handle) != INVALID_NETID) {
			if (nowMs - c.sinceMs < LEFT_CAR_WAIT_MS)
				g_leftCars[kept++] = c;
			continue;
		}
		// The unlink CWorld::Remove skips for a car that went static while
		// it was on the moving list, as vehicle.cpp's despawn makes it.
		Func<void(__thiscall *)(void *)>(CPhysical__RemoveFromMovingList)(car);
		// And the garages, which DELETE_CAR does not tell (game/teardown.h).
		ForgetEngineRawPointersTo(car);
		RunOurs(op::DELETE_CAR, {c.handle});
		++cleared;
	}
	g_leftCarCount = kept;
	if (cleared != 0)
		Log("missions: %u car%s the failed mission made, with nobody in %s, went with it",
		    static_cast<unsigned>(cleared), cleared == 1 ? "" : "s", cleared == 1 ? "it" : "them");
}

// ---- the player's clothes (game/outfit.h) ------------------------------------------

// UNDRESS_CHAR in the owner's mission. On its own player it goes to
// everybody, for each participant to change its own (game/outfit.h); on
// anybody else - 8-Ball in his new suit - it stays the mission's business.
int8_t Undress(void *script, int32_t command, RangeFn original) {
	if (!g_client || !g_own.running || !IsMissionScript(script))
		return original(script, nullptr, command);
	PeekParams(script, 1);
	const int32_t charHandle = reinterpret_cast<const int32_t *>(Params())[0];
	void *const   player     = Func<PlayerFn>(FindPlayerPed)();
	if (!player || Func<GetPedFn>(CPools__GetPed)(charHandle) != player)
		return original(script, nullptr, command);
	return Record(script, command, original);
}

// ---- who got out of the mission's car ---------------------------------------------

// The last location check a mission script ran, with the and/or counter it
// ran under and the frame, for PlayerInCar (Location sets them).
const void *g_locationScript      = nullptr;
uint32_t    g_locationFrame       = 0;
uint16_t    g_locationAndOr       = 0;
// And the area it asked about, when it named one (AreaForCondition).
MissionArea g_locationArea{};
bool        g_locationHasArea     = false;
bool        g_saidInCarForAnybody = false;
bool        g_saidInCarAtPlace    = false;
// The car IS_PLAYER_IN_CAR was last answered yes in for a participant, with the
// script and frame it ran in, for a location asked after it in the same block
// (standin.h, the car at the place). An `if` forgets it.
const void *g_inCarScript         = nullptr;
uint32_t    g_inCarFrame          = 0;
int32_t     g_inCarHandle         = -1;
bool        g_saidCarAtPlace      = false;
// The car the owner's mission last asked IS_PLAYER_IN_CAR about, and in
// which mission: Decoy's van (standin.h, IsDecoyEnd).
int32_t     g_askedCar            = -1;
uint16_t    g_askedCarMission     = MISSION_NONE;
bool        g_saidDecoyEnd        = false;
// The car the owner's mission walked its own player out of, in a scene that
// has not given him his controls back (standin.h): its IS_PLAYER_IN_CAR is
// the owner's own answer.
standin::WalkedOut g_walkedOut{};
bool               g_saidWalkedOut = false;

// The `if` (00D6) a script starts a block with: a location check of an earlier
// block is not one of this block's conditions.
constexpr int32_t OP_ANDOR = 0x00D6;
// GOTO_IF_FALSE, where a block's result is read (a held answer is settled there).
constexpr int32_t OP_GOTO_IF_FALSE = 0x004D;

// The car a participant's copy sits in here, at the wheel or riding: his
// replica's own seat, which holds while the session's word for it is on its
// way (a car whose netId is still NAME_PENDING has none to compare).
bool ReplicaSeatedIn(uint8_t id, void *car);

// SET_CHAR_OBJ_LEAVE_CAR on the owner's own ped, and SET_PLAYER_CONTROL giving
// him his controls back, in the owner's mission (standin.h, the car the owner
// was walked out of). Only noted: the instruction runs as it always does.
void NoteWalkOut(void *script, int32_t command) {
	if (!g_client || !g_own.running || !IsMissionScript(script))
		return;
	PeekParams(script, 2);
	const int32_t *p = reinterpret_cast<const int32_t *>(Params());
	if (command == op::SET_PLAYER_CONTROL) {
		standin::NoteControl(g_walkedOut, p[1] != 0);
		return;
	}
	void *const player = Func<PlayerFn>(FindPlayerPed)();
	standin::NoteLeaveCar(g_walkedOut, player && Func<GetPedFn>(CPools__GetPed)(p[0]) == player,
	                      p[1]);
}

// IS_PLAYER_IN_CAR from the owner's mission, about the owner: the car it
// wants its player in. A participant who was in that car at the last ask and
// is out of it now is told to get back in, alone.
//
// And any participant in that car, at the wheel or riding, is the player in
// it (mission-audit.md R4): Mike Lips Last Lunch waited for its owner at
// `while not is_player_in_car` with a helper already driving Lips' car. The
// answer is only ever widened to yes, through the flag the and/or block
// would have had (mission.h, CompareFlagIfTrue), and not in an `if and` that
// has already asked where the owner is (MayAnswerInCarForAnybody).
int8_t PlayerInCar(void *script, int32_t command, RangeFn original) {
	if (!g_client || !g_own.running || g_own.finishing || !IsMissionScript(script))
		return original(script, nullptr, command);
	PeekParams(script, 2);
	const int32_t  handle     = reinterpret_cast<const int32_t *>(Params())[1];
	const uint8_t  condBefore = At<uint8_t>(script, layout::SCRIPT_COND_RESULT);
	const uint16_t andOr      = At<uint16_t>(script, layout::SCRIPT_AND_OR);
	const bool     notFlag    = At<uint8_t>(script, layout::SCRIPT_NOT) != 0;
	g_askedCar                = handle;
	g_askedCarMission         = g_own.number;
	const int8_t   r          = original(script, nullptr, command);
	const int32_t  netId      = NetIdForCar(handle);
	if (netId < 0)
		return r;
	if (standin::InCarIsOwnersAlone(g_walkedOut, handle)) {
		if (!g_saidWalkedOut) {
			g_saidWalkedOut = true;
			Log("missions: %s waits for its own player to get out of car %d; whoever else "
			    "sits in it does not keep it waiting",
			    MissionName(g_own.number), handle);
		}
		return r;
	}
	void *const   car          = Func<GetPedFn>(CPools__GetVehicle)(handle);
	const uint8_t participants = g_client->Missions().Participants();
	uint8_t       inside       = 0;
	for (uint8_t id = 0; id < MAX_PLAYERS; ++id) {
		if (id == LocalId() || (participants & PlayerBit(id)) == 0)
			continue;
		const RemotePlayer &p = g_client->PlayerSlot(id);
		if (p.active && (p.seatVehicleNetId == static_cast<uint16_t>(netId) ||
		                 (car && ReplicaSeatedIn(id, car))))
			inside = static_cast<uint8_t>(inside | PlayerBit(id));
	}
	const bool     sameBlock     = g_locationScript == script && g_locationFrame == g_frame;
	const uint16_t locationAndOr = sameBlock ? g_locationAndOr : 0;
	bool           answer        = inside != 0 && MayAnswerInCarForAnybody(andOr, locationAndOr);
	if (inside != 0 && !answer && car) {
		// The owner at the place, and the car with a participant in it there too.
		const float *at = &Field<float>(car, offs::POSITION);
		answer = InCarAtThePlace(Vec3{at[0], at[1], at[2]}, sameBlock && g_locationHasArea,
		                         g_locationArea);
		if (answer && !g_saidInCarAtPlace) {
			g_saidInCarAtPlace = true;
			Log("missions: %s asks whether its player is at the place and in car %d; the "
			    "owner is there and a participant has brought the car in, and that answers it",
			    MissionName(g_own.number), handle);
		}
	}
	if (answer) {
		g_inCarScript = script;
		g_inCarFrame  = g_frame;
		g_inCarHandle = handle;
		At<uint8_t>(script, layout::SCRIPT_COND_RESULT) = CompareFlagIfTrue(condBefore, andOr, notFlag);
		if (!g_saidInCarForAnybody) {
			g_saidInCarForAnybody = true;
			Log("missions: %s asks whether its player is in car %d; a participant is, and that "
			    "answers it",
			    MissionName(g_own.number), handle);
		}
	}
	const uint32_t now = WallClock::NowMs();
	const uint8_t  out = g_getBackIn.Asked(static_cast<uint16_t>(netId), inside, now);
	for (uint8_t id = 0; id < MAX_PLAYERS; ++id) {
		if ((out & PlayerBit(id)) == 0)
			continue;
		g_client->Missions().SendEffect(GetBackInEffect(g_own.number, g_getBackInLabel, id), now);
		Log("missions: %s got out of %s's car, and is told to get back in",
		    g_client->PlayerSlot(id).nick.c_str(), MissionName(g_own.number));
	}
	return r;
}

// ---- the blue markers (mission.h) --------------------------------------------------

using HighlightFn = void(__cdecl *)(uint32_t id, float x1, float y1, float x2, float y2, float z);

Detour g_highlight;
// The function is the one addresses.h describes. A participant draws with it
// only then, and the owner only watches it then.
bool g_highlightUsable = false;

// A participant draws the owner's markers under ids of its own: the owner's
// with the top bit set, which no script's `this + m_nIp` has, so none is ever
// taken for a marker one of this machine's own scripts draws.
constexpr uint32_t SHOWN_MARKER_ID = 0x80000000u;

void __cdecl HookedHighlight(uint32_t id, float x1, float y1, float x2, float y2, float z) {
	if (g_missionInstruction &&
	    !g_ownMarkers.Drawn(id, MarkerArea{x1, y1, x2, y2, z}, WallClock::NowMs()) &&
	    !g_saidMarkersFull) {
		g_saidMarkersFull = true;
		Log("missions: %s draws more than %u blue markers at once; the rest stay on this screen",
		    MissionName(g_own.number), static_cast<unsigned>(MAX_MARKERS));
	}
	g_highlight.Original<HighlightFn>()(id, x1, y1, x2, y2, z);
}

bool HighlightLooksRight() {
	static const uint8_t kPrologue[] = {0x83, 0xEC, 0x18, 0xD9, 0x44, 0x24, 0x30};
	return std::memcmp(Ptr<uint8_t>(HIGHLIGHT_AREA), kPrologue, sizeof kPrologue) == 0;
}

void InstallHighlight() {
	if (!HighlightLooksRight()) {
		Log("missions: CTheScripts::HighlightImportantArea at 0x%08X does not start the way "
		    "addresses.h says; the mission's blue markers stay on its owner's screen",
		    static_cast<unsigned>(HIGHLIGHT_AREA));
		return;
	}
	g_highlightUsable = true;
	if (g_highlight.Install("CTheScripts::HighlightImportantArea",
	                        reinterpret_cast<void *>(HIGHLIGHT_AREA),
	                        reinterpret_cast<void *>(&HookedHighlight)))
		Log("missions: hooked CTheScripts::HighlightImportantArea at 0x%08X; a mission's blue "
		    "markers are drawn on every participant's screen",
		    static_cast<unsigned>(HIGHLIGHT_AREA));
	else
		Log("missions: CTheScripts::HighlightImportantArea would not hook; we still draw "
		    "another owner's blue markers, but our own mission's stay on our screen");
}

void Highlight(uint32_t id, const MarkerArea &a) {
	const HighlightFn draw = g_highlight.IsInstalled() ? g_highlight.Original<HighlightFn>()
	                                                   : Func<HighlightFn>(HIGHLIGHT_AREA);
	draw(id, a.x1, a.y1, a.x2, a.y2, a.z);
}

// The owner's: what its mission's markers did, to everybody. Each one's first
// going up and its coming down is logged; the resends are not.
void TellMarkers(uint32_t nowMs) {
	g_ownMarkers.Tick(nowMs, [&](uint32_t id, const MarkerArea &a, bool up, bool first) {
		g_client->Missions().SendEffect(MarkerEffect(g_own.number, id, a, up), nowMs);
		if (first || !up)
			Log("missions: %s %s its blue marker at (%.1f, %.1f)%s", MissionName(g_own.number),
			    up ? "put up" : "took down", (a.x1 + a.x2) * 0.5f, (a.y1 + a.y2) * 0.5f,
			    up ? "; everybody draws it" : "");
	});
}

// A participant's: the owner's machine says one went up or came down.
void HeardMarker(uint32_t id, const MarkerArea &a, bool up) {
	const bool was = g_shownMarkers.Has(id);
	if (!g_shownMarkers.Heard(id, a, up, WallClock::NowMs())) {
		if (!g_saidMarkersFull) {
			g_saidMarkersFull = true;
			Log("missions: the owner's mission has more than %u blue markers up; the rest are "
			    "not drawn here", static_cast<unsigned>(MAX_MARKERS));
		}
		return;
	}
	if (was != up)
		Log("missions: the owner's blue marker at (%.1f, %.1f) is %s here%s",
		    (a.x1 + a.x2) * 0.5f, (a.y1 + a.y2) * 0.5f, up ? "up" : "down",
		    up && !g_highlightUsable ? ", but this image cannot draw it" : "");
}

// ---- what the owner's mission shows, in the order it showed it ----------------------
//
// A pedestrian or car an instruction names goes as the session's name for it
// (replay::ToWire), and the mission usually names one the moment it has made
// it: CREATE_CAR, then ADD_BLIP_FOR_CAR, before the session has named the car.
// So an effect that cannot be put on the wire yet waits, and everything after
// it waits behind it, so a blip's colour never arrives before the blip. One
// that still cannot go after EFFECT_NAME_WAIT_MS is dropped and the rest go.
constexpr uint32_t EFFECT_NAME_WAIT_MS = 3000;
constexpr size_t   MAX_WAITING_EFFECTS = 64;
struct WaitingEffect {
	MissionEffectBody body;
	uint32_t          sinceMs;
};
WaitingEffect g_waiting[MAX_WAITING_EFFECTS];
size_t        g_waitingCount = 0;
bool          g_saidNameWait = false;

// One effect to everybody, and what it leaves standing noted.
void SendToAll(const MissionEffectBody &wire, const MissionEffectBody &recorded, uint32_t nowMs) {
	g_client->Missions().SendEffect(wire, nowMs);
	if (g_own.running && !g_standing.Note(recorded) && !g_saidStandingFull) {
		g_saidStandingFull = true;
		Log("missions: %s has more up than can be kept for a player who comes in late; they "
		    "will not see all of it",
		    MissionName(g_own.number));
	}
}

// The body with every pedestrian and car it names as the session's name for
// it: 1 when it is ready to go, 0 while one of them is still being named,
// -1 for one the session will never name.
int EffectToWire(MissionEffectBody &body) {
	if (body.length > replay::MAX_CODE)
		return -1;
	uint8_t code[replay::MAX_CODE];
	std::memcpy(code, body.code, body.length);
	bool waiting = false, never = false;
	const bool noCarOk = replay::CarMayBeNone(EffectOpcode(body));
	replay::EachHandle(code, body.length, [&](replay::Arg a, int32_t *v) {
		if (a != replay::Arg::Char && a != replay::Arg::Car)
			return true;
		// "No car", which everybody can name (replay.h, CarMayBeNone).
		if (a == replay::Arg::Car && noCarOk && *v < 0) {
			*v = -1;
			return true;
		}
		// The camera on our own player: everybody's goes to theirs.
		if (a == replay::Arg::Char && MayNameOwnPlayer(EffectOpcode(body))) {
			void *const me = Func<PlayerFn>(FindPlayerPed)();
			if (me && *v == Func<int32_t(__cdecl *)(void *)>(CPools__GetPedRef)(me)) {
				*v = WIRE_OWN_PLAYER;
				return true;
			}
		}
		// The Catalina helicopter, which every participant flies its own
		// copy of and nobody hosts: named as whoever's copy it is.
		if (a == replay::Arg::Car && *v >= 0 && *v == CatalinaRef()) {
			*v = replay::WIRE_CATALINA_HELI;
			return true;
		}
		int32_t n = a == replay::Arg::Char ? NetIdForChar(*v) : NetIdForCar(*v);
		// One of the mission's own cars that is on nobody's session is put
		// there now, and the instruction waits for its name, rather than going
		// to nobody: CHANGE_CAR_COLOUR right behind CREATE_CAR is how a
		// participant's copy comes to be the right colour.
		if (n == -1 && a == replay::Arg::Car && g_own.running && IsMissionCar(*v))
			for (size_t i = 0; i < g_missionCarCount; ++i)
				if (g_missionCars[i] == *v) {
					void *const car = Func<GetPedFn>(CPools__GetVehicle)(*v);
					if (car && PutMissionCarOnSession(i, car,
					                                  "was named by an instruction before "
					                                  "it was on the session"))
						n = NetIdForCar(*v);
					break;
				}
		if (n == NAME_PENDING)
			waiting = true;
		else if (n < 0)
			never = true;
		else
			*v = n;
		return true;
	});
	if (never)
		return -1;
	if (waiting)
		return 0;
	std::memcpy(body.code, code, body.length);
	return 1;
}

// Everything that can go, in order: until the first that cannot yet, which
// stays at the head unless it has waited too long, or `final`, the mission's
// end, when whatever is left goes nowhere.
void FlushEffects(uint32_t nowMs, bool final) {
	size_t sent = 0;
	while (sent < g_waitingCount) {
		WaitingEffect    &w     = g_waiting[sent];
		MissionEffectBody wire  = w.body;
		const int         ready = EffectToWire(wire);
		if (ready > 0) {
			SendToAll(wire, w.body, nowMs);
		} else if (ready == 0 && !final && nowMs - w.sinceMs < EFFECT_NAME_WAIT_MS) {
			break;
		} else if (!g_saidNameWait) {
			g_saidNameWait = true;
			Log("missions: an instruction %02X%02X of %s names a pedestrian or car the session "
			    "never named, and nobody else is shown it",
			    w.body.code[1], w.body.code[0], MissionName(g_own.number));
		}
		++sent;
	}
	for (size_t i = sent; i < g_waitingCount; ++i)
		g_waiting[i - sent] = g_waiting[i];
	g_waitingCount -= sent;
}

void QueueEffect(const MissionEffectBody &body) {
	const uint32_t now = WallClock::NowMs();
	if (g_waitingCount == 0) {
		MissionEffectBody out   = body;
		const int         ready = EffectToWire(out);
		if (ready > 0)
			SendToAll(out, body, now);
		else if (ready < 0 && !g_saidNameWait) {
			g_saidNameWait = true;
			Log("missions: an instruction %02X%02X of %s names a pedestrian or car the session "
			    "will never name, and nobody else is shown it",
			    body.code[1], body.code[0], MissionName(body.missionNumber));
		}
		if (ready != 0)
			return;   // sent, or never going to be
	}
	// A queue this full has been waiting on names that are not coming: all of
	// it goes now, what can be sent sent.
	if (g_waitingCount == MAX_WAITING_EFFECTS)
		FlushEffects(now + EFFECT_NAME_WAIT_MS, false);
	g_waiting[g_waitingCount++] = WaitingEffect{body, now};
}

// What the owner's mission ran, on its way to everybody: a blip taken off and
// put back the same frame stays one blip, a repeat of what was set last is
// not sent, and a change to the same thing goes at most so often, the newest
// last (game/effectshape.h).
void ShapeEffect(const MissionEffectBody &body) {
	const uint32_t now    = WallClock::NowMs();
	auto           toWire = [](const MissionEffectBody &b) { QueueEffect(b); };
	auto           shaped = [&](const MissionEffectBody &b) { g_shaper.Offer(b, now, toWire); };
	if (body.kind == MISSION_EFFECT_BLIP_NEW || body.kind == MISSION_EFFECT_BLIP_USE)
		g_aliases.Offer(body, g_frame, now, shaped);
	else
		shaped(body);
}

// Once a frame on the owner, before its scripts run: what waited its turn.
void TickShape(uint32_t nowMs) {
	auto toWire = [](const MissionEffectBody &b) { QueueEffect(b); };
	auto shaped = [&](const MissionEffectBody &b) { g_shaper.Offer(b, nowMs, toWire); };
	g_aliases.Tick(g_frame, nowMs, shaped);
	g_shaper.Tick(nowMs, toWire);
}

// The owner's mission is over, or its trigger's part is and the mission's
// starts: whatever was held goes, and at the end how much less went out than
// it ran is said once.
void FlushShape(uint32_t nowMs, bool say) {
	auto toWire = [](const MissionEffectBody &b) { QueueEffect(b); };
	auto shaped = [&](const MissionEffectBody &b) { g_shaper.Offer(b, nowMs, toWire); };
	g_aliases.Flush(nowMs, shaped);
	g_shaper.Flush(nowMs, toWire);
	if (say && (g_shaper.Dropped() != 0 || g_shaper.Held() != 0 || g_aliases.Kept() != 0))
		Log("missions: %s said %u thing%s again that nobody needed to hear twice, held %u change%s "
		    "back to the pace everybody gets them at, and moved %u blip%s rather than making new ones",
		    MissionName(g_own.number), g_shaper.Dropped(), g_shaper.Dropped() == 1 ? "" : "s",
		    g_shaper.Held(), g_shaper.Held() == 1 ? "" : "s", g_aliases.Kept(),
		    g_aliases.Kept() == 1 ? "" : "s");
	g_aliases.Clear();
	g_shaper.Clear();
	g_ownCoronas.Clear();
}

// ---- the models a cutscene is laid over (game/cutscene.h) --------------------------

// The owner's LOAD_CUTSCENE, before the engine runs it: what each animation
// of the scene is laid over here. Loads what was asked for and is not in yet
// first, the way LOAD_ALL_MODELS_NOW would, and says in the log what the
// engine is about to fail on if something is still missing.
bool OwnCutsceneModels(const replay::Encoded &enc, uint16_t missionNumber, CutsceneModels *scene) {
	char name[9];
	if (!CutsceneNameOf(enc.code, enc.length, name))
		return false;
	if (!ResolveCutsceneModels(name, true, scene)) {
		Log("missions: cutscene '%s' could not be read out of anim\\cuts.img here; it goes to "
		    "everybody without its models",
		    name);
		return false;
	}
	char models[512];
	DescribeCutsceneModels(*scene, models, sizeof models);
	if (scene->missing == 0) {
		Log("missions: cutscene '%s' of %s is laid over %s here; everybody is asked for the same "
		    "models ahead of it",
		    name, MissionName(missionNumber), models);
		return true;
	}
	char specials[192];
	DescribeSpecialModels(specials, sizeof specials);
	Log("missions: cutscene '%s' of %s has an animation with no model loaded to lay it over, even "
	    "after loading what was asked for: %s (models now %s). The engine reads address 0 on it",
	    name, MissionName(missionNumber), models, specials);
	return true;
}

// Ahead of the scene, to everybody: each model the owner's scene is laid
// over, asked for under the name it has here, and then loaded. A mission can
// rename a special character with no instruction on the replay list (Give Me
// Liberty's UNDRESS_CHAR on 8-Ball, whose 'eight2' Luigi's scene is laid
// over), and somebody who came in late missed the ones that are; either way
// the scene's animations would find no model on their machine. For a model
// that is already there under that name each of these is a RequestModel of a
// loaded model, which does nothing.
void SendCutsceneModels(const CutsceneModels &scene, uint16_t missionNumber) {
	int32_t sent[CUTSCENE_ANIMS_MAX];
	size_t  count = 0;
	for (size_t i = 0; i < scene.anims.count; ++i) {
		const int32_t id = scene.model[i];
		bool          again = false;
		for (size_t k = 0; k < count; ++k)
			again = again || sent[k] == id;
		if (id < 0 || again || !ScriptOwnsModel(id))
			continue;
		uint8_t      code[CUTSCENE_MODEL_CODE];
		const size_t n = CutsceneModelCode(static_cast<uint16_t>(id), NameOfModel(id), code);
		if (n == 0)
			continue;
		MissionEffectBody b{};
		b.missionNumber = missionNumber;
		b.kind          = MISSION_EFFECT_RUN;
		b.handleAt      = 0xFF;
		b.ownerBlip     = -1;
		b.length        = static_cast<uint8_t>(n);
		std::memcpy(b.code, code, n);
		QueueEffect(b);
		sent[count++] = id;
	}
	if (count == 0)
		return;
	MissionEffectBody load{};
	load.missionNumber = missionNumber;
	load.kind          = MISSION_EFFECT_RUN;
	load.handleAt      = 0xFF;
	load.ownerBlip     = -1;
	load.length        = 2;
	load.code[0]       = static_cast<uint8_t>(op::LOAD_ALL_MODELS_NOW & 0xFF);
	load.code[1]       = static_cast<uint8_t>(op::LOAD_ALL_MODELS_NOW >> 8);
	QueueEffect(load);
}

// A participant, before it runs the owner's LOAD_CUTSCENE: every animation
// of the scene has a model here to be laid over, once what was asked for
// ahead of it is loaded. One with none would be read at address 0 halfway
// through the load (addresses.h), and nothing after it could be undone, so
// then the scene is not loaded here at all and the log says what was missing.
bool CutsceneModelsHere(const MissionEffectBody &body) {
	char name[9];
	if (!CutsceneNameOf(body.code, body.length, name))
		return true;
	// Its tail reads through the player's ped with no test (0x00404876,
	// FindPlayerPed, then [esi+53Ch]).
	if (!Func<PlayerFn>(FindPlayerPed)()) {
		Log("missions: not loading cutscene '%s' here: this game has no player ped right now", name);
		return false;
	}
	CutsceneModels scene;
	if (!ResolveCutsceneModels(name, true, &scene)) {
		Log("missions: cutscene '%s' could not be read out of anim\\cuts.img here; loading it as "
		    "it came",
		    name);
		return true;
	}
	char models[512];
	DescribeCutsceneModels(scene, models, sizeof models);
	if (scene.missing == 0) {
		Log("missions: loading cutscene '%s' here, laid over %s", name, models);
		return true;
	}
	char specials[192];
	DescribeSpecialModels(specials, sizeof specials);
	Log("missions: not loading cutscene '%s' here: an animation of it has no model to be laid "
	    "over: %s (models now %s). The scene is skipped on this machine",
	    name, models, specials);
	return false;
}

// A replayed LOAD_SPECIAL_CHARACTER or LOAD_SPECIAL_MODEL that would rename a
// model something here is still built from, other than a remote player's ped
// (ped.cpp takes those down itself). RequestSpecialModel moves the model onto
// the new name's texture dictionary, and whatever was built from it gives its
// reference back to that one when it goes (CBaseModelInfo::RemoveRef,
// 0x004F6BB0), which CTxdStore::RemoveRef (0x00527970) unloads at zero. The
// engine never renames under anything: UNDRESS_CHAR takes its ped out first,
// and a mission deletes its special characters before the scene that loads
// others. Here the copy of a ped the owner deleted a moment ago can stand a
// frame or two longer, so the rename waits until it has gone.
bool RenameUnderLiveEntities(const MissionEffectBody &body) {
	int32_t model = -1;
	char    name[9];
	if (!SpecialLoadTarget(body.code, body.length, &model, name) || !IsRename(NameOfModel(model), name))
		return false;
	const uint16_t refs = ModelRefs(model);
	if (refs <= RemotePlayerPedsBuiltFrom(model))
		return false;
	static int32_t saidModel = -1;
	static char    saidName[9];
	if (saidModel != model || std::strcmp(saidName, name) != 0) {
		saidModel = model;
		std::memcpy(saidName, name, sizeof saidName);
		const char *const now = NameOfModel(model);
		Log("missions: model %d waits to become '%s' until the %u thing(s) here built from it as "
		    "'%s' are gone",
		    model, name, static_cast<unsigned>(refs), now ? now : "?");
	}
	return true;
}

// CREATE_CUTSCENE_OBJECT and CREATE_CUTSCENE_HEAD build from a model that
// has to be in. CreateCutsceneObject hands a prop's GetRwObject straight to
// RpClumpForAllAtomics (0x00404C1D..0x00404C3C), and anything else built
// with no clump is read through by SET_CUTSCENE_ANIM ([object+4Ch] at
// 0x00404D25). A participant's player model is out for a frame or more while
// its clothes change (game/outfit.h), and a model asked for ahead of the
// scene streams in only once it is loaded.
bool CutsceneObjectModelReady(const replay::Encoded &run) {
	const uint16_t opcode = static_cast<uint16_t>(run.code[0] | (run.code[1] << 8));
	if (opcode != cutscene_op::CREATE_CUTSCENE_OBJECT && opcode != cutscene_op::CREATE_CUTSCENE_HEAD)
		return true;
	int32_t model = -1;
	if (!replay::LiteralAt(run.code, run.length, opcode == cutscene_op::CREATE_CUTSCENE_HEAD ? 1 : 0,
	                       &model))
		return true;
	if (model >= 0 && HasModelLoaded(static_cast<uint32_t>(model)))
		return true;
	LoadRequestedModelsNow();
	if (model >= 0 && HasModelLoaded(static_cast<uint32_t>(model)))
		return true;
	const char *const name = NameOfModel(model);
	Log("missions: a cutscene %s of model %d ('%s') is not made here: the model is not loaded, "
	    "and the scene would read through one made without it",
	    opcode == cutscene_op::CREATE_CUTSCENE_HEAD ? "head" : "object", model, name ? name : "?");
	return false;
}

// ---- the mission's stash (mission-audit.md R2) -------------------------------------

// A pickup the mission is laying out that is everybody's own: a weapon or
// armour (CPickups::WeaponForModel, addresses.h), health, or cash. Anything
// else it lays out is an object of the story, a briefcase or a package, and
// stays one pickup for everybody.
bool IsStashKind(const replay::Encoded &enc) {
	const uint16_t opcode = static_cast<uint16_t>(enc.code[0] | (enc.code[1] << 8));
	if (opcode == op::CREATE_MONEY_PICKUP)
		return true;
	if (opcode == op::CREATE_FLOATING_PACKAGE)
		return false;
	int32_t model = 0;
	if (!replay::LiteralAt(enc.code, enc.length, 0, &model))
		return false;
	if (model == Global<int16_t>(MI_PICKUP_HEALTH))
		return true;
	return Func<int32_t(__cdecl *)(int32_t)>(CPickups__WeaponForModel)(model) != 0;
}

// Where a pickup instruction puts it, and of which model.
bool StashPlace(const replay::Encoded &enc, float *x, float *y, int16_t *model) {
	const uint16_t opcode = static_cast<uint16_t>(enc.code[0] | (enc.code[1] << 8));
	const uint8_t  at     = opcode == op::CREATE_MONEY_PICKUP        ? 0
	                        : opcode == op::CREATE_PICKUP_WITH_AMMO ? 3
	                                                                 : 2;
	int32_t xb = 0, yb = 0, m = -1;
	if (!replay::LiteralAt(enc.code, enc.length, at, &xb) ||
	    !replay::LiteralAt(enc.code, enc.length, static_cast<uint8_t>(at + 1), &yb))
		return false;
	if (opcode != op::CREATE_MONEY_PICKUP && !replay::LiteralAt(enc.code, enc.length, 0, &m))
		return false;
	std::memcpy(x, &xb, 4);
	std::memcpy(y, &yb, 4);
	*model = static_cast<int16_t>(m);
	return true;
}

void RememberStash(const replay::Encoded &enc, int32_t owner, int32_t ours) {
	if (g_stashCount == MAX_STASH || !IsStashKind(enc))
		return;
	StashPickup s;
	s.owner = owner;
	s.ours  = ours;
	if (StashPlace(enc, &s.x, &s.y, &s.model))
		g_stash[g_stashCount++] = s;
}

bool IsStash(int32_t owner) {
	for (size_t i = 0; i < g_stashCount; ++i)
		if (g_stash[i].owner == owner)
			return true;
	return false;
}

// The mission is over: whatever of its stash nobody here took goes, the way
// its own cleanup would have taken it.
void ClearStash() {
	for (size_t i = 0; i < g_stashCount; ++i)
		RunOurs(op::REMOVE_PICKUP, {g_stash[i].ours});
	g_stashCount = 0;
}

// ---- the mission's floating packages (mission-audit.md R2) -------------------------
//
// A Drop In The Ocean's packages float where they land, so the pickup sync,
// which names a pickup by where it is, leaves them to each machine. Each
// machine watches its own, and one that goes without the mission taking it
// away was taken here: everybody hears which, by the owner's handle. The
// owner's machine then takes its own for its mission to count, and everybody
// else's goes.

void NoteFloating(int32_t owner, int32_t ours) {
	if (g_floatingCount < MAX_FLOATING)
		g_floating[g_floatingCount++] = FloatingPackage{owner, ours, false};
}

FloatingPackage *FloatingOf(int32_t owner) {
	for (size_t i = 0; i < g_floatingCount; ++i)
		if (g_floating[i].owner == owner)
			return &g_floating[i];
	return nullptr;
}

// The mission took it away itself: nobody took it.
void FloatingRemoved(int32_t owner) {
	if (FloatingPackage *f = FloatingOf(owner))
		f->gone = true;
}

void WatchFloating(uint32_t nowMs) {
	for (size_t i = 0; i < g_floatingCount; ++i) {
		FloatingPackage &f = g_floating[i];
		if (f.gone || PickupStillUp(f.ours))
			continue;
		f.gone = true;
		g_client->Missions().PickupTaken(f.owner, nowMs);
	}
}

// Somebody else took theirs.
// A participant's kill, for this machine's running mission to count
// (mission-audit.md R11): the one entry CDarkel::RegisterKillByPlayer makes that
// GET_NUM_OF_MODELS_KILLED_BY_PLAYER reads, and none of its statistics, which
// are about this machine's own player. The array is NUM_DEFAULT_MODELS long and
// `model` came off the wire.
void CountKill(uint16_t model) {
	if (model < NUM_DEFAULT_MODELS)
		Ptr<uint16_t>(CDarkel__RegisteredKills)[model] += 1;
}

void TakePickup(int32_t ownerHandle) {
	FloatingPackage *f = FloatingOf(ownerHandle);
	if (!f || f->gone)
		return;
	f->gone = true;
	TakeMissionPickup(f->ours, g_own.running);
}

// REMOVE_PICKUP from the session's mission on one of its stash: not yet
// (StashPickup). Past the instruction, as its handler would have gone.
int8_t RemovePickup(void *script, int32_t command, RangeFn original) {
	if (g_own.running && IsMissionScript(script)) {
		PeekParams(script, 1);
		FloatingRemoved(reinterpret_cast<const int32_t *>(Params())[0]);
		if (IsStash(reinterpret_cast<const int32_t *>(Params())[0])) {
			uint32_t ip = At<uint32_t>(script, layout::SCRIPT_IP);
			Func<CollectFn>(CTheScripts__CollectParameters)(script, &ip, 1);
			At<uint32_t>(script, layout::SCRIPT_IP) = ip;
			return 0;
		}
	}
	return Record(script, command, original);
}

// The global an on-screen timer or counter reads, out of its encoded
// DISPLAY or CLEAR: `02 lo hi` right after the opcode.
uint16_t WidgetGlobal(const replay::Encoded &enc) {
	if (enc.length < 5 || enc.code[2] != PARAM_GLOBAL)
		return 0;
	return static_cast<uint16_t>(enc.code[3] | (enc.code[4] << 8));
}

// Which of the owner's widgets are up, for their values to follow them.
void NoteOwnWidget(const replay::Encoded &enc) {
	const uint16_t opcode = static_cast<uint16_t>(enc.code[0] | (enc.code[1] << 8));
	switch (opcode) {
	case op::DISPLAY_ONSCREEN_TIMER:  g_ownTimer = WidgetGlobal(enc); g_ownTimerFrozen = false; break;
	case op::CLEAR_ONSCREEN_TIMER:    g_ownTimer = 0; break;
	case op::DISPLAY_ONSCREEN_COUNTER:
	case op::DISPLAY_ONSCREEN_COUNTER_WITH_STRING: g_ownCounter = WidgetGlobal(enc); break;
	case op::CLEAR_ONSCREEN_COUNTER:  g_ownCounter = 0; break;
	case op::FREEZE_ONSCREEN_TIMER: {
		int32_t frozen = 0;
		if (replay::LiteralAt(enc.code, enc.length, 0, &frozen))
			g_ownTimerFrozen = frozen != 0;
		break;
	}
	default: break;
	}
}

// Where the car an instruction names is simulated (replay::Kind::Holder,
// mission-audit.md R9): the player who drives it or settles it, or
// CAR_HELD_BY_NOBODY for a session car nobody holds, which every machine keeps
// its own copy of. CAR_SIMULATED_HERE for one this machine drives or hosts,
// a car the mission made among them, whose state its stream carries.
constexpr int32_t CAR_SIMULATED_HERE = -2;
constexpr int32_t CAR_HELD_BY_NOBODY = -1;

int32_t HolderOfCarIn(const replay::Encoded &enc) {
	uint8_t code[replay::MAX_CODE];
	if (enc.length > sizeof code)
		return CAR_SIMULATED_HERE;
	std::memcpy(code, enc.code, enc.length);
	int32_t handle = -1;
	replay::EachHandle(code, enc.length, [&](replay::Arg a, int32_t *v) {
		if (a == replay::Arg::Car && handle < 0)
			handle = *v;
		return true;
	});
	const uint16_t netId = handle >= 0 ? g_client->SessionCarNetIdOf(handle) : INVALID_NETID;
	const RemoteVehicle *row = netId != INVALID_NETID ? g_client->VehicleByNetId(netId) : nullptr;
	// A car nobody has claimed is one this machine hosts: the mission's own,
	// which its engine made. game/carauthority.h has the whole rule.
	const CarSimulator sim =
	    row ? WhoSimulates(true, row->driverPlayerId, row->custodianPlayerId, INVALID_PLAYER, LocalId())
	        : WhoSimulates(false, INVALID_PLAYER, INVALID_PLAYER, INVALID_PLAYER, LocalId());
	const bool wreck = enc.length >= 2 && (enc.code[0] | (enc.code[1] << 8)) == op::EXPLODE_CAR;
	switch (wreck ? WhereTheWreckGoes(sim) : WhereTheWordGoes(sim)) {
	case CarWordTo::Nobody:    return CAR_SIMULATED_HERE;
	case CarWordTo::OnePlayer: return sim.player;
	default:                   return CAR_HELD_BY_NOBODY;
	}
}

// ARM_CAR_WITH_BOMB's two operands, the car and the bomb, out of an encoded
// copy of it: the car as the handle or netId the code carries.
bool ArmedCarIn(const uint8_t *code, size_t length, int32_t *car, int32_t *type) {
	if (length < 2 || static_cast<uint16_t>(code[0] | (code[1] << 8)) != op::ARM_CAR_WITH_BOMB ||
	    length > replay::MAX_CODE)
		return false;
	uint8_t copy[replay::MAX_CODE];
	std::memcpy(copy, code, length);
	*car = -1;
	replay::EachHandle(copy, length, [&](replay::Arg a, int32_t *v) {
		if (a == replay::Arg::Car && *car < 0)
			*car = *v;
		return true;
	});
	return *car >= 0 && replay::LiteralAt(code, length, 1, type);
}

// An instruction on the replay list (game/replay.h), from the owner's mission
// or from its trigger between the grant and the launch: read as the engine
// reads it, run, and sent for everybody else's engine to run too. A new blip's
// handle is only known once it has run.
int8_t Record(void *script, int32_t command, RangeFn original) {
	const bool fromMission = g_own.running && IsMissionScript(script);
	const bool fromTrigger = script == g_launching;
	if (!g_client || !(fromMission || fromTrigger))
		return original(script, nullptr, command);

	replay::Encoded enc;
	const uint32_t  ip   = At<uint32_t>(script, layout::SCRIPT_IP);
	bool            read = replay::Encode(static_cast<uint16_t>(command), Space(), SCRIPT_SPACE_SIZE, ip,
	                                      Locals(script), &enc);
	// A live blip the mission never made, named by a global: the main
	// script's own there, a contact's marker, which every machine keeps in its
	// own global. One already gone is left as it was, for nobody to run.
	if (read && enc.kind == replay::Kind::BlipUse && !MadeBlip(enc.ownerBlip) &&
	    LiveBlip(enc.ownerBlip))
		read = replay::Encode(static_cast<uint16_t>(command), Space(), SCRIPT_SPACE_SIZE, ip,
		                      Locals(script), &enc, true);
	else if (read && enc.kind == replay::Kind::BlipUse && command == op::REMOVE_BLIP)
		ForgetMadeBlip(enc.ownerBlip);
	// A pickup the world keeps (replay.h, KeepsPickup) is no stash: every
	// machine makes its own into its own global, and it is in the campaign.
	// And a pickup taken away that the mission never laid out goes by the
	// global that holds it.
	int32_t removed = -1;
	if (read && enc.kind == replay::Kind::PickupNew && replay::KeepsPickup(enc.code, enc.length))
		read = replay::Encode(static_cast<uint16_t>(command), Space(), SCRIPT_SPACE_SIZE, ip,
		                      Locals(script), &enc, false, true);
	else if (read && command == op::REMOVE_PICKUP &&
	         replay::LiteralAt(enc.code, enc.length, 0, &removed)) {
		if (!MadePickup(removed) && PickupInUse(removed))
			read = replay::Encode(static_cast<uint16_t>(command), Space(), SCRIPT_SPACE_SIZE, ip,
			                      Locals(script), &enc, false, true);
		else
			ForgetMadePickup(removed);
	}
	const replay::Entry *listed = replay::Find(static_cast<uint16_t>(command));
	const bool makesBlip = listed && listed->kind == replay::Kind::BlipNew;
	const uint16_t missionNumber = fromMission ? g_own.number : g_launchHint;
	CutsceneModels scene;
	const bool     sceneRead =
	    read && command == op::LOAD_CUTSCENE && OwnCutsceneModels(enc, missionNumber, &scene);
	const int8_t r = original(script, nullptr, command);
	if (makesBlip)
		NoteMadeBlip(*reinterpret_cast<const int32_t *>(Params()));   // what it stored
	if (!read) {
		if (!g_saidDropped) {
			g_saidDropped = true;
			Log("missions: an instruction %04X of the mission could not be read to be sent",
			    static_cast<unsigned>(command));
		}
		return r;
	}
	// Our mission fitted a bomb: the handler named our player, which is right
	// here, and the session is told the bomb is ours (Client::MissionArmedCar).
	int32_t armedCar = -1, armedType = 0;
	if (command == op::ARM_CAR_WITH_BOMB && ArmedCarIn(enc.code, enc.length, &armedCar, &armedType))
		g_client->MissionArmedCar(g_client->SessionCarNetIdOf(armedCar),
		                          static_cast<uint8_t>(armedType), true);
	// What the mission tells its own player about the car they are out of is
	// the owner's alone: every participant who gets out of it hears their own
	// (PlayerInCar), and nobody hears somebody else's.
	if (fromMission && IsGetBackInPrint(enc.code, enc.length)) {
		if (command != op::CLEAR_THIS_PRINT)
			std::memcpy(g_getBackInLabel, enc.code + 2, TEXT_LABEL);
		return r;
	}
	// What it says about the car goes to the one who has to deal with it
	// (game/getaway.h).
	if (fromMission && InGetaway(script) && GetawayCrewPrint(command, enc.code, enc.length))
		return r;
	MissionEffectBody body{};
	body.missionNumber = missionNumber;
	body.length        = enc.length;
	body.handleAt      = enc.handleAt;
	body.ownerBlip     = enc.ownerBlip;
	if (fromMission && command == op::LOAD_ALL_MODELS_NOW)
		body.readySeq = ++g_readySeq;
	std::memcpy(body.code, enc.code, enc.length);
	switch (enc.kind) {
	case replay::Kind::Plain:
		body.kind = MISSION_EFFECT_RUN;
		// One of the main script's objects taken away is the world's for good
		// (replay.h, DeletesWorldObject): in the campaign too, for a game that
		// catches up from the log rather than watching it happen.
		if (fromMission && g_worldOpCount < MAX_WORLD_OPS &&
		    replay::DeletesWorldObject(enc.code, enc.length,
		                               [](uint16_t g) { return g_writes.Known(g); })) {
			WorldOp &w = g_worldOps[g_worldOpCount++];
			w.length   = enc.length;
			std::memcpy(w.code, enc.code, enc.length);
		}
		break;
	case replay::Kind::Pay: {
		// What the mission charges its player (Bomb Da Base: Act II's
		// $100,000, The Exchange's $500,000) is the owner's alone: only the
		// owner's balance was checked, and a helper would go into debt for it.
		int32_t amount = 0;
		if (replay::LiteralAt(enc.code, enc.length, 1, &amount) && amount < 0) {
			static bool saidCharge = false;
			if (!saidCharge) {
				saidCharge = true;
				Log("missions: %s takes %d from its player; nobody else pays it",
				    MissionName(missionNumber), static_cast<int>(amount));
			}
			return r;
		}
		body.kind = MISSION_EFFECT_PAY;
		break;
	}
	case replay::Kind::BlipUse:
		body.kind = MISSION_EFFECT_BLIP_USE;
		if (fromMission && InGetaway(script) && GetawayBlipTarget(enc.ownerBlip, command, body))
			return r;
		break;
	case replay::Kind::BlipNew:
		body.kind      = MISSION_EFFECT_BLIP_NEW;
		body.ownerBlip = *reinterpret_cast<const int32_t *>(Params());   // what it stored
		if (fromMission && command == getaway::OP_ADD_BLIP_FOR_CHAR && InGetaway(script) &&
		    GetawayCrewBlip(body.ownerBlip, body))
			return r;
		break;
	case replay::Kind::World:
		body.kind = MISSION_EFFECT_RUN;
		if (fromMission && g_worldOpCount < MAX_WORLD_OPS) {
			WorldOp &w = g_worldOps[g_worldOpCount++];
			w.length   = enc.length;
			std::memcpy(w.code, enc.code, enc.length);
		}
		break;
	case replay::Kind::Campaign:
		// Counted once, from the delta, on every machine but this one.
		if (fromMission && g_worldOpCount < MAX_WORLD_OPS) {
			WorldOp &w = g_worldOps[g_worldOpCount++];
			w.length   = enc.length;
			std::memcpy(w.code, enc.code, enc.length);
		} else if (fromMission && !g_saidWorldOpsFull) {
			g_saidWorldOpsFull = true;
			Log("missions: %s changes more of the campaign than a delta holds; the rest of it "
			    "stays this game's own", MissionName(g_own.number));
		}
		return r;
	case replay::Kind::ObjectNew:
		body.kind      = MISSION_EFFECT_OBJECT_NEW;
		body.ownerBlip = *reinterpret_cast<const int32_t *>(Params());   // what it stored
		break;
	case replay::Kind::Teleport: {
		body.kind = MISSION_EFFECT_TELEPORT;
		// Whether it moved a pedestrian or a car: a participant in a car
		// otherwise drives it to where only a pedestrian fits (MissionMoveFor).
		void *const me  = Func<PlayerFn>(FindPlayerPed)();
		void *const car = me && Field<uint8_t>(me, offs::PED_IN_VEHICLE) != 0
		                      ? Field<void *>(me, offs::PED_MY_VEHICLE)
		                      : nullptr;
		const int32_t named =
		    car ? NetIdForCar(Func<int32_t(__cdecl *)(void *)>(CPools__GetVehicleRef)(car)) : -1;
		body.ownerBlip = OwnerMoveTag(car != nullptr, named > 0 && named <= 0xFFFF
		                                                  ? static_cast<uint16_t>(named)
		                                                  : INVALID_NETID);
		break;
	}
	case replay::Kind::SphereNew:
		body.kind      = MISSION_EFFECT_SPHERE_NEW;
		body.ownerBlip = *reinterpret_cast<const int32_t *>(Params());   // what it stored
		break;
	case replay::Kind::FireNew:
		body.kind      = MISSION_EFFECT_FIRE_NEW;
		body.ownerBlip = *reinterpret_cast<const int32_t *>(Params());   // what it stored
		break;
	case replay::Kind::Holder: {
		body.kind           = MISSION_EFFECT_RUN;
		const int32_t where = HolderOfCarIn(enc);
		if (where == CAR_SIMULATED_HERE)
			return r;   // this machine's engine is the one, and it has run it
		if (where >= 0)
			body.onlyTo = static_cast<uint8_t>(where + 1);
		break;
	}
	case replay::Kind::Outfit: {
		// The owner's player's handle means nothing anywhere else; each
		// participant names its own.
		body.kind = MISSION_EFFECT_RUN;
		replay::SetLiteralAt(body.code, body.length, 0, 0);
		char look[PLAYER_LOOK_LEN];
		if (!g_saidOutfitSent && body.length >= 2 + 5 + TEXT_LABEL &&
		    LookFromLabel(body.code + 7, look)) {
			g_saidOutfitSent = true;
			Log("missions: %s dresses its player in '%s'; every participant's own player "
			    "changes too",
			    MissionName(g_own.number), look);
		}
		break;
	}
	case replay::Kind::PickupNew:
		body.kind      = MISSION_EFFECT_PICKUP_NEW;
		body.ownerBlip = *reinterpret_cast<const int32_t *>(Params());   // what it stored
		NoteMadePickup(body.ownerBlip);
		if (fromMission)
			RememberStash(enc, body.ownerBlip, body.ownerBlip);
		if (fromMission && command == op::CREATE_FLOATING_PACKAGE)
			NoteFloating(body.ownerBlip, body.ownerBlip);
		break;
	}
	NoteOwnWidget(enc);
	static int32_t flashing = HUD_ITEM_NONE;
	int32_t        item     = HUD_ITEM_NONE;
	if (command == op::FLASH_HUD_OBJECT && replay::LiteralAt(enc.code, enc.length, 0, &item) &&
	    item != flashing) {
		flashing = item;
		Log("missions: %s %s, and so does everybody's HUD", MissionName(body.missionNumber),
		    item == HUD_ITEM_NONE    ? "stops flashing the HUD"
		    : item == HUD_ITEM_RADAR ? "flashes the radar"
		                             : "flashes a HUD item");
	}
	// The session's other players would stand in the middle of the scene.
	if (command == op::START_CUTSCENE)
		SetCutsceneScene(true);
	else if (command == op::CLEAR_CUTSCENE)
		SetCutsceneScene(false);
	if (sceneRead)
		SendCutsceneModels(scene, missionNumber);
	ShapeEffect(body);
	if (!g_saidEffect) {
		g_saidEffect = true;
		Log("missions: what this mission shows goes to everybody");
	}
	return r;
}

// A main-script locate the host's own player failed: a guest standing in the
// marker of one of the host's contacts answers it instead
// (MissionSync::OtherPlayerAtOurContact). What follows is the trigger's own
// 03EE, claim and launch, run as though the host had walked in.
void AnswerForAGuest(void *script, int32_t command, uint16_t andOr, bool notFlag,
                     uint8_t before) {
	MissionArea area{};
	uint8_t     result = 0;
	// A yes changes nothing where an earlier condition of an `if and` said no.
	if (!ResultIfYes(andOr, notFlag, before, &result) || result == 0 ||
	    !AreaForCondition(command, Params(), &area))
		return;
	uint8_t who = INVALID_PLAYER;
	if (!g_client->Missions().OtherPlayerAtOurContact(area, LocalPlayerPos(), LocalId(), &who))
		return;
	At<uint8_t>(script, layout::SCRIPT_COND_RESULT) = result;
	RememberArea(script, area);
	static uint8_t saidWho = INVALID_PLAYER;
	static Vec3    saidAt  = {};
	if (who != saidWho || saidAt.x != area.centre.x || saidAt.y != area.centre.y) {
		saidWho = who;
		saidAt  = area.centre;
		const char *nick = g_client->NickFor(who);
		Log("missions: %s stands in the marker of our contact at (%.1f %.1f); our trigger "
		    "takes it for our own player",
		    nick ? nick : "a player", area.centre.x, area.centre.y);
	}
}

// A location check of the owner, asked in the block where IS_PLAYER_IN_CAR was
// just answered for a participant in the car: yes when that car stands at the
// place, stopped where the check asks for stopped (standin.h). The Thieves and
// Don't Spank Ma Bitch Up ask "resprayed, in the car, stopped in the
// Pay'n'Spray" in that order, which a guest driving the car never passed.
void CarAtThePlace(void *script, int32_t command, uint16_t andOr, bool notFlag, uint8_t before) {
	standin::PlaceNeeds needs{};
	if (!g_client || g_own.finishing || g_inCarScript != script || g_inCarFrame != g_frame ||
	    !g_locationHasArea || !standin::PlaceNeedsOf(command, &needs))
		return;
	void *const car = Func<GetPedFn>(CPools__GetVehicle)(g_inCarHandle);
	if (!car)
		return;
	const float *v = &Field<float>(car, offs::MOVE_SPEED);
	if (!standin::CarAnswersPlace(needs, standin::CarStopped(v[0], v[1], v[2])))
		return;
	const float *at = &Field<float>(car, offs::POSITION);
	if (!InCarAtThePlace(Vec3{at[0], at[1], at[2]}, true, g_locationArea))
		return;
	At<uint8_t>(script, layout::SCRIPT_COND_RESULT) = CompareFlagIfTrue(before, andOr, notFlag);
	if (!g_saidCarAtPlace) {
		g_saidCarAtPlace = true;
		Log("missions: %s asks whether its player is at the place after asking about car %d; "
		    "a participant has the car there, and that answers it",
		    MissionName(g_own.number), g_inCarHandle);
	}
}

// Sayonara Salvatore's guards (standin.h): whom the two place checks right
// after a spotting are about, when the spotting was answered for somebody
// other than the owner, or for him in one of the two places with a
// participant seen in the open.
struct SpotStandIn {
	const void *script = nullptr;
	uint32_t    ip     = 0;   // the spotting's, where its operands start
	uint32_t    frame  = 0;
	uint8_t     who    = INVALID_PLAYER;
	Vec3        at{};
};
SpotStandIn g_spotStandIn;
bool        g_saidSpotStandIn  = false;

// One of the two place checks after a spotting: answered for where the
// participant spotted stands. True when it was.
bool AskedOfSpotted(void *script, int32_t command, uint32_t ip, uint16_t andOr, bool notFlag) {
	if (g_own.number != standin::SAYONARA_SALVATORE || andOr != ANDOR_NONE ||
	    g_spotStandIn.script != script || g_spotStandIn.frame != g_frame ||
	    !standin::AsksAfterSpotting(g_spotStandIn.ip, ip))
		return false;
	const int box = standin::SafeBoxOf(command, Params() + 1);
	if (box < 0)
		return false;
	const Vec3 &at = g_spotStandIn.at;
	const bool  in = standin::InBox(standin::SPOTTED_DOES_NOT_COUNT[box], at.x, at.y, at.z);
	At<uint8_t>(script, layout::SCRIPT_COND_RESULT) = in != notFlag ? 1 : 0;
	if (!g_saidSpotStandIn) {
		g_saidSpotStandIn = true;
		Log("missions: %s asks where its player is after a guard spotted %s; answered for where %s "
		    "stands",
		    MissionName(g_own.number), g_client->PlayerSlot(g_spotStandIn.who).nick.c_str(),
		    g_client->PlayerSlot(g_spotStandIn.who).nick.c_str());
	}
	return true;
}

void WakaCarpark(void *script, int32_t command, uint16_t andOr, bool notFlag);
void *ReplicaOf(uint8_t playerId);
void *SeatedCar(void *ped);

// A place the group answers as one (the owner's no becomes the yes it would
// have been, through the block's own flag, when a participant is inside the
// area as the check asks: on foot, in a car, stopped, standin.h PlaceNeedsOf).
// Two tables first: nearchar.h's places anybody answers (Smack Down's
// dealers, Shima's Diablos, Liberator's garage doors and compound,
// Bling-Bling Scramble's checkpoints), which are then no checkpoint for
// everybody to reach, and standin.h's boxes anybody keeps (Salvatore's garage
// in Sayonara Salvatore, so its door shuts only on nobody), which still are.
// Everywhere else the general rule (anyplace.h): anybody there with the owner
// nearby, a checkpoint as before. True for a site of the first table.
bool g_saidAnybodyAtPlace = false;

bool PedDown(void *ped);
bool Wrecked(void *car);
bool SkipOperand(uint32_t &ip);

// Participant `id` as the owner's machine sees him at a place: where his
// state puts him, whether he sits in a car and whether he has stopped (his
// copy's car here, or his own speed on foot), and whether that car is
// `storedCar`. Not valid for anybody outside the owner's mission.
anyplace::Candidate PlaceCandidate(uint8_t id, void *storedCar) {
	anyplace::Candidate c{};
	if (id == LocalId() || (g_client->Missions().Participants() & PlayerBit(id)) == 0)
		return c;
	const RemotePlayer &pl = g_client->PlayerSlot(id);
	if (!pl.active || !pl.haveState)
		return c;
	void *const car = SeatedCar(ReplicaOf(id));
	c.valid         = true;
	c.at            = Vec3{pl.last.pos.x, pl.last.pos.y, pl.last.pos.z};
	c.seated        = pl.Seated();
	if (car) {
		const float *v = &Field<float>(car, offs::MOVE_SPEED);
		c.stopped      = standin::CarStopped(v[0], v[1], v[2]);
	} else {
		c.stopped = !c.seated && standin::CarStopped(pl.last.moveSpeed.x, pl.last.moveSpeed.y,
		                                             pl.last.moveSpeed.z);
	}
	c.inStoredCar = storedCar && car == storedCar;
	return c;
}

// The participant who satisfies the place, or INVALID_PLAYER.
uint8_t WhoAnswersPlace(const standin::PlaceNeeds &needs, const MissionArea &area, void *storedCar) {
	anyplace::Candidate c[MAX_PLAYERS];
	for (uint8_t id = 0; id < MAX_PLAYERS; ++id)
		c[id] = PlaceCandidate(id, storedCar);
	const int who = anyplace::WhoAnswers(needs, area, c, MAX_PLAYERS, storedCar != nullptr);
	return who < 0 ? INVALID_PLAYER : static_cast<uint8_t>(who);
}

// ---- anybody at the place, the owner nearby (anyplace.h) ---------------------------
//
// The general rule, after the two tables. The block a place is asked in is
// followed from its `if` to its goto_if_false (AnyplaceInstruction, from
// MissionInstruction), for the cars asked beside it and for the yes that is
// only settled there (anyplace.h, PlanFor).

struct AnyplacePending {
	anyplace::Plan plan;
	MissionArea    area;
};

constexpr size_t ANYPLACE_MAX = 4;

struct AnyplaceBlock {
	const void     *script = nullptr;
	uint32_t        frame  = 0;
	bool            anyCar = false;   // IS_PLAYER_IN_ANY_CAR and the like beside it
	int32_t         cars[ANYPLACE_MAX] = {};
	uint8_t         carCount           = 0;
	AnyplacePending pending[ANYPLACE_MAX];
	uint8_t         pendingCount = 0;
};
AnyplaceBlock g_anyBlock;

// The car the mission stored as its player's (STORE_CAR_PLAYER_IS_IN), and
// in which mission.
int32_t  g_storedCar         = -1;
uint16_t g_storedCarMission  = MISSION_NONE;
bool     g_saidAnyplace      = false;
bool     g_saidAnyplaceUndone = false;
bool     g_saidAnyplaceOwners = false;
bool     g_saidAnyplaceTarget = false;

void ForgetAnyplace() {
	g_anyBlock           = AnyplaceBlock{};
	g_storedCar          = -1;
	g_storedCarMission   = MISSION_NONE;
	g_saidAnyplace       = false;
	g_saidAnyplaceUndone = false;
	g_saidAnyplaceOwners = false;
	g_saidAnyplaceTarget = false;
	g_askedCar           = -1;
	g_askedCarMission    = MISSION_NONE;
	g_saidDecoyEnd       = false;
}

bool InAnyplaceBlock(const void *script) {
	return g_anyBlock.script == script && g_anyBlock.frame == g_frame;
}

// The stored car, while it is still a car, or null.
void *AnyplaceStoredCar() {
	if (g_storedCar < 0 || g_storedCarMission != g_own.number)
		return nullptr;
	void *const car = Func<GetPedFn>(CPools__GetVehicle)(g_storedCar);
	return car && !Wrecked(car) ? car : nullptr;
}

// The owner's own ped is the script's: one of its scenes, or the walk out of
// the car at the casino.
bool OwnersSceneRuns() {
	const uint8_t controls = Global<uint8_t>(CPad__Pads + pad::DISABLE_PLAYER_CONTROLS);
	return g_sceneHidden || g_walkedOut.held ||
	       (controls & (pad::PLAYERCONTROL_PLAYERINFO | pad::PLAYERCONTROL_CUTSCENE)) != 0;
}

// Whether a car asked beside the place does not stand at it.
bool AnyplaceConflict(const AnyplaceBlock &b, const MissionArea *areas, size_t n) {
	if (b.anyCar)
		return true;
	for (uint8_t i = 0; i < b.carCount; ++i) {
		void *const car = Func<GetPedFn>(CPools__GetVehicle)(b.cars[i]);
		if (!car)
			return true;
		const float *at   = &Field<float>(car, offs::POSITION);
		bool         here = false;
		for (size_t k = 0; k < n && !here; ++k)
			here = InCarAtThePlace(Vec3{at[0], at[1], at[2]}, true, areas[k]);
		if (!here)
			return true;
	}
	return false;
}

// Every instruction of the owner's mission script, before it runs.
void AnyplaceInstruction(void *script, int32_t command) {
	if (command == OP_ANDOR) {
		g_anyBlock        = AnyplaceBlock{};
		g_anyBlock.script = script;
		g_anyBlock.frame  = g_frame;
		return;
	}
	if (!InAnyplaceBlock(script))
		return;
	if (command == OP_GOTO_IF_FALSE) {
		const AnyplaceBlock b = g_anyBlock;
		g_anyBlock            = AnyplaceBlock{};
		if (b.pendingCount == 0)
			return;
		MissionArea areas[ANYPLACE_MAX];
		for (uint8_t i = 0; i < b.pendingCount; ++i)
			areas[i] = b.pending[i].area;
		const bool conflict = AnyplaceConflict(b, areas, b.pendingCount);
		for (uint8_t i = 0; i < b.pendingCount; ++i)
			if (anyplace::SettleWrites(b.pending[i].plan, conflict))
				At<uint8_t>(script, layout::SCRIPT_COND_RESULT) = b.pending[i].plan.settleTo;
		if (conflict && !g_saidAnyplaceUndone) {
			g_saidAnyplaceUndone = true;
			Log("missions: %s asks where its player is and which car he is in, in one block; the "
			    "car is not at the place, so the owner answers it himself",
			    MissionName(g_own.number));
		}
		return;
	}
	switch (anyplace::BesideOf(command)) {
	case anyplace::Beside::AnyCar: g_anyBlock.anyCar = true; break;
	case anyplace::Beside::Car:
		if (g_anyBlock.carCount == ANYPLACE_MAX) {
			g_anyBlock.anyCar = true;
			break;
		}
		PeekParams(script, 2);
		g_anyBlock.cars[g_anyBlock.carCount++] = reinterpret_cast<const int32_t *>(Params())[1];
		break;
	default: break;
	}
}

// A handle the owner's mission just stored in global `at`, by the
// instruction whose operands start at `ip`: the car STORE_CAR_PLAYER_IS_IN
// stored is the mission's player's.
void AnyplaceStored(uint32_t ip, uint16_t at) {
	if (ip < 2 || ip > SCRIPT_SPACE_SIZE || at + 4u > SCRIPT_SPACE_SIZE)
		return;
	const uint8_t *s = Space();
	if ((s[ip - 2] | (s[ip - 1] << 8)) != anyplace::OP_STORE_CAR_PLAYER_IS_IN)
		return;
	if (anyplace::StoreIsOnlyATarget(g_own.number)) {
		if (!g_saidAnyplaceTarget) {
			g_saidAnyplaceTarget = true;
			Log("missions: %s stores its player's car only for its goons to destroy; its places "
			    "stay anybody's",
			    MissionName(g_own.number));
		}
		return;
	}
	std::memcpy(&g_storedCar, s + at, 4);
	g_storedCarMission = g_own.number;
}

// The general rule: the owner's no becomes the block's yes when a
// participant is at the place as the check asks and the owner is within
// anyplace::OWNER_NEARBY_M of it. `params` are the operands as the handler
// collected them (null for a zone, which has its label there instead).
void AnybodyNearby(void *script, int32_t command, uint16_t andOr, bool notFlag, uint8_t before,
                   const standin::PlaceNeeds &needs, const MissionArea &area, const float *params) {
	if (!g_client || !g_own.running || g_own.finishing || !IsMissionScript(script))
		return;
	const anyplace::Plan plan = anyplace::PlanFor(andOr, notFlag);
	const uint8_t        yes  = CompareFlagIfTrue(before, andOr, notFlag);
	if (!plan.valid || At<uint8_t>(script, layout::SCRIPT_COND_RESULT) == yes)
		return;
	if (const char *why = anyplace::ExcludedWhy(g_own.number, command, params)) {
		if (!g_saidAnyplaceOwners) {
			g_saidAnyplaceOwners = true;
			Log("missions: %s asks where its player is, and only the owner answers: %s",
			    MissionName(g_own.number), why);
		}
		return;
	}
	if (OwnersSceneRuns() || !anyplace::OwnerNearby(area, LocalPlayerPos()))
		return;
	if (plan.settle && (!InAnyplaceBlock(script) || g_anyBlock.pendingCount == ANYPLACE_MAX ||
	                    AnyplaceConflict(g_anyBlock, &area, 1)))
		return;
	const uint8_t who = WhoAnswersPlace(needs, area, AnyplaceStoredCar());
	if (who == INVALID_PLAYER)
		return;
	if (plan.now)
		At<uint8_t>(script, layout::SCRIPT_COND_RESULT) = yes;
	if (plan.settle)
		g_anyBlock.pending[g_anyBlock.pendingCount++] = AnyplacePending{plan, area};
	if (!g_saidAnyplace) {
		g_saidAnyplace = true;
		Log("missions: %s asks whether its player is at (%.1f %.1f); %s is, with the owner "
		    "nearby, and that answers it",
		    MissionName(g_own.number), area.centre.x, area.centre.y,
		    g_client->PlayerSlot(who).nick.c_str());
	}
}

bool AnybodyAtPlace(void *script, int32_t command, uint16_t andOr, bool notFlag, uint8_t before) {
	const float        *p = Params();
	standin::PlaceNeeds needs{};
	MissionArea         area{};
	if (g_own.finishing || !standin::PlaceNeedsOf(command, &needs) ||
	    !AreaForCondition(command, p, &area))
		return false;
	const bool place = nearchar::AnswersPlaceForAnybody(g_own.number, command, p);
	const bool keeps = !place && standin::AreaAnybodyKeeps(g_own.number, command, p + 1) != nullptr;
	if (!place && !keeps) {
		AnybodyNearby(script, command, andOr, notFlag, before, needs, area, p);
		return false;
	}
	const uint8_t yes = CompareFlagIfTrue(before, andOr, notFlag);
	if (At<uint8_t>(script, layout::SCRIPT_COND_RESULT) == yes)
		return place;
	const uint8_t who = WhoAnswersPlace(needs, area, nullptr);
	if (who == INVALID_PLAYER)
		return place;
	At<uint8_t>(script, layout::SCRIPT_COND_RESULT) = yes;
	if (!g_saidAnybodyAtPlace) {
		g_saidAnybodyAtPlace = true;
		Log("missions: %s asks whether its player is at (%.1f %.1f); %s is, and that answers it",
		    MissionName(g_own.number), area.centre.x, area.centre.y,
		    g_client->PlayerSlot(who).nick.c_str());
	}
	return place;
}

// IS_PLAYER_IN_ZONE (0121) in the owner's mission: the zone is read the way
// the handler reads it, its label out of the script after the player, and
// looked up in the engine's own ZoneArray (addresses.h, zones).
bool ZoneAreaOf(const char (&label)[zones::ZONE_NAME_LEN], MissionArea *out) {
	const uint16_t n = Global<uint16_t>(zones::CTheZones__TotalNumberOfZones);
	if (n > zones::MAX_ZONES)
		return false;
	for (uint16_t i = 0; i < n; ++i) {
		const uint8_t *z = Ptr<uint8_t>(zones::CTheZones__ZoneArray + i * zones::SIZEOF_ZONE);
		if (std::memcmp(z + zones::ZONE_NAME, label, zones::ZONE_NAME_LEN) != 0)
			continue;
		*out = anyplace::ZoneArea(reinterpret_cast<const float *>(z + zones::ZONE_MIN),
		                          reinterpret_cast<const float *>(z + zones::ZONE_MAX));
		return true;
	}
	return false;
}

int8_t PlayerInZone(void *script, int32_t command, RangeFn original) {
	const uint16_t andOr                       = At<uint16_t>(script, layout::SCRIPT_AND_OR);
	const bool     notFlag                     = At<uint8_t>(script, layout::SCRIPT_NOT) != 0;
	const uint8_t  before                      = At<uint8_t>(script, layout::SCRIPT_COND_RESULT);
	char           label[zones::ZONE_NAME_LEN] = {};
	bool           haveLabel                   = false;
	if (g_client && g_own.running && !g_own.finishing && IsMissionScript(script)) {
		// ReadTextLabelFromScript's strncpy: up to the first NUL, zeros after.
		uint32_t ip = At<uint32_t>(script, layout::SCRIPT_IP);
		if (SkipOperand(ip) && ip + zones::ZONE_NAME_LEN <= SCRIPT_SPACE_SIZE) {
			for (size_t i = 0; i < zones::ZONE_NAME_LEN && Space()[ip + i] != 0; ++i)
				label[i] = static_cast<char>(Space()[ip + i]);
			haveLabel = true;
		}
	}
	const int8_t r = original(script, nullptr, command);
	MissionArea  area{};
	if (haveLabel && ZoneAreaOf(label, &area))
		AnybodyNearby(script, command, andOr, notFlag, before, standin::PlaceNeeds{}, area, nullptr);
	return r;
}

bool InOwnMission(uint8_t id);

// Decoy's end (standin.h, IsDecoyEnd): with a participant in the decoy van
// and the owner not in it, "the player more than 160 m from the warehouse"
// is asked of the van, the owner's own answer replaced, either way. The van
// is the car the mission asks IS_PLAYER_IN_CAR about. True when answered.
bool DecoyEnd(void *script, int32_t command, uint16_t andOr, bool notFlag) {
	if (!g_client || !g_own.running || g_own.finishing || andOr != ANDOR_NONE ||
	    g_askedCarMission != g_own.number || !IsMissionScript(script))
		return false;
	const float *p = Params();
	if (!standin::IsDecoyEnd(g_own.number, command, p[1], p[2], p[3], p[4]))
		return false;
	void *const van = g_askedCar >= 0 ? Func<GetPedFn>(CPools__GetVehicle)(g_askedCar) : nullptr;
	if (!van || Wrecked(van))
		return false;
	void *const me = Func<PlayerFn>(FindPlayerPed)();
	bool        inVan[MAX_PLAYERS] = {};
	for (uint8_t id = 0; id < MAX_PLAYERS; ++id)
		inVan[id] = id != LocalId() && InOwnMission(id) && ReplicaSeatedIn(id, van);
	const int who = standin::DecoyRider(me && SeatedCar(me) == van, inVan, MAX_PLAYERS);
	if (who < 0)
		return false;
	const float *at  = &Field<float>(van, offs::POSITION);
	const bool   raw = nearchar::InLocateBox(at[0] - p[1], at[1] - p[2], 0.0f, p[3], p[4], 0.0f, false);
	At<uint8_t>(script, layout::SCRIPT_COND_RESULT) = notFlag != raw ? 1 : 0;
	if (!g_saidDecoyEnd) {
		g_saidDecoyEnd = true;
		Log("missions: %s asks how far its player is from the warehouse; %s drives the decoy van, "
		    "and the van answers it (%s)",
		    MissionName(g_own.number), g_client->PlayerSlot(static_cast<uint8_t>(who)).nick.c_str(),
		    raw ? "still near" : "far enough");
	}
	return true;
}

int8_t Location(void *script, int32_t command, RangeFn original) {
	const uint16_t andOr    = At<uint16_t>(script, layout::SCRIPT_AND_OR);
	const bool     notFlag  = At<uint8_t>(script, layout::SCRIPT_NOT) != 0;
	const uint8_t  before   = At<uint8_t>(script, layout::SCRIPT_COND_RESULT);
	const uint32_t ip       = At<uint32_t>(script, layout::SCRIPT_IP);
	const bool     mayForce = MayForceCondition(andOr, notFlag);
	const int8_t   r        = original(script, nullptr, command);
	if (g_client && At<uint8_t>(script, layout::SCRIPT_COND_RESULT) == 0 &&
	    !IsMissionScript(script)) {
		AnswerForAGuest(script, command, andOr, notFlag, before);
		return r;
	}
	if (g_client && g_own.running && IsMissionScript(script)) {
		if (AskedOfSpotted(script, command, ip, andOr, notFlag))
			return r;
		if (DecoyEnd(script, command, andOr, notFlag))
			return r;
		if (InGetaway(script) && GetawayPlace(script, command, andOr, notFlag, before))
			return r;
	}
	if (g_own.running && IsMissionScript(script)) {
		g_locationScript  = script;
		g_locationFrame   = g_frame;
		g_locationAndOr   = andOr;
		g_locationHasArea = AreaForCondition(command, Params(), &g_locationArea);
		CarAtThePlace(script, command, andOr, notFlag, before);
		WakaCarpark(script, command, andOr, notFlag);
		if (g_client && AnybodyAtPlace(script, command, andOr, notFlag, before))
			return r;
	}
	if (!g_client || !mayForce || At<uint8_t>(script, layout::SCRIPT_COND_RESULT) == 0)
		return r;
	MissionArea area{};
	if (!AreaForCondition(command, Params(), &area))
		return r;
	if (!IsMissionScript(script)) {
		RememberArea(script, area);
		return r;
	}
	if (!g_own.running || !HoldsCoordBlip(area, g_blips, g_blipCount))
		return r;
	if (!g_client->Missions().AskCheckpoint(area, LocalId(), WallClock::NowMs())) {
		At<uint8_t>(script, layout::SCRIPT_COND_RESULT) = 0;
		if (!g_saidCheckpoint) {
			g_saidCheckpoint = true;
			Log("missions: a checkpoint of %s waits for everybody", MissionName(g_own.number));
		}
	}
	return r;
}

int8_t BlipAdded(void *script, int32_t command, RangeFn original) {
	const bool mission = IsMissionScript(script);
	float      x = 0.0f, y = 0.0f;
	if (mission) {
		PeekParams(script, 2);
		x = Params()[0];
		y = Params()[1];
	}
	const int8_t r = Record(script, command, original);
	if (mission && g_own.running)
		AddBlip(*reinterpret_cast<const int32_t *>(Params()), x, y);
	return r;
}

int8_t BlipRemoved(void *script, int32_t command, RangeFn original) {
	int32_t handle = -1;
	if (IsMissionScript(script)) {
		PeekParams(script, 1);
		handle = *reinterpret_cast<const int32_t *>(Params());
	}
	const int8_t r = Record(script, command, original);
	if (handle != -1)
		DropBlip(handle);
	return r;
}

// HAS_MODEL_LOADED and HAS_SPECIAL_CHARACTER_LOADED in the session's mission,
// while a participant is still loading what its LOAD_ALL_MODELS_NOW asked for:
// held where it is, so the mission's own wait goes on until everybody's
// models are in and nobody's cutscene starts behind (missions.md 11.3). Held
// rather than answered, since the wait asks under a NOT and an OR: an
// instruction that has not run leaves what the script is working out as it
// was.
int8_t ModelsLoaded(void *script, int32_t command, RangeFn original) {
	if (g_client && g_own.running && g_readySeq != 0 && IsMissionScript(script) &&
	    !g_client->Missions().AskEverybodyLoaded(g_readySeq, LocalId(), WallClock::NowMs())) {
		if (!g_saidReadyWait) {
			g_saidReadyWait = true;
			Log("missions: %s waits for everybody's models before it goes on",
			    MissionName(g_own.number));
		}
		At<uint32_t>(script, layout::SCRIPT_IP) -= 2;   // this instruction again next frame
		return 1;
	}
	return original(script, nullptr, command);
}

int8_t Passed(void *script, int32_t command, RangeFn original) {
	if (IsMissionScript(script) && g_own.running)
		g_own.passed = true;
	return Record(script, command, original);
}

// What an instruction of the session's mission is, while it runs. Nested
// calls leave it as the outer one set it.
void NoteWrite(uint16_t at, bool handle, int from = -1);

// Also where a handle the instruction made lands: when it is one of the
// owner's mission's that stores a handle, the global it went into holds a
// handle now, and the delta leaves it out (mission.h, WritesHandle).
struct MissionInstruction {
	bool     was;
	void    *handleScript = nullptr;
	uint32_t handleIp     = 0;
	MissionInstruction(void *script, int32_t command) : was(g_missionInstruction) {
		if (g_own.running && IsMissionScript(script)) {
			g_missionInstruction = true;
			AnyplaceInstruction(script, command);
			if (WritesHandle(command)) {
				handleScript = script;
				handleIp     = At<uint32_t>(script, layout::SCRIPT_IP);
			}
		}
	}
	~MissionInstruction() {
		g_missionInstruction = was;
		if (handleScript) {
			const uint32_t after = At<uint32_t>(handleScript, layout::SCRIPT_IP);
			const uint16_t at    = HandleOutputGlobal(Space(), SCRIPT_SPACE_SIZE, handleIp, after);
			if (at != 0) {
				NoteWrite(at, true);
				AnyplaceStored(handleIp, at);
			}
		}
	}
	MissionInstruction(const MissionInstruction &)            = delete;
	MissionInstruction &operator=(const MissionInstruction &) = delete;
};

// ---- the mission's enemies go for everybody (missions.md 5.3) -----------------

void RunOurs(uint16_t opcode, std::initializer_list<int32_t> values);

// A remote player's replica here, alive and ours to point at, or null.
void *ReplicaOf(uint8_t playerId) {
	if (!g_client || playerId >= MAX_PLAYERS || playerId == LocalId())
		return nullptr;
	const RemotePlayer &p = g_client->PlayerSlot(playerId);
	if (!p.active || p.poolHandle < 0)
		return nullptr;
	void *const ped = Func<GetPedFn>(CPools__GetPed)(p.poolHandle);
	if (!ped || Field<uintptr_t>(ped, offs::VTABLE) != CCivilianPed__vtable)
		return nullptr;
	return ped;
}

// Whose replica `ped` is, or INVALID_PLAYER.
uint8_t PlayerOfReplica(void *ped) {
	for (uint8_t id = 0; ped && id < MAX_PLAYERS; ++id)
		if (ReplicaOf(id) == ped)
			return id;
	return INVALID_PLAYER;
}

float DistanceSq(void *a, void *b) {
	const float *pa = &Field<float>(a, offs::POSITION);
	const float *pb = &Field<float>(b, offs::POSITION);
	const float  dx = pa[0] - pb[0], dy = pa[1] - pb[1], dz = pa[2] - pb[2];
	return dx * dx + dy * dy + dz * dz;
}

// An enemy of the session's mission made tougher for the players in it
// (docs/missions.md 10.2): its health and armour times the session's
// factor, once, through the engine's own SET_CHAR_HEALTH and
// ADD_ARMOUR_TO_CHAR. The script waits for it to die as it always does.

void Toughen(int32_t charHandle, void *npc) {
	const float factor = g_client->Missions().EnemyToughness();
	if (factor <= 1.0f || g_toughenedCount == MAX_TOUGHENED)
		return;
	for (size_t i = 0; i < g_toughenedCount; ++i)
		if (g_toughened[i] == charHandle)
			return;
	g_toughened[g_toughenedCount++] = charHandle;
	const float health = Field<float>(npc, offs::PED_HEALTH);
	const float armour = Field<float>(npc, offs::PED_ARMOUR);
	RunOurs(op::SET_CHAR_HEALTH, {charHandle, static_cast<int32_t>(health * factor + 0.5f)});
	if (armour > 0.0f)
		RunOurs(op::ADD_ARMOUR_TO_CHAR,
		        {charHandle, static_cast<int32_t>(armour * (factor - 1.0f) + 0.5f)});
	static bool said = false;
	if (!said) {
		said = true;
		Log("missions: %s's enemies are %.2f times as tough for the players in it",
		    MissionName(g_own.number), static_cast<double>(factor));
	}
}

// ---- who the enemies may go for --------------------------------------------------
//
// Every player in the mission: this machine's own, and each participant's
// replica. Before the server has said who is in it, every replica here.

bool PedDown(void *ped) {
	const uint32_t state = Field<uint32_t>(ped, offs::PED_STATE);
	return state == PEDSTATE_DIE || state == PEDSTATE_DEAD ||
	       !(Field<float>(ped, offs::PED_HEALTH) > 0.0f);
}

bool InOwnMission(uint8_t id) {
	const MissionSync &m = g_client->Missions();
	if (!m.Running() || m.Owner() != LocalId())
		return true;
	return (m.Participants() & PlayerBit(id)) != 0;
}

// The ped of player `id` here, standing: ours, or a participant's replica.
void *PlayerPedHere(uint8_t id) {
	void *ped = nullptr;
	if (id == LocalId())
		ped = Func<PlayerFn>(FindPlayerPed)();
	else if (InOwnMission(id))
		ped = ReplicaOf(id);
	return ped && !PedDown(ped) ? ped : nullptr;
}

bool Wrecked(void *car) {
	return (Field<uint8_t>(car, offs::ENTITY_FLAGS) >> ENTITY_STATUS_SHIFT) == ENTITY_STATUS_WRECKED;
}

// The car `ped` sits in, still a car, or null.
void *SeatedCar(void *ped) {
	if (!ped || Field<uint8_t>(ped, offs::PED_IN_VEHICLE) == 0)
		return nullptr;
	void *const car = Field<void *>(ped, offs::PED_MY_VEHICLE);
	return car && !Wrecked(car) ? car : nullptr;
}

bool ReplicaSeatedIn(uint8_t id, void *car) {
	return car && SeatedCar(ReplicaOf(id)) == car;
}

// ---- a session car the mission clears away (mission.h, ClearTakesCar) ------------

int32_t FloatBits(float f) {
	int32_t v = 0;
	std::memcpy(&v, &f, 4);
	return v;
}

Vec3 EntityPos(void *entity) {
	const float *p = &Field<float>(entity, offs::POSITION);
	return {p[0], p[1], p[2]};
}

float CarRadius(void *car) {
	return Func<float(__thiscall *)(void *)>(CEntity__GetBoundRadius)(car);
}

// Every car of the engine's pool, the way its own walks go (game/ride.cpp).
template <typename F>
void ForEachVehicle(F f) {
	auto *const pool = Global<uint8_t *>(CPools__ms_pVehiclePool);
	if (!pool)
		return;
	uint8_t *const entries = Field<uint8_t *>(pool, object::POOL_ENTRIES);
	uint8_t *const flags   = Field<uint8_t *>(pool, object::POOL_FLAGS);
	const int32_t  size    = Field<int32_t>(pool, object::POOL_SIZE);
	if (!entries || !flags || size <= 0 || size > VEHICLE_POOL_SIZE)
		return;
	for (int32_t i = 0; i < size; ++i)
		if ((flags[i] & object::POOLFLAG_ISFREE) == 0)
			f(static_cast<void *>(entries + static_cast<size_t>(i) * offs::SIZEOF_AUTOMOBILE));
}

int32_t CarHandle(void *car) { return Func<int32_t(__cdecl *)(void *)>(CPools__GetVehicleRef)(car); }

// A point beside `car`, across it, on the first side the buildings leave
// clear of it. The ground under it is the engine's to find (a z of -100).
void BesideCar(void *car, float *x, float *y) {
	const float *right = &Field<float>(car, offs::MATRIX_RIGHT);
	const Vec3   at    = EntityPos(car);
	const float  d     = LeaveCarDistance(CarRadius(car));
	for (float side : {1.0f, -1.0f}) {
		const float cx = at.x + side * right[0] * d, cy = at.y + side * right[1] * d;
		const Vec3f from{at.x, at.y, at.z + 1.0f}, to{cx, cy, at.z + 1.0f};
		if (Func<LineOfSightFn>(CWorld__GetIsLineOfSightClear)(&from, &to, 1, 0, 0, 1, 0, 0, 0) != 0) {
			*x = cx;
			*y = cy;
			return;
		}
	}
	*x = at.x + right[0] * d;
	*y = at.y + right[1] * d;
}

// Our own player out of `car`, or off its roof, and down beside it on foot
// (mission.h, LeaveCarEffect). Nothing when he is neither.
void LeaveCarHere(void *car) {
	void *const me = Func<PlayerFn>(FindPlayerPed)();
	if (!me || !car)
		return;
	float x = 0.0f, y = 0.0f;
	if (Field<uint8_t>(me, offs::PED_IN_VEHICLE) != 0) {
		if (Field<void *>(me, offs::PED_MY_VEHICLE) != car)
			return;
		BesideCar(car, &x, &y);
		// The warp nils m_pMyVehicle under whatever door animation he is in
		// (game/animcb.h): its callbacks come off first, on this car.
		LetGoOfCarChain(me);
		RunOurs(op::WARP_PLAYER_FROM_CAR_TO_COORD, {0, FloatBits(x), FloatBits(y), FloatBits(-100.0f)});
		Log("missions: the owner's mission takes away the car we were in; we are out of it, "
		    "beside it at (%.1f, %.1f)",
		    x, y);
		return;
	}
	if (!StandingOnCar(EntityPos(me), EntityPos(car), CarRadius(car)))
		return;
	BesideCar(car, &x, &y);
	RunOurs(op::SET_PLAYER_COORDINATES, {0, FloatBits(x), FloatBits(y), FloatBits(-100.0f)});
	Log("missions: the owner's mission takes away the car we stood on; we are down beside it "
	    "at (%.1f, %.1f)",
	    x, y);
}

// Another player in `car`, the session car `netId`: his copy in a seat of it
// here, or the session's word that he sits in it or drives it.
bool OtherPlayerIn(void *car, uint16_t netId) {
	for (uint8_t id = 0; id < MAX_PLAYERS; ++id) {
		if (id == LocalId())
			continue;
		const RemotePlayer &p = g_client->PlayerSlot(id);
		if (p.active && (p.seatVehicleNetId == netId || ReplicaSeatedIn(id, car)))
			return true;
	}
	const RemoteVehicle *row = g_client->VehicleByNetId(netId);
	return row && row->driverPlayerId != INVALID_PLAYER && row->driverPlayerId != LocalId();
}

// One of the mission's people in a seat of `car`: a character the engine
// keeps for a script, and not a player's copy.
bool MissionPedIn(void *car) {
	void *const me = Func<PlayerFn>(FindPlayerPed)();
	for (size_t seat = 0; seat <= offs::VEH_MAX_PASSENGERS; ++seat) {
		void *const in = seat == 0 ? Field<void *>(car, offs::VEH_DRIVER)
		                           : Field<void *>(car, offs::VEH_PASSENGERS + (seat - 1) * 4);
		if (in && in != me && PlayerOfReplica(in) == INVALID_PLAYER &&
		    Field<uint8_t>(in, offs::PED_CHAR_CREATED_BY) == CHAR_CREATED_BY_MISSION)
			return true;
	}
	return false;
}

// A session car the owner's mission takes away, out of this world and, by
// C_VehicleRemoved, out of everybody else's (game/carremoval.h).
void TakeSessionCar(int32_t handle, uint16_t netId) {
	void *const car = handle >= 0 ? VehicleAt(handle) : nullptr;
	if (!car)
		return;
	Func<void(__thiscall *)(void *)>(CPhysical__RemoveFromMovingList)(car);
	ForgetEngineRawPointersTo(car);
	NoteCarTakenAway(netId, VEHICLE_REMOVED_MISSION);
	RunOurs(op::DELETE_CAR, {handle});
	Log("missions: %s takes session car %u away, on every machine", MissionName(g_own.number),
	    static_cast<unsigned>(netId));
}

// While a CLEAR_AREA of the session's mission runs here, the owner's or the
// replay's: every session car is locked, so the engine's own clear takes none
// of them, with whoever sits in it, nor one the session would put back. A
// session car goes by TakeSessionCar alone.
class SessionCarsLocked {
public:
	explicit SessionCarsLocked(bool active) {
		if (!active || !g_client)
			return;
		ForEachVehicle([this](void *car) {
			uint8_t &flags = Field<uint8_t>(car, offs::VEH_FLAGS_A);
			if ((flags & offs::VEH_IS_LOCKED) != 0 || m_count == MAX ||
			    g_client->SessionCarNetIdOf(CarHandle(car)) == INVALID_NETID)
				return;
			flags                = static_cast<uint8_t>(flags | offs::VEH_IS_LOCKED);
			m_handles[m_count++] = CarHandle(car);
		});
	}
	~SessionCarsLocked() {
		for (size_t i = 0; i < m_count; ++i)
			if (void *const car = VehicleAt(m_handles[i])) {
				uint8_t &flags = Field<uint8_t>(car, offs::VEH_FLAGS_A);
				flags          = static_cast<uint8_t>(flags & ~offs::VEH_IS_LOCKED);
			}
	}
	SessionCarsLocked(const SessionCarsLocked &)            = delete;
	SessionCarsLocked &operator=(const SessionCarsLocked &) = delete;

private:
	static constexpr size_t MAX = 64;
	int32_t m_handles[MAX] = {};
	size_t  m_count        = 0;
};

// The cars the owner's mission is taking away, and the instruction it is held
// at until no other player sits in them (mission.h, CLEAR_HOLD_MS).
constexpr size_t MAX_TAKEN_CARS = 16;
struct TakenCar {
	int32_t  handle = -1;
	uint16_t netId  = INVALID_NETID;
};
struct CarHold {
	const void *script  = nullptr;
	uint32_t    ip      = 0;
	uint32_t    sinceMs = 0;
};
CarHold g_carHold;
// The cars a hold gave up on, a player still in them: this mission does not
// wait for them again, so a clear it runs every frame does not stall it.
uint16_t g_sparedCars[MAX_TAKEN_CARS];
size_t   g_sparedCarCount = 0;

bool Spared(uint16_t netId) {
	for (size_t i = 0; i < g_sparedCarCount; ++i)
		if (g_sparedCars[i] == netId)
			return true;
	return false;
}

// Whether the owner's mission waits at this instruction another frame for the
// players in `cars`. The first time, everybody is told to get out of them and
// off them, and so is our own player.
bool HoldForPlayersIn(void *script, const TakenCar *cars, size_t count, bool mayHold) {
	const uint32_t ip  = At<uint32_t>(script, layout::SCRIPT_IP);
	const uint32_t now = WallClock::NowMs();
	if (g_carHold.script != script || g_carHold.ip != ip) {
		g_carHold = CarHold{script, ip, now};
		for (size_t i = 0; i < count; ++i) {
			g_client->Missions().SendEffect(LeaveCarEffect(g_own.number, cars[i].netId), now);
			LeaveCarHere(VehicleAt(cars[i].handle));
		}
	}
	bool seated = false;
	for (size_t i = 0; i < count; ++i)
		if (void *const car = VehicleAt(cars[i].handle))
			seated = seated || OtherPlayerIn(car, cars[i].netId);
	if (mayHold && KeepHoldingForSeats(seated, g_carHold.sinceMs, now)) {
		At<uint32_t>(script, layout::SCRIPT_IP) = ip - 2;   // this instruction again next frame
		return true;
	}
	g_carHold = CarHold{};
	return false;
}

// Each of `cars` nobody else sits in, taken away; the others are left, as they
// always were.
void TakeFreeCars(const TakenCar *cars, size_t count) {
	for (size_t i = 0; i < count; ++i) {
		void *const car = VehicleAt(cars[i].handle);
		if (!car)
			continue;
		if (OtherPlayerIn(car, cars[i].netId)) {
			if (!Spared(cars[i].netId) && g_sparedCarCount < MAX_TAKEN_CARS)
				g_sparedCars[g_sparedCarCount++] = cars[i].netId;
			Log("missions: %s would take session car %u away, but a player is still in it; "
			    "it stays",
			    MissionName(g_own.number), static_cast<unsigned>(cars[i].netId));
			continue;
		}
		TakeSessionCar(cars[i].handle, cars[i].netId);
	}
}

// Everybody an enemy could go for, as it stands now.
struct Candidates {
	void *ped[MAX_PLAYERS] = {};
	void *car[MAX_PLAYERS] = {};
	bool  others           = false;   // a participant standing besides our player

	void Gather() {
		for (uint8_t id = 0; id < MAX_PLAYERS; ++id) {
			ped[id] = PlayerPedHere(id);
			car[id] = SeatedCar(ped[id]);
			if (ped[id] && id != LocalId())
				others = true;
		}
	}
	int8_t OfPed(const void *p) const {
		for (uint8_t id = 0; p && id < MAX_PLAYERS; ++id)
			if (ped[id] == p)
				return static_cast<int8_t>(id);
		return -1;
	}
	int8_t OfCar(const void *c) const {
		for (uint8_t id = 0; c && id < MAX_PLAYERS; ++id)
			if (car[id] == c)
				return static_cast<int8_t>(id);
		return -1;
	}
};

// MISSION_RAMPLAYER goes for our player wherever he is; anybody else only in
// a car, since RAMCAR needs one to go for.
bool MayBeRammed(const Candidates &c, uint8_t id) {
	return c.ped[id] && (id == LocalId() || c.car[id]);
}

// `enemy`'s choice among `c`: ChooseEnemyTarget over the players it may go for.
int8_t Choose(const Candidates &c, void *enemy, int8_t current, bool ram, uint32_t sinceMs) {
	float distSq[MAX_PLAYERS];
	bool  valid[MAX_PLAYERS];
	for (uint8_t id = 0; id < MAX_PLAYERS; ++id) {
		valid[id]  = ram ? MayBeRammed(c, id) : c.ped[id] != nullptr;
		distSq[id] = valid[id] ? DistanceSq(enemy, ram && c.car[id] ? c.car[id] : c.ped[id]) : 0.0f;
	}
	return static_cast<int8_t>(ChooseEnemyTarget(current, distSq, valid, MAX_PLAYERS, sinceMs));
}

// ---- the enemies keep going for somebody -----------------------------------------
//
// An order is given once, and the engine forgets it the moment its target is
// gone: a participant's replica rebuilt (a death with the death rule off, a
// change of clothes) or dead, and the objective completes and the enemy stands
// about (CPed::ProcessObjective's tail restores the previous objective, none).
// And the car the mission told to ram the player, or the man told to shoot up
// the player's car, only ever went for the owner. So every enemy the mission
// sends at a player is watched, twice a second, and pointed again at whoever
// is nearest when its target is gone, or when somebody else is a good deal
// nearer (missioncombat.h, ChooseEnemyTarget). Nothing is changed while this
// machine's player is the only one standing, and an order the script replaces
// takes the enemy off the watch.

enum class EnemyOrder : uint8_t { KillOnFoot, KillAnyMeans, DestroyPlayersCar, Ram };

struct WatchedEnemy {
	int32_t    handle     = -1;
	EnemyOrder order      = EnemyOrder::KillOnFoot;
	int8_t     target     = -1;   // the player it was last sent at
	uint32_t   switchedMs = 0;
};
constexpr size_t MAX_WATCHED_ENEMIES = 96;
WatchedEnemy     g_watched[MAX_WATCHED_ENEMIES];
size_t           g_watchedCount   = 0;
uint16_t         g_watchedMission = MISSION_NONE;
uint32_t         g_sweptMs        = 0;
bool             g_saidRetarget   = false;

bool IsCarOrder(EnemyOrder o) { return o == EnemyOrder::Ram; }

WatchedEnemy *FindWatched(int32_t handle, bool car) {
	for (size_t i = 0; i < g_watchedCount; ++i)
		if (g_watched[i].handle == handle && IsCarOrder(g_watched[i].order) == car)
			return &g_watched[i];
	return nullptr;
}

void Unwatch(int32_t handle, bool car) {
	for (size_t i = 0; i < g_watchedCount; ++i)
		if (g_watched[i].handle == handle && IsCarOrder(g_watched[i].order) == car) {
			g_watched[i] = g_watched[--g_watchedCount];
			return;
		}
}

WatchedEnemy *Watch(int32_t handle, EnemyOrder order, int8_t target, uint32_t nowMs) {
	if (g_watchedMission != g_own.number) {
		g_watchedCount   = 0;
		g_watchedMission = g_own.number;
	}
	WatchedEnemy *w = FindWatched(handle, IsCarOrder(order));
	if (!w) {
		if (g_watchedCount == MAX_WATCHED_ENEMIES)
			return nullptr;
		w = &g_watched[g_watchedCount++];
	}
	*w = WatchedEnemy{handle, order, target, nowMs};
	return w;
}

// CPed::SetObjective refuses an objective equal to the stored one
// (missioncombat.h), which a DESTROY_CAR turned KILL and back leaves behind.
void UnstoreObjective(void *npc, uint32_t objective) {
	uint32_t &prev = Field<uint32_t>(npc, offs::PED_PREV_OBJECTIVE);
	if (prev == objective)
		prev = OBJECTIVE_NONE;
}

int32_t PedRef(void *ped) { return Func<int32_t(__cdecl *)(void *)>(CPools__GetPedRef)(ped); }
int32_t CarRef(void *car) { return Func<int32_t(__cdecl *)(void *)>(CPools__GetVehicleRef)(car); }

// `w`'s order, at player `id`, through the engine's own handlers.
void SendEnemy(WatchedEnemy &w, void *enemy, const Candidates &c, uint8_t id, uint32_t nowMs) {
	switch (w.order) {
	case EnemyOrder::KillOnFoot:
	case EnemyOrder::KillAnyMeans: {
		const bool foot = w.order == EnemyOrder::KillOnFoot;
		UnstoreObjective(enemy, foot ? mcombat::OBJECTIVE_KILL_CHAR_ON_FOOT
		                             : mcombat::OBJECTIVE_KILL_CHAR_ANY_MEANS);
		RunOurs(static_cast<uint16_t>(foot ? op::SET_CHAR_OBJ_KILL_CHAR_ON_FOOT
		                                   : op::SET_CHAR_OBJ_KILL_CHAR_ANY_MEANS),
		        {w.handle, PedRef(c.ped[id])});
		break;
	}
	case EnemyOrder::DestroyPlayersCar:
		// What the missions write for it: the car when the player is in one,
		// him on foot when he is not (Grand Theft Aero's goons, 01D9 or 01CA).
		if (c.car[id]) {
			UnstoreObjective(enemy, mcombat::OBJECTIVE_DESTROY_CAR);
			RunOurs(mcombat::OP_SET_CHAR_OBJ_DESTROY_CAR, {w.handle, CarRef(c.car[id])});
		} else {
			UnstoreObjective(enemy, mcombat::OBJECTIVE_KILL_CHAR_ON_FOOT);
			RunOurs(static_cast<uint16_t>(op::SET_CHAR_OBJ_KILL_CHAR_ON_FOOT),
			        {w.handle, PedRef(c.ped[id])});
		}
		break;
	case EnemyOrder::Ram:
		if (id == LocalId())
			RunOurs(mcombat::OP_SET_CAR_MISSION, {w.handle, mcombat::CARMISSION_RAMPLAYER_FARAWAY});
		else
			RunOurs(mcombat::OP_SET_CAR_RAM_CAR, {w.handle, CarRef(c.car[id])});
		break;
	}
	if (id != LocalId() && !g_saidRetarget) {
		g_saidRetarget = true;
		Log("missions: %s's enemies are sent at whoever is nearest, and again when they lose "
		    "their man; %s is the first they went for",
		    MissionName(g_own.number), g_client->PlayerSlot(id).nick.c_str());
	}
	w.target     = static_cast<int8_t>(id);
	w.switchedMs = nowMs;
}

// Where a watched pedestrian stands with its order, and at whom.
EnemyHold HoldOfPed(const WatchedEnemy &w, void *npc, const Candidates &c, int8_t *at) {
	const uint32_t objective = Field<uint32_t>(npc, offs::PED_OBJECTIVE);
	const uint32_t prev      = Field<uint32_t>(npc, offs::PED_PREV_OBJECTIVE);
	*at                      = -1;
	if (w.order == EnemyOrder::DestroyPlayersCar) {
		if (objective == mcombat::OBJECTIVE_DESTROY_CAR) {
			// A car no player sits in any more is engaged at nobody, and he
			// goes for somebody again: the car or the man, as the mission
			// would have sent him.
			void *const car = Field<void *>(npc, offs::PED_CAR_IN_OBJECTIVE);
			*at             = c.OfCar(car);
			return ClassifyEnemyPed(true, objective, false, !car || Wrecked(car), true);
		}
		if (objective == mcombat::OBJECTIVE_KILL_CHAR_ON_FOOT) {
			void *const ped  = Field<void *>(npc, mcombat::PED_IN_OBJECTIVE);
			const bool  gone = !ped || PedDown(ped);
			*at              = c.OfPed(ped);
			return ClassifyEnemyPed(true, objective, false, gone, *at >= 0);
		}
		const bool prevOurs = prev == mcombat::OBJECTIVE_DESTROY_CAR ||
		                      prev == mcombat::OBJECTIVE_KILL_CHAR_ON_FOOT;
		return ClassifyEnemyPed(false, objective, prevOurs, false, false);
	}
	const uint32_t want = w.order == EnemyOrder::KillOnFoot ? mcombat::OBJECTIVE_KILL_CHAR_ON_FOOT
	                                                        : mcombat::OBJECTIVE_KILL_CHAR_ANY_MEANS;
	if (objective == want) {
		void *const ped  = Field<void *>(npc, mcombat::PED_IN_OBJECTIVE);
		const bool  gone = !ped || PedDown(ped);
		*at              = c.OfPed(ped);
		return ClassifyEnemyPed(true, objective, false, gone, *at >= 0);
	}
	return ClassifyEnemyPed(false, objective, prev == want, false, false);
}

EnemyHold HoldOfRammer(void *car, const Candidates &c, int8_t *at) {
	const uint8_t mission = Field<uint8_t>(car, offs::AUTOPILOT_CAR_MISSION);
	*at                   = -1;
	if (mcombat::IsRamPlayer(mission))
		*at = c.ped[LocalId()] ? static_cast<int8_t>(LocalId()) : -1;
	void *const target =
	    mcombat::IsRamCar(mission) ? Field<void *>(car, mcombat::AUTOPILOT_TARGET_CAR) : nullptr;
	if (target)
		*at = c.OfCar(target);
	return ClassifyRammer(mission, mcombat::IsRamCar(mission) && (!target || Wrecked(target)));
}

void SweepMissionEnemies(uint32_t nowMs) {
	if (!g_own.running || g_watchedMission != g_own.number) {
		g_watchedCount = 0;
		return;
	}
	if (g_watchedCount == 0 || nowMs - g_sweptMs < ENEMY_SWEEP_MS || LocalId() >= MAX_PLAYERS)
		return;
	g_sweptMs = nowMs;
	Candidates c;
	c.Gather();
	// Alone, the mission's own orders stand as it wrote them.
	if (!c.others)
		return;
	for (size_t i = 0; i < g_watchedCount;) {
		WatchedEnemy &w   = g_watched[i];
		const bool    car = IsCarOrder(w.order);
		void *const   e   = car ? Func<GetPedFn>(CPools__GetVehicle)(w.handle)
		                        : Func<GetPedFn>(CPools__GetPed)(w.handle);
		if (!e || (car ? Wrecked(e) : PedDown(e))) {
			g_watched[i] = g_watched[--g_watchedCount];
			continue;
		}
		int8_t          at   = -1;
		const EnemyHold hold = car ? HoldOfRammer(e, c, &at) : HoldOfPed(w, e, c, &at);
		if (hold == EnemyHold::Foreign) {
			g_watched[i] = g_watched[--g_watchedCount];
			continue;
		}
		// A car with nobody at its wheel rams nobody: left be until it has one.
		const bool driven = !car || Field<void *>(e, offs::VEH_DRIVER) != nullptr;
		if (hold != EnemyHold::Busy && driven) {
			const int8_t current = hold == EnemyHold::Engaged ? at : -1;
			const int8_t pick    = Choose(c, e, current, car, nowMs - w.switchedMs);
			if (pick >= 0 && pick != current)
				SendEnemy(w, e, c, static_cast<uint8_t>(pick), nowMs);
		}
		++i;
	}
}

void CopyEnemy(int32_t charHandle, void *npc, int32_t command, void *owner);

// SET_CHAR_OBJ_KILL_PLAYER_ON_FOOT or _ANY_MEANS, from the session's mission:
// the enemy goes for whichever player is nearest it, a participant's replica
// on this machine included, as the CHAR form of the same objective. The hits
// it lands on a replica go to that player, the way every hosted pedestrian's
// do (population.md 6). With the owner nearest, it is the instruction as the
// mission wrote it. Either way it is watched from then on.
int8_t RetargetKill(void *script, int32_t command, RangeFn original) {
	if (!g_client || !g_own.running || !IsMissionScript(script))
		return original(script, nullptr, command);
	PeekParams(script, 2);
	const int32_t charHandle = reinterpret_cast<const int32_t *>(Params())[0];
	void *const   npc        = Func<GetPedFn>(CPools__GetPed)(charHandle);
	void *const   owner      = Func<PlayerFn>(FindPlayerPed)();
	if (!npc || !owner)
		return original(script, nullptr, command);
	Toughen(charHandle, npc);
	CopyEnemy(charHandle, npc, command, owner);
	const uint32_t   now   = WallClock::NowMs();
	const EnemyOrder order = command == op::SET_CHAR_OBJ_KILL_PLAYER_ON_FOOT
	                             ? EnemyOrder::KillOnFoot
	                             : EnemyOrder::KillAnyMeans;
	Candidates c;
	c.Gather();
	const int8_t id = Choose(c, npc, -1, false, 0);
	WatchedEnemy *const w = Watch(charHandle, order, static_cast<int8_t>(LocalId()), now);
	if (id < 0 || id == LocalId() || !w)
		return original(script, nullptr, command);
	// Past the instruction, as its handler would have gone, and the CHAR form
	// in its place.
	uint32_t ip = At<uint32_t>(script, layout::SCRIPT_IP);
	Func<CollectFn>(CTheScripts__CollectParameters)(script, &ip, 2);
	At<uint32_t>(script, layout::SCRIPT_IP) = ip;
	SendEnemy(*w, npc, c, static_cast<uint8_t>(id), now);
	return 0;
}

// SET_CHAR_OBJ_DESTROY_CAR at the car the owner's player sits in: the goon
// goes for the nearest player's car, or for him when he is on foot, and is
// watched. At any other car it is the mission's business, as written.
int8_t RetargetDestroy(void *script, int32_t command, RangeFn original) {
	if (!g_client || !g_own.running || !IsMissionScript(script))
		return original(script, nullptr, command);
	PeekParams(script, 2);
	const int32_t charHandle = reinterpret_cast<const int32_t *>(Params())[0];
	const int32_t carHandle  = reinterpret_cast<const int32_t *>(Params())[1];
	void *const   npc        = Func<GetPedFn>(CPools__GetPed)(charHandle);
	void *const   car        = Func<GetPedFn>(CPools__GetVehicle)(carHandle);
	void *const   owner      = Func<PlayerFn>(FindPlayerPed)();
	if (!npc || !car || !owner || SeatedCar(owner) != car) {
		Unwatch(charHandle, false);
		return original(script, nullptr, command);
	}
	const int8_t   r   = original(script, nullptr, command);
	const uint32_t now = WallClock::NowMs();
	WatchedEnemy *const w =
	    Watch(charHandle, EnemyOrder::DestroyPlayersCar, static_cast<int8_t>(LocalId()), now);
	Candidates c;
	c.Gather();
	const int8_t id = Choose(c, npc, -1, false, 0);
	if (w && id >= 0 && id != LocalId())
		SendEnemy(*w, npc, c, static_cast<uint8_t>(id), now);
	return r;
}

// SET_CAR_MISSION to MISSION_RAMPLAYER_FARAWAY: the car goes for the nearest
// player in a car, or ours, and is watched. Any other car mission, and
// CAR_SET_IDLE, take the car off the watch: the script has other plans.
int8_t RetargetRam(void *script, int32_t command, RangeFn original) {
	if (!g_client || !g_own.running || !IsMissionScript(script))
		return original(script, nullptr, command);
	const bool setMission = command == mcombat::OP_SET_CAR_MISSION;
	PeekParams(script, setMission ? 2 : 1);
	const int32_t carHandle = reinterpret_cast<const int32_t *>(Params())[0];
	const int32_t mission   = setMission ? reinterpret_cast<const int32_t *>(Params())[1] : 0;
	const int8_t  r         = original(script, nullptr, command);
	if (mission < 0 || mission > 0xFF || !mcombat::IsRamPlayer(static_cast<uint8_t>(mission))) {
		Unwatch(carHandle, true);
		return r;
	}
	void *const car = Func<GetPedFn>(CPools__GetVehicle)(carHandle);
	if (!car)
		return r;
	const uint32_t      now = WallClock::NowMs();
	WatchedEnemy *const w   = Watch(carHandle, EnemyOrder::Ram, static_cast<int8_t>(LocalId()), now);
	Candidates c;
	c.Gather();
	const int8_t id = Choose(c, car, -1, true, 0);
	if (w && id >= 0 && id != LocalId())
		SendEnemy(*w, car, c, static_cast<uint8_t>(id), now);
	return r;
}

// SET_CHAR_OBJ_NO_OBJ, MARK_CHAR_AS_NO_LONGER_NEEDED, DELETE_CHAR and
// MARK_CAR_AS_NO_LONGER_NEEDED from the mission: whatever it meant by it, the
// sweep leaves that one be.
void MissionLetsGoOfEnemy(void *script, bool car) {
	if (!g_own.running || g_watchedCount == 0 || !IsMissionScript(script))
		return;
	PeekParams(script, 1);
	Unwatch(reinterpret_cast<const int32_t *>(Params())[0], car);
}

// ---- the wheel another player holds (seatplan.h, PlanScriptedWheel) -------------
//
// Give Me Liberty after the change of clothes: `go_to_and_drive_car` on the
// owner's player, a participant already at the Kuruma's wheel, and the
// owner's engine walked up and pulled him out. Any script of this machine's,
// the session's mission or not, telling our own player to drive a car another
// player drives, gets him put straight into a passenger seat of it instead.

// The car a script told our player to walk to the wheel of, while he walks:
// watched every frame, since the wheel can be taken while he is on his way
// and the jack is decided at the door. -1 for none.
int32_t g_wheelWalk = -1;
// The car a drive was turned into a ride in, while he is on his way into it,
// for Client::NoteScriptedRide. -1 for none.
int32_t g_ride = -1;
// A turned warp that found no seat after all runs the script's own warp,
// which comes back through here.
bool g_wheelAsWritten = false;
bool g_saidRide       = false;

// Another player at the wheel of `car`: his ped in the driver's seat here, or
// the session's word that he drives it, which holds while his ped is between
// two builds of itself (a change of clothes takes it down and puts it back).
bool AnotherPlayerDrives(void *car, int32_t handle) {
	void *const driver = Field<void *>(car, offs::VEH_DRIVER);
	if (driver && driver == Func<PlayerFn>(FindPlayerPed)())
		return false;
	if (driver)
		for (uint8_t id = 0; id < MAX_PLAYERS; ++id)
			if (ReplicaOf(id) == driver)
				return true;
	const uint16_t       netId = g_client->SessionCarNetIdOf(handle);
	const RemoteVehicle *v     = netId != INVALID_NETID ? g_client->VehicleByNetId(netId) : nullptr;
	return v && v->driverPlayerId != INVALID_PLAYER && v->driverPlayerId != LocalId();
}

// Opening a door or pulling somebody out: past the point of choosing a seat.
bool EnteringACar(void *ped) {
	const uint32_t state = Field<uint32_t>(ped, offs::PED_STATE);
	return state == PEDSTATE_ENTER_CAR || state == PEDSTATE_CARJACK;
}

// SET_CHAR_OBJ_ENTER_CAR_AS_PASSENGER on our player for `handle`, what the
// script would have written for a seat beside the driver. The car objectives
// go first: SetObjective returns at once when the stored objective is the one
// it is given (0x004D8403..0x004D8414, `cmp [ebx+168h],esi` and nonzero).
void HeadForPassengerSeat(void *me, int32_t handle) {
	for (size_t at : {offs::PED_OBJECTIVE, offs::PED_PREV_OBJECTIVE}) {
		uint32_t &objective = Field<uint32_t>(me, at);
		if (objective == OBJECTIVE_ENTER_CAR_AS_DRIVER || objective == OBJECTIVE_ENTER_CAR_AS_PASSENGER)
			objective = OBJECTIVE_NONE;
	}
	RunOurs(static_cast<uint16_t>(op::SET_CHAR_OBJ_ENTER_CAR_AS_PASSENGER),
	        {Func<int32_t(__cdecl *)(void *)>(CPools__GetPedRef)(me), handle});
}

void RideInstead(int32_t handle, uint16_t netId, bool warp) {
	g_wheelWalk = -1;
	g_ride      = handle;
	g_client->NoteScriptedRide(netId, WallClock::NowMs());
	if (!g_saidRide) {
		g_saidRide = true;
		Log("missions: a script put our player at the wheel of vehicle %u, which another "
		    "player is driving; he %s a passenger seat of it instead",
		    netId, warp ? "was put in" : "walks to");
	}
}

// A warp of the session's mission that put our player, its owner, in a car:
// the participants on foot get its free passenger seats (BoardParticipants,
// protocol.h C_MissionBoard) once the session has a name for it. -1 for
// none.
int32_t  g_boardCar     = -1;
uint32_t g_boardSinceMs = 0;

void NoteOwnerWarp(void *script, int32_t command, int32_t handle) {
	if (command == op::SET_CHAR_OBJ_ENTER_CAR_AS_DRIVER || !g_own.running || !IsMissionScript(script))
		return;
	g_boardCar     = handle;
	g_boardSinceMs = WallClock::NowMs();
}

// SET_CHAR_OBJ_ENTER_CAR_AS_DRIVER, WARP_PLAYER_INTO_CAR or WARP_CHAR_INTO_CAR,
// char or player then car, on our own player.
int8_t ScriptedWheel(void *script, int32_t command, RangeFn original) {
	if (!g_client || g_wheelAsWritten)
		return original(script, nullptr, command);
	PeekParams(script, 2);
	const int32_t who    = reinterpret_cast<const int32_t *>(Params())[0];
	const int32_t handle = reinterpret_cast<const int32_t *>(Params())[1];
	void *const   me     = Func<PlayerFn>(FindPlayerPed)();
	void *const   car    = Func<GetPedFn>(CPools__GetVehicle)(handle);
	// WARP_PLAYER_INTO_CAR's ped is CWorld::Players[n].m_pPed, the first
	// field; this game only ever has the one.
	void *const ped = command != op::WARP_PLAYER_INTO_CAR ? Func<GetPedFn>(CPools__GetPed)(who)
	                  : who == 0 ? *Ptr<void *>(CWorld__Players)
	                             : nullptr;
	if (!me || ped != me || !car)
		return original(script, nullptr, command);

	const bool      warp  = command != op::SET_CHAR_OBJ_ENTER_CAR_AS_DRIVER;
	const uint16_t  netId = g_client->SessionCarNetIdOf(handle);
	const WheelMove move =
	    netId == INVALID_NETID
	        ? WheelMove::Drive
	        : PlanScriptedWheel(AnotherPlayerDrives(car, handle), PassengerSeatFree(car),
	                            Field<bool>(me, offs::PED_IN_VEHICLE), EnteringACar(me), warp);
	if (move == WheelMove::Drive) {
		const int8_t r = original(script, nullptr, command);
		g_wheelWalk    = warp ? -1 : handle;
		NoteOwnerWarp(script, command, handle);
		return r;
	}

	// Past the instruction, as its handler would have gone, and the ride in
	// its place: straight into the seat, the walk included. Walked to a
	// passenger door he stood beside the car until the engine's own timer put
	// him in (seatplan.h); a player already in a car still gets the walk,
	// which the engine counts as done at once.
	uint32_t ip = At<uint32_t>(script, layout::SCRIPT_IP);
	Func<CollectFn>(CTheScripts__CollectParameters)(script, &ip, 2);
	At<uint32_t>(script, layout::SCRIPT_IP) = ip;
	const bool onFoot = !Field<bool>(me, offs::PED_IN_VEHICLE);
	bool       warped = false;
	if (warp || onFoot) {
		warped = WarpLocalPlayerIntoPassengerSeat(handle) >= 0;
		if (!warped && warp) {
			g_wheelAsWritten = true;
			RunOurs(static_cast<uint16_t>(command), {who, handle});
			g_wheelAsWritten = false;
			Log("missions: a script's warp to the wheel of vehicle %u found no passenger seat "
			    "to put our player in after all, so it went as written",
			    netId);
			NoteOwnerWarp(script, command, handle);
			return 0;
		}
	}
	if (!warped)
		HeadForPassengerSeat(me, handle);
	else
		NoteOwnerWarp(script, command, handle);
	RideInstead(handle, netId, warped);
	return 0;
}

// Every frame: the walk to the wheel, turned into a passenger seat of that car
// if another player takes that wheel before our player reaches it; and the
// ride, said to the client until he is in it.
void WatchScriptedWheel(uint32_t nowMs) {
	void *const me = Func<PlayerFn>(FindPlayerPed)();
	if (g_ride >= 0) {
		void *const car  = Func<GetPedFn>(CPools__GetVehicle)(g_ride);
		const bool  inIt = me && car && Field<bool>(me, offs::PED_IN_VEHICLE) &&
		                  Field<void *>(me, offs::PED_MY_VEHICLE) == car;
		// The objective can outlive the entry it was for, so on foot only.
		const bool heading = me && car && !Field<bool>(me, offs::PED_IN_VEHICLE) &&
		                     Field<uint32_t>(me, offs::PED_OBJECTIVE) == OBJECTIVE_ENTER_CAR_AS_PASSENGER &&
		                     Field<void *>(me, offs::PED_CAR_IN_OBJECTIVE) == car;
		const uint16_t netId = g_client->SessionCarNetIdOf(g_ride);
		if ((heading || inIt) && netId != INVALID_NETID)
			g_client->NoteScriptedRide(netId, nowMs);
		if (!heading)
			g_ride = -1;
	}
	if (g_wheelWalk < 0)
		return;
	void *const car = Func<GetPedFn>(CPools__GetVehicle)(g_wheelWalk);
	if (!me || !car || Field<bool>(me, offs::PED_IN_VEHICLE) ||
	    Field<uint32_t>(me, offs::PED_OBJECTIVE) != OBJECTIVE_ENTER_CAR_AS_DRIVER ||
	    Field<void *>(me, offs::PED_CAR_IN_OBJECTIVE) != car) {
		g_wheelWalk = -1;
		return;
	}
	const uint16_t netId = g_client->SessionCarNetIdOf(g_wheelWalk);
	if (netId == INVALID_NETID ||
	    PlanScriptedWheel(AnotherPlayerDrives(car, g_wheelWalk), PassengerSeatFree(car), false,
	                      EnteringACar(me), false) != WheelMove::Ride)
		return;
	const int32_t handle = g_wheelWalk;
	const bool    warped = WarpLocalPlayerIntoPassengerSeat(handle) >= 0;
	if (!warped)
		HeadForPassengerSeat(me, handle);
	RideInstead(handle, netId, warped);
}

// ---- how many ride in the car (missionaddr.h, GET_NUMBER_OF_PASSENGERS) -----------
//
// The Paramedic calls its ambulance full when GET_NUMBER_OF_PASSENGERS reaches
// GET_MAXIMUM_NUMBER_OF_PASSENGERS, and then never tells the patient to get
// in. With participants riding in the back, their copies here counted, so
// two of them in a three-seat ambulance left room for one patient and a
// third put up "Ambulance full!!" with nobody on board. Our own mission is
// told the count without the other players' copies; the maximum stays what
// the car has. When a patient then heads for a seat a participant is in,
// KeepMissionSeats has as many of them get out as it takes.
bool g_saidPassengers = false;

int8_t PassengersBesidePlayers(void *script, int32_t command, RangeFn original) {
	if (!g_client || !g_own.running || !IsMissionScript(script))
		return original(script, nullptr, command);
	PeekParams(script, 1);
	const int32_t handle = reinterpret_cast<const int32_t *>(Params())[0];
	void *const   car    = Func<GetPedFn>(CPools__GetVehicle)(handle);
	if (!car)
		return original(script, nullptr, command);
	uint8_t players = 0;
	for (size_t slot = 0; slot < offs::VEH_MAX_PASSENGERS; ++slot) {
		void *const in = Field<void *>(car, offs::VEH_PASSENGERS + slot * 4u);
		if (in && PlayerOfReplica(in) != INVALID_PLAYER)
			++players;
	}
	if (players == 0)
		return original(script, nullptr, command);
	// The handler reads the byte and nothing else between here and its
	// return, so the count goes back as it was straight after.
	uint8_t      &count = Field<uint8_t>(car, offs::VEH_NUM_PASSENGERS);
	const uint8_t was   = count;
	count               = static_cast<uint8_t>(was > players ? was - players : 0);
	const int8_t r      = original(script, nullptr, command);
	count               = was;
	if (!g_saidPassengers) {
		g_saidPassengers = true;
		Log("missions: %s counts the passengers in a car without the %u player%s riding in it",
		    MissionName(g_own.number), players, players == 1 ? "" : "s");
	}
	return r;
}

// ---- more enemies, that count like the originals (missions.md 10.5) ------------
//
// Under `missionEnemies = more`, an enemy the mission tells to kill the player
// gets a copy for each player past the first: the same model, type and weapon,
// beside it, as tough, going for whichever player is nearest it. The script
// never learns the copies exist. The original's handle becomes a group
// (mission.h, EnemyGroups), and while the owner's mission runs an
// instruction the ped pool answers that handle with the group's
// representative: IS_CHAR_DEAD is true only once every member is dead, and
// where the enemy is, is where somebody still standing is.

Detour      g_pedGetAt;
bool        g_pedGetAtTried = false;

using PedGetAtFn = void *(__fastcall *)(void *pool, void *edx, int32_t handle);

bool DyingOrDead(void *ped) {
	const uint32_t state = Field<uint32_t>(ped, offs::PED_STATE);
	return state == PEDSTATE_DIE || state == PEDSTATE_DEAD;
}

void *__fastcall HookedPedGetAt(void *pool, void *edx, int32_t handle) {
	const PedGetAtFn original = g_pedGetAt.Original<PedGetAtFn>();
	void *const      ped      = original(pool, edx, handle);
	if (!g_missionInstruction || g_groups.Count() == 0 || !g_groups.Has(handle))
		return ped;
	const int32_t stands = g_groups.Representative(handle, [&](int32_t h) {
		void *const member = h == handle ? ped : original(pool, nullptr, h);
		return member != nullptr && !DyingOrDead(member);
	});
	return stands == handle ? ped : original(pool, nullptr, stands);
}

// ADD_BLIP_FOR_CHAR's handler loads the ped pool and calls CPool<CPed>::GetAt,
// as addresses.h found it: `8B 0D <ms_pPedPool>` and an E8 that lands on it.
bool PedGetAtLooksRight() {
	const uint8_t *code = Ptr<uint8_t>(ADD_BLIP_FOR_CHAR_HANDLER);
	for (size_t i = 0; i + 11 <= 48; ++i) {
		uint32_t pool = 0;
		std::memcpy(&pool, code + i + 2, 4);
		if (code[i] != 0x8B || code[i + 1] != 0x0D || pool != CPools__ms_pPedPool ||
		    code[i + 6] != 0xE8)
			continue;
		int32_t rel = 0;
		std::memcpy(&rel, code + i + 7, 4);
		return ADD_BLIP_FOR_CHAR_HANDLER + i + 11 + static_cast<uint32_t>(rel) == CPool_CPed__GetAt;
	}
	return false;
}

// The ped pool's own seam, hooked the first time a mission gets copies.
bool GroupsUsable() {
	if (g_pedGetAt.IsInstalled())
		return true;
	if (g_pedGetAtTried)
		return false;
	g_pedGetAtTried = true;
	if (!PedGetAtLooksRight()) {
		Log("missions: ADD_BLIP_FOR_CHAR does not call CPool<CPed>::GetAt at 0x%08X in this "
		    "image, so the enemies get no copies: `more` plays as `tougher`",
		    static_cast<unsigned>(CPool_CPed__GetAt));
		return false;
	}
	if (!g_pedGetAt.Install("CPool<CPed>::GetAt", reinterpret_cast<void *>(CPool_CPed__GetAt),
	                        reinterpret_cast<void *>(&HookedPedGetAt))) {
		Log("missions: CPool<CPed>::GetAt would not hook, so the enemies get no copies: "
		    "`more` plays as `tougher`");
		return false;
	}
	Log("missions: CPool<CPed>::GetAt hooked; an enemy's copies count like the original");
	return true;
}

// How many of the ped pool's slots are free: copies leave the city its
// pedestrians.
constexpr int32_t PED_SLOTS_LEFT_FREE = 24;

int32_t FreePedSlots() {
	const uint8_t *pool = *reinterpret_cast<uint8_t *const *>(CPools__ms_pPedPool);
	if (!pool)
		return 0;
	const uint8_t *flags = *reinterpret_cast<uint8_t *const *>(pool + object::POOL_FLAGS);
	const int32_t  size  = *reinterpret_cast<const int32_t *>(pool + object::POOL_SIZE);
	int32_t        free  = 0;
	for (int32_t i = 0; flags && i < size && i < 1024; ++i)
		if ((flags[i] & object::POOLFLAG_ISFREE) != 0)
			++free;
	return free;
}

// CREATE_CHAR through the engine's own handler, with the handle it made.
int32_t CreateCharHere(int32_t pedType, int32_t model, float x, float y, float z) {
	uint8_t code[2 + 5 * 5 + 3];
	size_t  n  = 0;
	code[n++]  = static_cast<uint8_t>(op::CREATE_CHAR & 0xFF);
	code[n++]  = static_cast<uint8_t>(op::CREATE_CHAR >> 8);
	auto value = [&](const void *bits) {
		code[n++] = PARAM_INT32;
		std::memcpy(code + n, bits, 4);
		n += 4;
	};
	value(&pedType);
	value(&model);
	value(&x);
	value(&y);
	value(&z);
	code[n++] = PARAM_LOCAL;
	code[n++] = 0;
	code[n++] = 0;
	int32_t handle = -1;
	return RunHere(code, n, &handle) ? handle : -1;
}

// `charHandle` goes for the nearest player, the way RetargetKill sends the
// original: the objective as the mission wrote it at this machine's player,
// or its CHAR form at a participant's replica. Watched like the original.
void GoForNearest(int32_t charHandle, void *npc, int32_t command, void *) {
	const uint32_t   now   = WallClock::NowMs();
	const EnemyOrder order = command == op::SET_CHAR_OBJ_KILL_PLAYER_ON_FOOT
	                             ? EnemyOrder::KillOnFoot
	                             : EnemyOrder::KillAnyMeans;
	Candidates c;
	c.Gather();
	const int8_t        id = Choose(c, npc, -1, false, 0);
	WatchedEnemy *const w  = Watch(charHandle, order, static_cast<int8_t>(LocalId()), now);
	if (id < 0 || id == LocalId() || !w) {
		RunOurs(static_cast<uint16_t>(command), {charHandle, 0});
		return;
	}
	SendEnemy(*w, npc, c, static_cast<uint8_t>(id), now);
}

void CopyEnemy(int32_t charHandle, void *npc, int32_t command, void *owner) {
	const uint8_t copies = g_client->Missions().EnemyCopies();
	if (copies == 0 || g_groups.Has(charHandle) || g_groups.IsCopy(charHandle) ||
	    !MayCopyPedType(Field<int32_t>(npc, offs::PED_TYPE)) ||
	    Field<uint8_t>(npc, offs::PED_IN_VEHICLE) != 0 || FreePedSlots() < PED_SLOTS_LEFT_FREE ||
	    !GroupsUsable() || !g_groups.Add(charHandle))
		return;
	const int32_t pedType = Field<int32_t>(npc, offs::PED_TYPE);
	const int32_t model   = Field<int16_t>(npc, offs::MODEL_INDEX);
	const uint8_t slot    = Field<uint8_t>(npc, offs::PED_CURRENT_WEAPON);
	int32_t       weapon = 0, ammo = 0;
	if (slot < offs::NUM_WEAPON_SLOTS) {
		const size_t at = offs::PED_WEAPONS + slot * offs::SIZEOF_WEAPON;
		weapon          = Field<int32_t>(npc, at + offs::WEAPON_TYPE);
		ammo            = Field<int32_t>(npc, at + offs::WEAPON_AMMO_TOTAL);
	}
	const float *pos  = &Field<float>(npc, offs::POSITION);
	uint8_t      made = 0;
	for (uint8_t i = 0; i < copies && FreePedSlots() >= PED_SLOTS_LEFT_FREE; ++i) {
		// Beside it, a step and a half round, where no building is in between.
		float x = pos[0], y = pos[1];
		for (uint8_t attempt = 0; attempt < SPREAD_ATTEMPTS; ++attempt) {
			float cx = 0.0f, cy = 0.0f;
			SpreadSpot(pos[0], pos[1], i, copies, attempt, &cx, &cy);
			const Vec3f from{pos[0], pos[1], pos[2]}, to{cx, cy, pos[2]};
			if (Func<LineOfSightFn>(CWorld__GetIsLineOfSightClear)(&from, &to, 1, 0, 0, 1, 0, 0, 0) !=
			    0) {
				x = cx;
				y = cy;
				break;
			}
		}
		// Its z is where its middle is; CREATE_CHAR puts a ped's feet on the one
		// it is given.
		const int32_t copy = CreateCharHere(pedType, model, x, y, pos[2] - 1.0f);
		void *const   ped  = copy >= 0 ? Func<GetPedFn>(CPools__GetPed)(copy) : nullptr;
		if (!ped)
			continue;
		g_groups.AddCopy(charHandle, copy);
		++made;
		if (weapon != 0)
			RunOurs(op::GIVE_WEAPON_TO_CHAR, {copy, weapon, ammo > 0 ? ammo : 1});
		Toughen(copy, ped);
		GoForNearest(copy, ped, command, owner);
	}
	static bool said = false;
	if (made != 0 && !said) {
		said = true;
		Log("missions: %s's enemies come %u at a time for the players in it, and count as one",
		    MissionName(g_own.number), static_cast<unsigned>(made + 1));
	}
}

// DELETE_CHAR or MARK_CHAR_AS_NO_LONGER_NEEDED on an enemy that has copies:
// the group goes first, so the instruction lands on the original itself, and
// then every copy is let go of the same way.
int8_t LetGoOfChar(void *script, int32_t command, RangeFn original) {
	if (!g_own.running || !IsMissionScript(script) || g_groups.Count() == 0)
		return original(script, nullptr, command);
	PeekParams(script, 1);
	int32_t      copies[MISSION_ENEMY_COPIES_MAX];
	const size_t n = g_groups.Dissolve(reinterpret_cast<const int32_t *>(Params())[0], copies,
	                                   MISSION_ENEMY_COPIES_MAX);
	const int8_t r = original(script, nullptr, command);
	for (size_t i = 0; i < n; ++i)
		if (Func<GetPedFn>(CPools__GetPed)(copies[i]) != nullptr)
			RunOurs(static_cast<uint16_t>(command), {copies[i]});
	return r;
}

// The mission is over: every copy is let go of, as the mission's own cleanup
// lets go of the originals.
void ReleaseCopies(const int32_t *copies, size_t n) {
	for (size_t i = 0; i < n; ++i)
		if (Func<GetPedFn>(CPools__GetPed)(copies[i]) != nullptr)
			RunOurs(op::MARK_CHAR_AS_NO_LONGER_NEEDED, {copies[i]});
}

// ---- everybody has to be quiet (mission-audit.md R13) ----------------------------
//
// It was decided that a participant can give the game away as well as the owner
// can. Each of these is a single condition wherever the missions ask it, so
// the owner's answer is only ever widened to true, and only under ANDOR_NONE,
// where the result is this condition's alone.

// The condition the engine just answered, before its NOT, and a way to say
// it was true after all.
bool RawResult(void *script) {
	return RawCondition(At<uint8_t>(script, layout::SCRIPT_COND_RESULT),
	                    At<uint8_t>(script, layout::SCRIPT_NOT) != 0);
}

void ForceTrue(void *script) {
	At<uint8_t>(script, layout::SCRIPT_COND_RESULT) =
	    TrueAfterAll(At<uint8_t>(script, layout::SCRIPT_NOT) != 0);
}

bool MayWiden(void *script) {
	return g_client && g_own.running && IsMissionScript(script) &&
	       At<uint16_t>(script, layout::SCRIPT_AND_OR) == ANDOR_NONE && !RawResult(script);
}

// CPed::OurPedCanSeeThisOne as re3 has it: ahead of the pedestrian, nearer
// than 40 m, and no building between its head and the other.
bool CanSee(void *npc, void *target) {
	const float *n  = &Field<float>(npc, offs::POSITION);
	const float *t  = &Field<float>(target, offs::POSITION);
	const float *f  = &Field<float>(npc, offs::MATRIX_FWD);
	const float  dx = t[0] - n[0], dy = t[1] - n[1];
	if (dx * f[0] + dy * f[1] < 0.0f || dx * dx + dy * dy >= 40.0f * 40.0f)
		return false;
	const Vec3f head{n[0], n[1], n[2] + 1.0f}, at{t[0], t[1], t[2]};
	return Func<LineOfSightFn>(CWorld__GetIsLineOfSightClear)(&head, &at, 1, 0, 0, 0, 0, 0, 0) != 0;
}

// IS_CAR_IN_MISSION_GARAGE and HAS_RESPRAY_HAPPENED (mission-audit.md R5): this
// machine's own garage first, then what the participants' own garages said,
// for a helper who brought the car in or had it resprayed.
bool g_saidGarageElsewhere = false;

// HAS_RESPRAY_HAPPENED inside a block, where MayWiden never reaches: a
// participant's respray answers it, and is held until the block's
// goto_if_false says whether it was spent (standin.h).
struct HeldRespray {
	const void     *script  = nullptr;
	uint8_t         garage  = 0;
	standin::Block  block   = standin::Block::Single;
	bool            notFlag = false;
};
HeldRespray g_heldRespray;
bool        g_saidHeldRespray = false;

bool HoldRespray(void *script, int32_t garage, uint16_t andOrBefore, uint8_t condBefore,
                 bool notFlag) {
	const standin::Block block = standin::BlockOf(andOrBefore);
	if (!g_client || !g_own.running || g_own.finishing || !IsMissionScript(script) ||
	    (block != standin::Block::And && block != standin::Block::Or) || garage < 0 ||
	    garage >= MISSION_GARAGES ||
	    !g_client->Missions().ResprayElsewhere(static_cast<uint8_t>(garage)))
		return false;
	At<uint8_t>(script, layout::SCRIPT_COND_RESULT) = CompareFlagIfTrue(condBefore, andOrBefore, notFlag);
	g_heldRespray = HeldRespray{script, static_cast<uint8_t>(garage), block, notFlag};
	return true;
}

// The block's goto_if_false: the held respray is taken if the block went its way.
void SettleRespray(void *script) {
	const HeldRespray held = g_heldRespray;
	g_heldRespray          = HeldRespray{};
	const bool final       = At<uint8_t>(script, layout::SCRIPT_COND_RESULT) != 0;
	if (!g_client || !standin::HeldAnswerSpent(held.block, held.notFlag, final))
		return;
	g_client->Missions().TakeResprayElsewhere(held.garage);
	if (!g_saidHeldRespray) {
		g_saidHeldRespray = true;
		Log("missions: a participant's respray at garage %u answered %s", static_cast<unsigned>(held.garage),
		    MissionName(g_own.number));
	}
}

int8_t GarageCondition(void *script, int32_t command, RangeFn original) {
	PeekParams(script, 1);
	const int32_t  garage     = *reinterpret_cast<const int32_t *>(Params());
	const uint16_t andOr      = At<uint16_t>(script, layout::SCRIPT_AND_OR);
	const uint8_t  condBefore = At<uint8_t>(script, layout::SCRIPT_COND_RESULT);
	const bool     notFlag    = At<uint8_t>(script, layout::SCRIPT_NOT) != 0;
	const int8_t   r          = original(script, nullptr, command);
	if (command == op::HAS_RESPRAY_HAPPENED && HoldRespray(script, garage, andOr, condBefore, notFlag))
		return r;
	// Grand Theft Auto asks it in an `if and` beside a flag of its own (the
	// car stood in the lock-up undamaged), the only block main.scm puts it in.
	// Whether the car is in the garage is about the car, not about who
	// brought it, so the block's own arithmetic takes the yes: without it a
	// participant's delivery deleted the car here unanswered, and the next
	// IS_CAR_DEAD on it failed the mission with "The vehicle is wrecked!".
	if (command == op::IS_CAR_IN_MISSION_GARAGE && andOr != ANDOR_NONE) {
		if (g_client && g_own.running && IsMissionScript(script) && garage >= 0 &&
		    garage < MISSION_GARAGES &&
		    g_client->Missions().GarageHasCarElsewhere(static_cast<uint8_t>(garage))) {
			At<uint8_t>(script, layout::SCRIPT_COND_RESULT) = CompareFlagIfTrue(condBefore, andOr, notFlag);
			if (!g_saidGarageElsewhere) {
				g_saidGarageElsewhere = true;
				Log("missions: garage %d of %s is answered by somebody else's machine, where the car "
				    "is", static_cast<int>(garage), MissionName(g_own.number));
			}
		}
		return r;
	}
	if (!MayWiden(script) || garage < 0 || garage >= MISSION_GARAGES)
		return r;
	const uint8_t g   = static_cast<uint8_t>(garage);
	const bool    yes = command == op::IS_CAR_IN_MISSION_GARAGE
	                        ? g_client->Missions().GarageHasCarElsewhere(g)
	                        : g_client->Missions().TakeResprayElsewhere(g);
	if (!yes)
		return r;
	ForceTrue(script);
	if (!g_saidGarageElsewhere) {
		g_saidGarageElsewhere = true;
		Log("missions: garage %u of %s is answered by somebody else's machine, where the car is",
		    static_cast<unsigned>(g), MissionName(g_own.number));
	}
	return r;
}

// HAS_DRUG_PLANE_BEEN_SHOT_DOWN, HAS_DROP_OFF_PLANE_BEEN_SHOT_DOWN and
// HAS_CATALINA_HELI_BEEN_SHOT_DOWN (mission-audit.md R7): the owner's own
// first, then a participant's copy that their rocket or rounds brought down.
int8_t PlaneCondition(void *script, int32_t command, RangeFn original) {
	const int8_t r = original(script, nullptr, command);
	if (!MayWiden(script))
		return r;
	const bool     catalina = command == OP_HAS_CATALINA_HELI_BEEN_SHOT_DOWN;
	const uint16_t plane    = catalina ? MISSION_SHOT_DOWN_CATALINA
	                          : command == op::HAS_DRUG_PLANE_BEEN_SHOT_DOWN ? MISSION_SHOT_DOWN_DRUG_PLANE
	                                                                         : MISSION_SHOT_DOWN_DROP_OFF;
	if (g_client->Missions().TakeShotDownElsewhere(plane)) {
		ForceTrue(script);
		Log("missions: %s's %s went down on somebody else's machine", MissionName(g_own.number),
		    catalina ? "Catalina helicopter" : "Cessna");
	}
	return r;
}

// HAS_CHAR_SPOTTED_PLAYER: any participant it can see.
// Sayonara Salvatore's spotting (standin.h, SpottedStandIn): who was seen,
// and whether the two place checks after it are about him rather than the
// owner. Only in a single condition, as every one of its thirteen is.
void SpottedInSayonara(void *script, void *npc, uint32_t ip) {
	g_spotStandIn = SpotStandIn{};
	if (At<uint16_t>(script, layout::SCRIPT_AND_OR) != ANDOR_NONE)
		return;
	const bool  ownerSeen = RawResult(script);
	void *const me        = Func<PlayerFn>(FindPlayerPed)();
	const Vec3  mine      = me ? EntityPos(me) : Vec3{};
	const bool  ownerSafe = me && standin::InSafeBox(mine.x, mine.y);
	standin::Watched w[MAX_PLAYERS];
	Vec3             at[MAX_PLAYERS] = {};
	for (uint8_t id = 0; id < MAX_PLAYERS; ++id) {
		void *const ped = InOwnMission(id) ? ReplicaOf(id) : nullptr;
		if (!ped || PedDown(ped))
			continue;
		at[id]     = EntityPos(ped);
		w[id].valid = true;
		w[id].seen  = CanSee(npc, ped);
		w[id].safe  = standin::InSafeBox(at[id].x, at[id].y);
	}
	const int who = standin::SpottedStandIn(ownerSeen, ownerSafe, w, MAX_PLAYERS);
	if (who < 0)
		return;
	if (!ownerSeen)
		ForceTrue(script);
	g_spotStandIn = SpotStandIn{script, ip, g_frame, static_cast<uint8_t>(who), at[who]};
	static uint8_t saidWho = INVALID_PLAYER;
	if (saidWho != who) {
		saidWho = static_cast<uint8_t>(who);
		Log("missions: %s's guard spotted %s%s", MissionName(g_own.number),
		    g_client->PlayerSlot(saidWho).nick.c_str(), w[who].safe ? ", where it does not count" : "");
	}
}

int8_t Spotted(void *script, int32_t command, RangeFn original) {
	PeekParams(script, 2);
	const int32_t  charHandle = reinterpret_cast<const int32_t *>(Params())[0];
	const uint32_t ip         = At<uint32_t>(script, layout::SCRIPT_IP);
	const int8_t   r          = original(script, nullptr, command);
	if (g_client && g_own.running && g_own.number == standin::SAYONARA_SALVATORE &&
	    IsMissionScript(script)) {
		if (void *const npc = Func<GetPedFn>(CPools__GetPed)(charHandle))
			SpottedInSayonara(script, npc, ip);
		return r;
	}
	if (!MayWiden(script))
		return r;
	void *const npc = Func<GetPedFn>(CPools__GetPed)(charHandle);
	if (!npc)
		return r;
	for (uint8_t id = 0; id < MAX_PLAYERS; ++id)
		if (void *const ped = ReplicaOf(id))
			if (CanSee(npc, ped)) {
				ForceTrue(script);
				Log("missions: %s's pedestrian spotted %s", MissionName(g_own.number),
				    g_client->PlayerSlot(id).nick.c_str());
				break;
			}
	return r;
}

// IS_PLAYER_SHOOTING_IN_AREA: any participant firing inside it, as their own
// snapshot says (PF_FIRING). Alone, or in an `if or`, where a yes for anybody
// is the block's yes: Bomb Da Base: Act II sends 8-Ball in on "a guard down
// or the player shooting at the docks" (38_frank3.sc, FLAG_BLOKE_IN_AREA_FM3
// 1), and a participant's first shot there went unheard (mission.h,
// QuietCheckMayWiden).
int8_t ShootingInArea(void *script, int32_t command, RangeFn original) {
	PeekParams(script, 5);
	const float    x1 = Params()[1], y1 = Params()[2], x2 = Params()[3], y2 = Params()[4];
	const uint16_t andOr   = At<uint16_t>(script, layout::SCRIPT_AND_OR);
	const bool     notFlag = At<uint8_t>(script, layout::SCRIPT_NOT) != 0;
	const uint8_t  before  = At<uint8_t>(script, layout::SCRIPT_COND_RESULT);
	const int8_t   r       = original(script, nullptr, command);
	if (!g_client || !g_own.running || !IsMissionScript(script) || !QuietCheckMayWiden(andOr))
		return r;
	const float lx = x1 < x2 ? x1 : x2, hx = x1 < x2 ? x2 : x1;
	const float ly = y1 < y2 ? y1 : y2, hy = y1 < y2 ? y2 : y1;
	for (uint8_t id = 0; id < MAX_PLAYERS; ++id) {
		if (id == LocalId())
			continue;
		const RemotePlayer &p = g_client->PlayerSlot(id);
		if (!p.active || !p.haveState || (p.last.flags & PF_FIRING) == 0)
			continue;
		if (p.last.pos.x >= lx && p.last.pos.x <= hx && p.last.pos.y >= ly && p.last.pos.y <= hy) {
			At<uint8_t>(script, layout::SCRIPT_COND_RESULT) = CompareFlagIfTrue(before, andOr, notFlag);
			break;
		}
	}
	return r;
}

// Cutting The Grass's Spookometer: how near Curly Bob the player is, and
// whether in a Mafia car, is answered for whichever player is nearest him.
// Every locate against a pedestrian in that mission is about him (at 40, 30,
// 20 and 25 m for the spooking, 160 m for losing him), and the car test is
// asked right after one. Triads And Tribulations' warlords and escort are
// answered the same way (nearchar.h).
constexpr uint16_t CUTTING_THE_GRASS  = nearchar::CUTTING_THE_GRASS;
int32_t            g_stealthChar      = -1;
uint32_t           g_stealthCharFrame = 0;
uint8_t            g_stealthNearest   = INVALID_PLAYER;

int8_t NearestAtChar(void *script, int32_t charHandle, float rx, float ry, float rz, bool in3d,
                     uint8_t condBefore, uint16_t andOr, bool notFlag);

// Who of the players is nearest the pedestrian a locate asks about, the owner
// counting: INVALID_PLAYER when it is the owner. Cutting The Grass's Mafia
// car test, right after a locate against Curly, asks it of that player.
void NoteNearestToChar(int32_t charHandle) {
	void *const npc = Func<GetPedFn>(CPools__GetPed)(charHandle);
	if (!npc)
		return;
	g_stealthChar      = charHandle;
	g_stealthCharFrame = g_frame;
	g_stealthNearest   = INVALID_PLAYER;
	void *const owner = Func<PlayerFn>(FindPlayerPed)();
	float       best  = owner ? DistanceSq(npc, owner) : 1e18f;
	for (uint8_t id = 0; id < MAX_PLAYERS; ++id)
		if (void *const ped = ReplicaOf(id)) {
			const float d = DistanceSq(npc, ped);
			if (d < best) {
				best             = d;
				g_stealthNearest = id;
			}
		}
}

// LOCATE_PLAYER_ANY_MEANS_CHAR_2D/3D in a mission of nearchar.h's table: the
// owner's no becomes a yes, through the block's own flag, when a participant
// is inside the same box (NearestAtChar).
int8_t LocateNearChar(void *script, int32_t command, RangeFn original) {
	const bool in3d = command == op::LOCATE_PLAYER_ANY_MEANS_CHAR_3D;
	PeekParams(script, in3d ? 5 : 4);
	const int32_t  charHandle = reinterpret_cast<const int32_t *>(Params())[1];
	const float    rx = Params()[2], ry = Params()[3], rz = in3d ? Params()[4] : 1e9f;
	const uint8_t  condBefore = At<uint8_t>(script, layout::SCRIPT_COND_RESULT);
	const uint16_t andOr      = At<uint16_t>(script, layout::SCRIPT_AND_OR);
	const bool     notFlag    = At<uint8_t>(script, layout::SCRIPT_NOT) != 0;
	const int8_t   r          = original(script, nullptr, command);
	if (!g_client || !g_own.running || !nearchar::AnswersForNearest(g_own.number) ||
	    !IsMissionScript(script))
		return r;
	if (g_own.number == CUTTING_THE_GRASS)
		NoteNearestToChar(charHandle);
	NearestAtChar(script, charHandle, rx, ry, rz, in3d, condBefore, andOr, notFlag);
	return r;
}

// A locate against one of the mission's pedestrians in a mission that asks it
// of whoever is on him (nearchar.h): the owner's no becomes the yes it would
// have been, through the and/or block's own arithmetic, when a participant
// stands inside the same box round the pedestrian, or round his car.
bool g_saidNearestAtChar = false;
bool g_saidNearestAtCar  = false;

// The box round `at`, the pedestrian's or the car's position: yes through the
// block's flag when a participant stands inside it. Whom it was about goes in
// the log once, `said` saying whether it has.
void NearestInBox(void *script, const float *at, float rx, float ry, float rz, bool in3d,
                  uint8_t condBefore, uint16_t andOr, bool notFlag, const char *what, int32_t handle,
                  bool *said, nearchar::CarMeans means = nearchar::CarMeans::Any) {
	const uint8_t yes = CompareFlagIfTrue(condBefore, andOr, notFlag);
	if (At<uint8_t>(script, layout::SCRIPT_COND_RESULT) == yes)
		return;
	const uint8_t participants = g_client->Missions().Participants();
	for (uint8_t id = 0; id < MAX_PLAYERS; ++id) {
		if (id == LocalId() || (participants & PlayerBit(id)) == 0)
			continue;
		const RemotePlayer &p = g_client->PlayerSlot(id);
		if (!p.active || !p.haveState || !nearchar::MeansFits(means, p.Seated()))
			continue;
		if (!nearchar::InLocateBox(p.last.pos.x - at[0], p.last.pos.y - at[1], p.last.pos.z - at[2], rx,
		                           ry, rz, in3d))
			continue;
		At<uint8_t>(script, layout::SCRIPT_COND_RESULT) = yes;
		if (!*said) {
			*said = true;
			Log("missions: %s asks whether its player is near %s %d; %s is, and that answers it",
			    MissionName(g_own.number), what, static_cast<int>(handle), p.nick.c_str());
		}
		break;
	}
}

int8_t NearestAtChar(void *script, int32_t charHandle, float rx, float ry, float rz, bool in3d,
                     uint8_t condBefore, uint16_t andOr, bool notFlag) {
	void *const npc = Func<GetPedFn>(CPools__GetPed)(charHandle);
	if (!npc)
		return 0;
	void *const car = SeatedCar(npc);
	NearestInBox(script, &Field<float>(car ? car : npc, offs::POSITION), rx, ry, rz, in3d, condBefore,
	             andOr, notFlag, "pedestrian", charHandle, &g_saidNearestAtChar);
	return 0;
}

// LOCATE_PLAYER_*_CAR_2D/3D against one of the mission's cars, in a mission
// of nearchar.h's car table: the owner's no becomes the yes it would have
// been when a participant stands inside the same box round the car, on foot
// or in a car as the locate asks (Evidence Dash's prosecution, Gone
// Fishing's partner, Paparazzi Purge's spy boat and the Stallion he gets away
// in, Grand Theft Aero's van, Escort Service's truck).
int8_t LocateNearCar(void *script, int32_t command, RangeFn original) {
	nearchar::CarLocate l{};
	nearchar::CarLocateOf(command, &l);
	PeekParams(script, l.is3d ? 5 : 4);
	const int32_t  carHandle  = reinterpret_cast<const int32_t *>(Params())[1];
	const float    rx = Params()[2], ry = Params()[3], rz = l.is3d ? Params()[4] : 1e9f;
	const uint8_t  condBefore = At<uint8_t>(script, layout::SCRIPT_COND_RESULT);
	const uint16_t andOr      = At<uint16_t>(script, layout::SCRIPT_AND_OR);
	const bool     notFlag    = At<uint8_t>(script, layout::SCRIPT_NOT) != 0;
	const int8_t   r          = original(script, nullptr, command);
	if (!g_client || !g_own.running || g_own.finishing ||
	    !nearchar::AnswersNearCarForNearest(g_own.number) || !IsMissionScript(script) ||
	    !nearchar::CarLocateMayWiden(l, andOr) || !IsMissionCar(carHandle))
		return r;
	void *const car = Func<GetPedFn>(CPools__GetVehicle)(carHandle);
	if (!car)
		return r;
	NearestInBox(script, &Field<float>(car, offs::POSITION), rx, ry, rz, l.is3d, condBefore, andOr, notFlag,
	             "car", carHandle, &g_saidNearestAtCar, l.means);
	return r;
}

// IS_PLAYER_IN_MODEL, in Cutting The Grass right after a locate against Curly:
// the car of whoever that locate found nearest him.
int8_t InModelOfNearest(void *script, int32_t command, RangeFn original) {
	PeekParams(script, 2);
	const int32_t model = reinterpret_cast<const int32_t *>(Params())[1];
	const int8_t  r     = original(script, nullptr, command);
	if (!g_client || !g_own.running || g_own.number != CUTTING_THE_GRASS ||
	    !IsMissionScript(script) || g_stealthNearest == INVALID_PLAYER ||
	    g_frame - g_stealthCharFrame > 1 ||
	    At<uint16_t>(script, layout::SCRIPT_AND_OR) != ANDOR_NONE)
		return r;
	const RemotePlayer  &p   = g_client->PlayerSlot(g_stealthNearest);
	const RemoteVehicle *car = p.active && p.Seated() ? g_client->VehicleByNetId(p.seatedVehicleNetId)
	                                                   : nullptr;
	const bool raw = car && car->modelId == model;
	At<uint8_t>(script, layout::SCRIPT_COND_RESULT) =
	    (At<uint8_t>(script, layout::SCRIPT_NOT) != 0) != raw ? 1 : 0;
	return r;
}

// ---- The Fuzz Ball: every player picks up his own girls (game/fuzzball.h) ------
//
// Only on the owner's machine, only in mission 23, only in its own script.
uint8_t g_fuzzSubject     = INVALID_PLAYER;   // the participant a pick-up is answered for
int32_t g_fuzzGirl        = -1;               // the girl it is about
bool    g_saidFuzzPickup  = false;
bool    g_saidFuzzLeader  = false;

bool InFuzzBall(void *script) {
	return g_client && g_own.running && g_own.number == fuzzball::THE_FUZZ_BALL &&
	       IsMissionScript(script);
}

void ForgetFuzzSubject() {
	g_fuzzSubject = INVALID_PLAYER;
	g_fuzzGirl    = -1;
}

// The subject's copy here, still in his car, or null.
void *FuzzSubjectPed() {
	if (g_fuzzSubject == INVALID_PLAYER || !InOwnMission(g_fuzzSubject))
		return nullptr;
	void *const ped = ReplicaOf(g_fuzzSubject);
	return ped && !PedDown(ped) && SeatedCar(ped) ? ped : nullptr;
}

// 00FD, player near a girl in a car: the owner's answer first, then the
// nearest participant in a car inside the same box, who becomes the subject.
int8_t FuzzLocateInCar(void *script, int32_t command, RangeFn original) {
	PeekParams(script, 5);
	const int32_t girlHandle = reinterpret_cast<const int32_t *>(Params())[1];
	const float   rx = Params()[2], ry = Params()[3], rz = Params()[4];
	const int8_t  r  = original(script, nullptr, command);
	ForgetFuzzSubject();
	if (!MayWiden(script))
		return r;
	void *const girl = Func<GetPedFn>(CPools__GetPed)(girlHandle);
	if (!girl || PedDown(girl))
		return r;
	fuzzball::Picker pickers[MAX_PLAYERS];
	const float     *g = &Field<float>(girl, offs::POSITION);
	for (uint8_t id = 0; id < MAX_PLAYERS; ++id) {
		void *const ped = id != LocalId() && InOwnMission(id) ? ReplicaOf(id) : nullptr;
		if (!ped || PedDown(ped))
			continue;
		void *const  car = SeatedCar(ped);
		const float *at  = &Field<float>(car ? car : ped, offs::POSITION);
		pickers[id].dx     = at[0] - g[0];
		pickers[id].dy     = at[1] - g[1];
		pickers[id].dz     = at[2] - g[2];
		pickers[id].seated = car != nullptr;
		pickers[id].valid  = true;
	}
	const bool led = Field<void *>(girl, fuzzball::PED_LEADER) != nullptr;
	const int  id  = fuzzball::ChoosePicker(pickers, MAX_PLAYERS, rx, ry, rz, led);
	if (id < 0)
		return r;
	g_fuzzSubject = static_cast<uint8_t>(id);
	g_fuzzGirl    = girlHandle;
	ForceTrue(script);
	return r;
}

// 029F and 00E0 for the subject: his car stopped, and him in it.
int8_t FuzzSubjectCondition(void *script, int32_t command, RangeFn original) {
	const int8_t r = original(script, nullptr, command);
	if (!MayWiden(script))
		return r;
	void *const ped = FuzzSubjectPed();
	if (!ped)
		return r;
	if (command == fuzzball::OP_IS_PLAYER_STOPPED) {
		const float *v = &Field<float>(SeatedCar(ped), offs::MOVE_SPEED);
		if (!fuzzball::Stopped(v[0], v[1], v[2]))
			return r;
	}
	ForceTrue(script);
	return r;
}

// Past one operand at `ip`, or false for what is not one.
bool SkipOperand(uint32_t &ip) {
	if (ip >= SCRIPT_SPACE_SIZE)
		return false;
	const size_t bytes = fuzzball::OperandBytes(Space()[ip]);
	if (bytes == 0 || ip + 1 + bytes > SCRIPT_SPACE_SIZE)
		return false;
	ip += static_cast<uint32_t>(1 + bytes);
	return true;
}

// 00DA for somebody else's player: the handle of the car `ped` sits in, into
// the variable the script names, and the instruction done without the handler
// (which reads the owner's car).
int8_t StoreCarOf(void *ped, void *script, int32_t command, RangeFn original) {
	uint32_t ip = At<uint32_t>(script, layout::SCRIPT_IP);
	if (!SkipOperand(ip) || ip + 3 > SCRIPT_SPACE_SIZE)
		return original(script, nullptr, command);
	const uint8_t  type  = Space()[ip];
	const uint16_t which = static_cast<uint16_t>(Space()[ip + 1] | (Space()[ip + 2] << 8));
	const int32_t  car   = Func<int32_t(__cdecl *)(void *)>(CPools__GetVehicleRef)(SeatedCar(ped));
	if (type == PARAM_GLOBAL && which >= 8 && which + 4u <= GlobalsEnd(Space(), MAIN_SCRIPT_SIZE))
		At<int32_t>(Space(), which) = car;
	else if (type == PARAM_LOCAL && which < 16)
		At<int32_t>(script, layout::SCRIPT_LOCALS + which * 4u) = car;
	else
		return original(script, nullptr, command);
	At<uint32_t>(script, layout::SCRIPT_IP) = ip + 3;
	return 0;
}

// 00DA for the subject of The Fuzz Ball.
int8_t FuzzStoreCar(void *script, int32_t command, RangeFn original) {
	void *const ped = FuzzSubjectPed();
	if (!ped)
		return original(script, nullptr, command);
	return StoreCarOf(ped, script, command, original);
}

// 01DF for the subject's girl: his copy is her leader, not the owner.
int8_t FuzzLeader(void *script, int32_t command, RangeFn original) {
	PeekParams(script, 1);
	const int32_t girlHandle = reinterpret_cast<const int32_t *>(Params())[0];
	void *const   ped        = FuzzSubjectPed();
	if (!ped || girlHandle != g_fuzzGirl)
		return original(script, nullptr, command);
	uint32_t ip = At<uint32_t>(script, layout::SCRIPT_IP);
	Func<CollectFn>(CTheScripts__CollectParameters)(script, &ip, 2);
	At<uint32_t>(script, layout::SCRIPT_IP) = ip;
	RunOurs(fuzzball::OP_SET_CHAR_AS_LEADER, {girlHandle, PedRef(ped)});
	if (!g_saidFuzzPickup) {
		g_saidFuzzPickup = true;
		Log("missions: %s stopped his own car by one of The Fuzz Ball's girls; she follows him, "
		    "and counts when he drops her at the station",
		    g_client->PlayerSlot(g_fuzzSubject).nick.c_str());
	}
	return 0;
}

// 0320, the girl in the player's group: a participant's copy leading her is
// the player, for the length of the handler.
int8_t FuzzInGroup(void *script, int32_t command, RangeFn original) {
	PeekParams(script, 1);
	void *const girl = Func<GetPedFn>(CPools__GetPed)(reinterpret_cast<const int32_t *>(Params())[0]);
	void *const me   = Func<PlayerFn>(FindPlayerPed)();
	void *const lead = girl ? Field<void *>(girl, fuzzball::PED_LEADER) : nullptr;
	const uint8_t id = lead ? PlayerOfReplica(lead) : INVALID_PLAYER;
	if (!me || id == INVALID_PLAYER || !InOwnMission(id))
		return original(script, nullptr, command);
	Field<void *>(girl, fuzzball::PED_LEADER) = me;
	const int8_t r = original(script, nullptr, command);
	Field<void *>(girl, fuzzball::PED_LEADER) = lead;
	if (!g_saidFuzzLeader) {
		g_saidFuzzLeader = true;
		Log("missions: a girl of The Fuzz Ball follows %s, and the script counts her as the "
		    "player's", g_client->PlayerSlot(id).nick.c_str());
	}
	return r;
}

// ---- Donald Love's and King Courtney's Staunton missions (standin.h) -------------
//
// Only on the owner's machine, only in the mission's own script, and only
// ever turning a no into a yes the group gave.

bool g_saidAnybodyInModel = false;

// The participant `id`'s copy here, standing or seated, in the owner's mission.
void *ParticipantPed(uint8_t id) {
	if (id == LocalId() || !InOwnMission(id))
		return nullptr;
	void *const ped = ReplicaOf(id);
	return ped && !PedDown(ped) ? ped : nullptr;
}

int32_t CarModel(void *car) { return car ? Field<int16_t>(car, offs::MODEL_INDEX) : -1; }

// IS_PLAYER_IN_MODEL in Liberator (single) and S.A.M. (three to a block):
// yes, through the block's own flag, when a participant sits in a car of
// that model.
int8_t AnybodyInModel(void *script, int32_t command, RangeFn original) {
	PeekParams(script, 2);
	const int32_t  model   = reinterpret_cast<const int32_t *>(Params())[1];
	const uint8_t  before  = At<uint8_t>(script, layout::SCRIPT_COND_RESULT);
	const uint16_t andOr   = At<uint16_t>(script, layout::SCRIPT_AND_OR);
	const bool     notFlag = At<uint8_t>(script, layout::SCRIPT_NOT) != 0;
	const int8_t   r       = original(script, nullptr, command);
	if (!g_client || !g_own.running || g_own.finishing || !IsMissionScript(script))
		return r;
	const uint8_t yes = CompareFlagIfTrue(before, andOr, notFlag);
	if (At<uint8_t>(script, layout::SCRIPT_COND_RESULT) == yes)
		return r;
	for (uint8_t id = 0; id < MAX_PLAYERS; ++id) {
		void *const ped = ParticipantPed(id);
		if (!ped || CarModel(SeatedCar(ped)) != model)
			continue;
		At<uint8_t>(script, layout::SCRIPT_COND_RESULT) = yes;
		if (!g_saidAnybodyInModel) {
			g_saidAnybodyInModel = true;
			Log("missions: %s asks whether its player is in a model %d; %s is, and that answers it",
			    MissionName(g_own.number), static_cast<int>(model),
			    g_client->PlayerSlot(id).nick.c_str());
		}
		break;
	}
	return r;
}

// Gangcar Round-Up: with the owner not in a gang car, the participant who
// is, for IS_PLAYER_IN_ANY_CAR, IS_PLAYER_IN_MODEL and STORE_CAR_PLAYER_IS_IN
// (standin.h, RoundUpSubject). Chosen at each IS_PLAYER_IN_ANY_CAR, which
// every one of the script's model checks and stores follows in the same frame.
uint8_t  g_roundUpSubject      = INVALID_PLAYER;
uint32_t g_roundUpSubjectFrame = 0;
bool     g_saidRoundUp         = false;

bool InRoundUp(void *script) {
	return g_client && g_own.running && !g_own.finishing &&
	       g_own.number == standin::GANGCAR_ROUND_UP && IsMissionScript(script);
}

void *RoundUpSubjectPed() {
	if (g_roundUpSubject == INVALID_PLAYER || g_roundUpSubjectFrame != g_frame)
		return nullptr;
	void *const ped = ParticipantPed(g_roundUpSubject);
	return ped && SeatedCar(ped) ? ped : nullptr;
}

int8_t RoundUpInAnyCar(void *script, int32_t command, RangeFn original) {
	const int8_t r = original(script, nullptr, command);
	g_roundUpSubject = INVALID_PLAYER;
	int32_t models[MAX_PLAYERS];
	for (uint8_t id = 0; id < MAX_PLAYERS; ++id) {
		void *const ped = ParticipantPed(id);
		models[id]      = ped ? CarModel(SeatedCar(ped)) : -1;
	}
	const int subject = standin::RoundUpSubject(
	    CarModel(SeatedCar(Func<PlayerFn>(FindPlayerPed)())), models, MAX_PLAYERS);
	if (subject < 0)
		return r;
	g_roundUpSubject      = static_cast<uint8_t>(subject);
	g_roundUpSubjectFrame = g_frame;
	if (MayWiden(script))
		ForceTrue(script);
	if (!g_saidRoundUp) {
		g_saidRoundUp = true;
		Log("missions: %s sits in a gang car of Gangcar Round-Up; the mission takes his car as "
		    "its player's", g_client->PlayerSlot(g_roundUpSubject).nick.c_str());
	}
	return r;
}

// IS_PLAYER_IN_MODEL, single, for the subject: his car's model, the owner's
// answer replaced (the owner sits in none of the three, or there would be no
// subject).
int8_t RoundUpInModel(void *script, int32_t command, RangeFn original) {
	PeekParams(script, 2);
	const int32_t model = reinterpret_cast<const int32_t *>(Params())[1];
	const int8_t  r     = original(script, nullptr, command);
	void *const   ped   = RoundUpSubjectPed();
	if (!ped || At<uint16_t>(script, layout::SCRIPT_AND_OR) != ANDOR_NONE)
		return r;
	const bool raw = CarModel(SeatedCar(ped)) == model;
	At<uint8_t>(script, layout::SCRIPT_COND_RESULT) =
	    (At<uint8_t>(script, layout::SCRIPT_NOT) != 0) != raw ? 1 : 0;
	return r;
}

int8_t RoundUpStoreCar(void *script, int32_t command, RangeFn original) {
	void *const ped = RoundUpSubjectPed();
	if (!ped)
		return original(script, nullptr, command);
	return StoreCarOf(ped, script, command, original);
}

// ---- The Getaway: the robbers ride with whoever drives them (game/getaway.h) -------
//
// Only on the owner's machine, only in mission 29, only in its own script. The
// owner as the robbers' driver is the script as it always was; a guest as the
// driver answers the questions about "the player" in the owner's place.
uint8_t  g_gwDriver               = INVALID_PLAYER;
bool     g_gwRobbersSeated        = false;
bool     g_gwHasGuest             = false;   // somebody besides the owner is in it
uint32_t g_gwFrame                = 0xFFFFFFFFu;
uint32_t g_gwArrived[MAX_PLAYERS] = {};
uint32_t g_gwSeq                  = 0;
bool     g_saidGetawayGuest       = false;
bool     g_saidGetawayNobody      = false;
bool     g_saidGetawayHint        = false;
uint8_t  g_gwPrintLabel[TEXT_LABEL] = {};
uint32_t g_gwPrintMs              = 0;

// The blips the owner's script made above a robber, and whose they are.
struct GetawayBlip {
	int32_t handle;
	uint8_t viewer;
};
constexpr size_t GETAWAY_BLIPS = 8;
GetawayBlip      g_gwBlips[GETAWAY_BLIPS];
size_t           g_gwBlipCount = 0;

void ForgetGetaway() {
	g_gwDriver          = INVALID_PLAYER;
	g_gwRobbersSeated   = false;
	g_gwHasGuest        = false;
	g_gwFrame           = 0xFFFFFFFFu;
	std::memset(g_gwArrived, 0, sizeof g_gwArrived);
	g_gwSeq             = 0;
	g_saidGetawayGuest  = false;
	g_saidGetawayNobody = false;
	g_saidGetawayHint   = false;
	std::memset(g_gwPrintLabel, 0, sizeof g_gwPrintLabel);
	g_gwPrintMs         = 0;
	g_gwBlipCount       = 0;
}

bool InGetaway(void *script) {
	return g_client && g_own.running && !g_own.finishing && getaway::RideOf(g_own.number) &&
	       IsMissionScript(script);
}

// The ped of player `id` here when he is in the owner's mission: ours for the
// owner, a participant's copy for the rest.
void *GetawayPedOf(uint8_t id) {
	return id == LocalId() ? Func<PlayerFn>(FindPlayerPed)() : ParticipantPed(id);
}

// Who drives the robbers, once a frame (getaway.h, ChooseDriver).
void GetawayRefresh() {
	if (g_gwFrame == g_frame)
		return;
	g_gwFrame = g_frame;
	const getaway::Ride *const rideOf = getaway::RideOf(g_own.number);
	if (!rideOf) {
		g_gwDriver = INVALID_PLAYER;
		return;
	}
	const getaway::Ride &ride = *rideOf;
	void        *peds[16];
	void        *robbers[8];
	size_t       alive      = 0;
	void        *robbersCar = nullptr;
	const size_t n          = HostedMissionPeds(peds, sizeof peds / sizeof peds[0]);
	for (size_t i = 0; i < n && alive < sizeof robbers / sizeof robbers[0]; ++i) {
		if (PedDown(peds[i]))
			continue;
		robbers[alive++] = peds[i];
		if (!robbersCar)
			robbersCar = SeatedCar(peds[i]);
	}
	const int need = getaway::SeatsNeeded(static_cast<int>(alive), ride);
	getaway::Candidate c[MAX_PLAYERS];
	g_gwHasGuest = false;
	for (uint8_t id = 0; id < MAX_PLAYERS; ++id) {
		void *const ped = GetawayPedOf(id);
		if (!ped) {
			g_gwArrived[id] = 0;
			continue;
		}
		void *const car     = SeatedCar(ped);
		c[id].valid         = true;
		g_gwHasGuest        = g_gwHasGuest || id != LocalId();
		c[id].drives        = car && Field<void *>(car, offs::VEH_DRIVER) == ped;
		c[id].inRobbersCar  = car && car == robbersCar;
		c[id].drivesRobbers = c[id].drives && c[id].inRobbersCar;
		if (c[id].drives) {
			uint8_t max = Field<uint8_t>(car, offs::VEH_NUM_MAX_PASSENGERS);
			if (max > offs::VEH_MAX_PASSENGERS)
				max = static_cast<uint8_t>(offs::VEH_MAX_PASSENGERS);
			const uint8_t taken = Field<uint8_t>(car, offs::VEH_NUM_PASSENGERS);
			c[id].freeSeats     = max > taken ? max - taken : 0;
		}
		const float *at = &Field<float>(car ? car : ped, offs::POSITION);
		if (alive == 0) {
			c[id].nearby = getaway::AtThePickup(at[0], at[1], ride);
		} else {
			for (size_t i = 0; i < alive && !c[id].nearby; ++i) {
				const float *r  = &Field<float>(robbers[i], offs::POSITION);
				const float  dx = r[0] - at[0], dy = r[1] - at[1];
				c[id].nearby = dx * dx + dy * dy <=
				             ride.passengerReachM * ride.passengerReachM;
			}
		}
		if (getaway::CouldTakeThem(c[id], need)) {
			if (g_gwArrived[id] == 0)
				g_gwArrived[id] = ++g_gwSeq;
		} else {
			g_gwArrived[id] = 0;
		}
		c[id].arrived = g_gwArrived[id];
	}
	const int previous = g_gwDriver == INVALID_PLAYER ? -1 : g_gwDriver;
	const int pick     = getaway::ChooseDriver(c, MAX_PLAYERS, LocalId(), need, robbersCar != nullptr,
	                                           previous, alive > 0);
	g_gwDriver        = pick < 0 ? INVALID_PLAYER : static_cast<uint8_t>(pick);
	g_gwRobbersSeated = robbersCar != nullptr;
	if (g_gwDriver != INVALID_PLAYER && g_gwDriver != LocalId() && !g_saidGetawayGuest) {
		g_saidGetawayGuest = true;
		Log("missions: %s takes the robbers of %s in his own car; the mission asks about him "
		    "where it asks about its player",
		    g_client->PlayerSlot(g_gwDriver).nick.c_str(), MissionName(g_own.number));
	}
	if (g_gwRobbersSeated && g_gwDriver == INVALID_PLAYER && !g_saidGetawayNobody) {
		g_saidGetawayNobody = true;
		Log("missions: nobody of the group sits in the car the robbers of %s are in; the owner in "
		    "another car is not its player there", MissionName(g_own.number));
	}
}

// The guest who drives the robbers, his copy here, or null: the owner, or
// nobody, is the script as it was.
void *GetawayGuestPed() {
	GetawayRefresh();
	if (g_gwDriver == INVALID_PLAYER || g_gwDriver == LocalId())
		return nullptr;
	void *const ped = ParticipantPed(g_gwDriver);
	return ped && SeatedCar(ped) ? ped : nullptr;
}

// The robbers in a car with nobody of the group in it. Alone, the owner changing
// cars is the script as it always was: the robbers follow him into the new one.
bool GetawayNobodyInCar() {
	GetawayRefresh();
	return g_gwRobbersSeated && g_gwDriver == INVALID_PLAYER && g_gwHasGuest;
}

// The condition the engine just answered for the owner, said again as `raw`.
void GetawayAnswer(void *script, uint8_t before, uint16_t andOr, bool notFlag, bool raw) {
	At<uint8_t>(script, layout::SCRIPT_COND_RESULT) = CompareFlagFor(before, andOr, notFlag, raw);
}

// IS_PLAYER_IN_ANY_CAR, STORE_CAR_PLAYER_IS_IN, IS_PLAYER_IN_MODEL,
// IS_PLAYER_PRESSING_HORN and LOCATE_PLAYER_ANY_MEANS_CHAR_2D, for the guest
// who drives the robbers; and IS_PLAYER_IN_ANY_CAR no, for the robbers in a
// car nobody of the group sits in. False for anything the owner answers
// himself, which is then run as it was.
bool GetawayCommand(void *script, int32_t command, RangeFn original, int8_t *out) {
	if (command != getaway::OP_IS_PLAYER_IN_ANY_CAR && command != getaway::OP_STORE_CAR_PLAYER_IS_IN &&
	    command != getaway::OP_IS_PLAYER_IN_MODEL && command != getaway::OP_IS_PLAYER_PRESSING_HORN &&
	    command != getaway::OP_LOCATE_PLAYER_ANY_MEANS_CHAR_2D &&
	    command != getaway::OP_IS_PLAYER_SITTING_IN_ANY_CAR)
		return false;
	void *const    guest   = GetawayGuestPed();
	const uint8_t  before  = At<uint8_t>(script, layout::SCRIPT_COND_RESULT);
	const uint16_t andOr   = At<uint16_t>(script, layout::SCRIPT_AND_OR);
	const bool     notFlag = At<uint8_t>(script, layout::SCRIPT_NOT) != 0;
	if (!guest) {
		if ((command != getaway::OP_IS_PLAYER_IN_ANY_CAR &&
		     command != getaway::OP_IS_PLAYER_SITTING_IN_ANY_CAR) ||
		    !GetawayNobodyInCar())
			return false;
		*out = original(script, nullptr, command);
		GetawayAnswer(script, before, andOr, notFlag, false);
		return true;
	}
	if (command == getaway::OP_STORE_CAR_PLAYER_IS_IN) {
		*out = StoreCarOf(guest, script, command, original);
		return true;
	}
	bool raw = true;
	if (command == getaway::OP_IS_PLAYER_IN_MODEL) {
		PeekParams(script, 2);
		raw = CarModel(SeatedCar(guest)) == reinterpret_cast<const int32_t *>(Params())[1];
	} else if (command == getaway::OP_IS_PLAYER_PRESSING_HORN) {
		const RemotePlayer  &p   = g_client->PlayerSlot(g_gwDriver);
		const RemoteVehicle *car = p.active && p.Seated() ? g_client->VehicleByNetId(p.seatedVehicleNetId)
		                                                   : nullptr;
		raw = car && car->hornSounding;
	} else if (command == getaway::OP_LOCATE_PLAYER_ANY_MEANS_CHAR_2D) {
		PeekParams(script, 4);
		void *const npc = Func<GetPedFn>(CPools__GetPed)(reinterpret_cast<const int32_t *>(Params())[1]);
		if (!npc)
			return false;
		const float  rx = Params()[2], ry = Params()[3];
		const float *a  = &Field<float>(SeatedCar(npc) ? SeatedCar(npc) : npc, offs::POSITION);
		const float *b  = &Field<float>(SeatedCar(guest), offs::POSITION);
		raw = nearchar::InLocateBox(b[0] - a[0], b[1] - a[1], 0.0f, rx, ry, 0.0f, false);
	}
	*out = original(script, nullptr, command);
	GetawayAnswer(script, before, andOr, notFlag, raw);
	return true;
}

// A location condition of the mission, asked of its player: the guest who
// drives the robbers answers it as himself, the owner as the owner, and with
// the robbers in a car nobody of the group sits in, only what is asked on
// foot stays the owner's. Never a checkpoint for everybody to reach: the
// group is not in one car, and the one that matters is the robbers'. True once
// it is this rule's.
bool GetawayPlace(void *script, int32_t command, uint16_t andOr, bool notFlag, uint8_t before) {
	standin::PlaceNeeds needs{};
	MissionArea         area{};
	if (!standin::PlaceNeedsOf(command, &needs) || !AreaForCondition(command, Params(), &area))
		return false;
	if (GetawayGuestPed()) {
		const anyplace::Candidate who = PlaceCandidate(g_gwDriver, nullptr);
		GetawayAnswer(script, before, andOr, notFlag, anyplace::Answers(needs, area, who, false));
	} else if (GetawayNobodyInCar() && !getaway::NobodyAnswersAsOwner(needs.onFoot)) {
		GetawayAnswer(script, before, andOr, notFlag, false);
	}
	return true;
}

// What the script prints about the car goes to the one who has to deal with
// it (getaway.h, IsCrewLabel). Ours, where the engine has just shown it, when
// that is the owner; otherwise taken off here and shown to the guest, no more
// than once a second, which is as often as the script's own loop reprints it.
bool GetawayCrewPrint(int32_t command, const uint8_t *code, size_t length) {
	if (length < 2 + TEXT_LABEL ||
	    (command != op::PRINT && command != op::PRINT_NOW && command != op::PRINT_SOON) ||
	    !getaway::IsCrewLabel(code + 2, *getaway::RideOf(g_own.number)))
		return false;
	GetawayRefresh();
	const uint8_t viewer = g_gwDriver != INVALID_PLAYER ? g_gwDriver : LocalId();
	if (viewer == LocalId())
		return true;
	uint8_t clear[2 + TEXT_LABEL] = {static_cast<uint8_t>(op::CLEAR_THIS_PRINT & 0xFF),
	                                 static_cast<uint8_t>(op::CLEAR_THIS_PRINT >> 8)};
	std::memcpy(clear + 2, code + 2, TEXT_LABEL);
	RunHere(clear, sizeof clear, nullptr);
	const uint32_t now = WallClock::NowMs();
	if (std::memcmp(g_gwPrintLabel, code + 2, TEXT_LABEL) == 0 && now - g_gwPrintMs < 1000)
		return true;
	std::memcpy(g_gwPrintLabel, code + 2, TEXT_LABEL);
	g_gwPrintMs = now;
	g_client->Missions().SendEffect(GetBackInEffect(g_own.number, g_gwPrintLabel, viewer), now);
	if (!g_saidGetawayHint) {
		g_saidGetawayHint = true;
		Log("missions: what %s says about the car is shown to %s, who drives the robbers, and "
		    "not to the owner", MissionName(g_own.number), g_client->PlayerSlot(viewer).nick.c_str());
	}
	return true;
}

// The marker the script puts above a robber who has fallen behind is for the
// one who has to go back for him. The owner's own: kept off the wire. The
// guest's: sent to him alone and put out here. Remembered by handle, so the
// marker's end goes the same way (GetawayBlipTarget).
bool GetawayCrewBlip(int32_t handle, MissionEffectBody &body) {
	GetawayRefresh();
	const uint8_t viewer = g_gwDriver != INVALID_PLAYER ? g_gwDriver : LocalId();
	if (g_gwBlipCount < GETAWAY_BLIPS)
		g_gwBlips[g_gwBlipCount++] = GetawayBlip{handle, viewer};
	if (viewer == LocalId())
		return true;
	RunOurs(op::CHANGE_BLIP_DISPLAY, {handle, 0});
	body.onlyTo = static_cast<uint8_t>(viewer + 1);
	return false;
}

// A later instruction about one of those markers: where it goes, and whether.
bool GetawayBlipTarget(int32_t handle, int32_t command, MissionEffectBody &body) {
	for (size_t i = 0; i < g_gwBlipCount; ++i) {
		if (g_gwBlips[i].handle != handle)
			continue;
		const uint8_t viewer = g_gwBlips[i].viewer;
		if (command == op::REMOVE_BLIP)
			g_gwBlips[i] = g_gwBlips[--g_gwBlipCount];
		if (viewer == LocalId())
			return true;
		body.onlyTo = static_cast<uint8_t>(viewer + 1);
		return false;
	}
	return false;
}

// Waka-Gashira Wipeout!'s car park (missioncombat.h, stealth): a participant
// on foot there, or upstairs in anything but a Colombian car, gives the hit
// away as the owner would. The condition before the car park in its block is
// remembered as it runs.
struct WakaPrev {
	void    *script  = nullptr;
	uint32_t frame   = 0;
	int32_t  command = 0;
	bool     notFlag = false;
	int32_t  model   = -1;
};
WakaPrev g_wakaPrev;
void    *g_wakaUpperScript = nullptr;   // upstairs was given away; the model check follows
uint32_t g_wakaUpperFrame  = 0;
bool     g_saidWaka        = false;

bool InWaka(void *script) {
	return g_client && g_own.running && !g_own.finishing &&
	       g_own.number == stealth::WAKA_GASHIRA_WIPEOUT && IsMissionScript(script);
}

void NoteWakaCondition(void *script, int32_t command) {
	if (!InWaka(script))
		return;
	g_wakaPrev.script  = script;
	g_wakaPrev.frame   = g_frame;
	g_wakaPrev.command = command;
	g_wakaPrev.notFlag = At<uint8_t>(script, layout::SCRIPT_NOT) != 0;
	g_wakaPrev.model   = -1;
	if (command == 0x00DE) {
		PeekParams(script, 2);
		g_wakaPrev.model = reinterpret_cast<const int32_t *>(Params())[1];
	}
}

bool WatcherOf(uint8_t id, stealth::Watcher *w);

// Whoever of the participants gives the car park away, or INVALID_PLAYER.
uint8_t WakaGivenAway(stealth::Carpark box, stealth::CarparkAsk ask) {
	for (uint8_t id = 0; id < MAX_PLAYERS; ++id) {
		stealth::Watcher w;
		if (WatcherOf(id, &w) && stealth::BlowsCarparkCover(w, box, ask))
			return id;
	}
	return INVALID_PLAYER;
}

void SayWaka(uint8_t id) {
	if (g_saidWaka)
		return;
	g_saidWaka = true;
	Log("missions: %s is in the Newport car park out of a Colombian car; the Yakuza have "
	    "identified the group", g_client->PlayerSlot(id).nick.c_str());
}

// IS_PLAYER_IN_AREA_3D in Waka-Gashira, answered: the two `if and`s ending
// on the car park, and the upstairs check alone.
void WakaCarpark(void *script, int32_t command, uint16_t andOr, bool notFlag) {
	if (command != nearchar::OP_IS_PLAYER_IN_AREA_3D || notFlag || !InWaka(script))
		return;
	const float *p   = Params();
	const auto   box = stealth::CarparkOf(p[1], p[2], p[3], p[4], p[5], p[6]);
	if (box == stealth::Carpark::None)
		return;
	if (andOr == ANDOR_NONE && box == stealth::Carpark::Upper) {
		const uint8_t id = WakaGivenAway(box, stealth::CarparkAsk::NotColombian);
		if (id == INVALID_PLAYER)
			return;
		At<uint8_t>(script, layout::SCRIPT_COND_RESULT) = 1;
		g_wakaUpperScript                               = script;
		g_wakaUpperFrame                                = g_frame;
		SayWaka(id);
		return;
	}
	stealth::CarparkAsk ask;
	if (andOr != 1 || box != stealth::Carpark::Whole || g_wakaPrev.script != script ||
	    g_wakaPrev.frame != g_frame ||
	    !stealth::CarparkAskOf(g_wakaPrev.command, g_wakaPrev.notFlag, g_wakaPrev.model, &ask))
		return;
	const uint8_t id = WakaGivenAway(box, ask);
	if (id == INVALID_PLAYER)
		return;
	// The last condition of a two-condition `if and`: the block's answer.
	At<uint8_t>(script, layout::SCRIPT_COND_RESULT) = 1;
	SayWaka(id);
}

// NOT IS_PLAYER_IN_MODEL #COLUMB right after a participant gave upstairs
// away: he is the one not in a Colombian car.
bool WakaUpperModel(void *script) {
	if (g_wakaUpperScript != script || g_frame - g_wakaUpperFrame > 1 ||
	    At<uint16_t>(script, layout::SCRIPT_AND_OR) != ANDOR_NONE ||
	    At<uint8_t>(script, layout::SCRIPT_NOT) == 0 || g_wakaPrev.model != stealth::COLOMBIAN_CAR)
		return false;
	g_wakaUpperScript                               = nullptr;
	At<uint8_t>(script, layout::SCRIPT_COND_RESULT) = 1;
	return true;
}

// IS_PLAYER_IN_MODEL, by mission.
int8_t InModel(void *script, int32_t command, RangeFn original) {
	if (InRoundUp(script))
		return RoundUpInModel(script, command, original);
	if (InWaka(script)) {
		NoteWakaCondition(script, command);
		const int8_t r = original(script, nullptr, command);
		WakaUpperModel(script);
		return r;
	}
	if (g_own.running && standin::AnybodyInModel(g_own.number) && IsMissionScript(script))
		return AnybodyInModel(script, command, original);
	return InModelOfNearest(script, command, original);
}

// ---- Marty's passengers: a participant driving them is the player near them --------
//
// LOCATE_PLAYER_*_CHAR in The Crook, The Thieves, The Wife and Her Lover: the
// owner's answer first, then a participant sitting in one of the mission's
// cars, or in the car the pedestrian rides in, inside the box (standin.h).
bool g_saidEscort = false;

int8_t EscortLocate(void *script, int32_t command, const standin::CharLocate &l, RangeFn original) {
	PeekParams(script, l.is3d ? 5 : 4);
	const int32_t  charHandle = reinterpret_cast<const int32_t *>(Params())[1];
	const float    rx = Params()[2], ry = Params()[3], rz = l.is3d ? Params()[4] : 0.0f;
	const uint16_t andOr      = At<uint16_t>(script, layout::SCRIPT_AND_OR);
	const uint8_t  condBefore = At<uint8_t>(script, layout::SCRIPT_COND_RESULT);
	const bool     notFlag    = At<uint8_t>(script, layout::SCRIPT_NOT) != 0;
	const int8_t   r          = original(script, nullptr, command);
	if (!g_client || g_own.finishing)
		return r;
	void *const npc = Func<GetPedFn>(CPools__GetPed)(charHandle);
	if (!npc || PedDown(npc))
		return r;
	void *const   charCar = SeatedCar(npc);
	const float  *c       = &Field<float>(charCar ? charCar : npc, offs::POSITION);
	standin::Escort escorts[MAX_PLAYERS];
	for (uint8_t id = 0; id < MAX_PLAYERS; ++id) {
		void *const ped = id != LocalId() && InOwnMission(id) ? ReplicaOf(id) : nullptr;
		if (!ped || PedDown(ped))
			continue;
		void *const  car = SeatedCar(ped);
		const float *at  = &Field<float>(car ? car : ped, offs::POSITION);
		standin::Escort &e = escorts[id];
		e.dx           = at[0] - c[0];
		e.dy           = at[1] - c[1];
		e.dz           = at[2] - c[2];
		e.seated       = car != nullptr;
		e.inMissionCar = car && IsMissionCar(CarHandle(car));
		e.withChar     = car && car == charCar;
		e.valid        = true;
	}
	const int id = standin::EscortFor(escorts, MAX_PLAYERS, l, rx, ry, rz);
	if (id < 0)
		return r;
	At<uint8_t>(script, layout::SCRIPT_COND_RESULT) = CompareFlagIfTrue(condBefore, andOr, notFlag);
	if (!g_saidEscort) {
		g_saidEscort = true;
		Log("missions: %s drives %s's passenger, and is the player near him",
		    g_client->PlayerSlot(static_cast<uint8_t>(id)).nick.c_str(), MissionName(g_own.number));
	}
	return r;
}

// IS_PLAYER_SITTING_IN_CAR in Give Me Liberty, at the hideout: not widened,
// but with the Kuruma there and a participant in it, the owner is told to get
// in rather than left waiting with nothing on his screen (standin.h).
constexpr int32_t OP_IS_PLAYER_SITTING_IN_CAR = 0x0442;
uint32_t          g_sitHintMs                 = 0;

int8_t SittingInCar(void *script, int32_t command, RangeFn original) {
	PeekParams(script, 2);
	const int32_t handle = reinterpret_cast<const int32_t *>(Params())[1];
	const int8_t  r      = original(script, nullptr, command);
	if (!g_client || !g_own.running || g_own.finishing || g_own.number != standin::GIVE_ME_LIBERTY ||
	    !IsMissionScript(script))
		return r;
	void *const car = Func<GetPedFn>(CPools__GetVehicle)(handle);
	void *const me  = Func<PlayerFn>(FindPlayerPed)();
	if (!car || !me || SeatedCar(me) == car)
		return r;
	bool participantIn = false;
	for (uint8_t id = 0; id < MAX_PLAYERS && !participantIn; ++id)
		participantIn = id != LocalId() && InOwnMission(id) && ReplicaSeatedIn(id, car);
	const float *at       = &Field<float>(car, offs::POSITION);
	const bool   sameBlock = g_locationScript == script && g_locationFrame == g_frame;
	const uint32_t now     = WallClock::NowMs();
	if (!participantIn || !InCarAtThePlace(Vec3{at[0], at[1], at[2]}, sameBlock && g_locationHasArea,
	                                       g_locationArea) ||
	    !standin::SitHintDue(g_sitHintMs, now))
		return r;
	g_sitHintMs                = now != 0 ? now : 1;
	const MissionEffectBody hint = GetBackInEffect(g_own.number, g_getBackInLabel, LocalId());
	RunHere(hint.code, hint.length, nullptr);
	Log("missions: %s waits at the place for its player to sit in car %d, where a participant "
	    "is; the owner is told to get in", MissionName(g_own.number), handle);
	return r;
}

// Deal Steal's rendezvous and Plaster Blaster's decoy (missioncombat.h,
// stealth): a participant gives the game away there too. Each participant
// as their own snapshot has them: where, in which car, and firing.
constexpr int32_t OP_LOCATE_PLAYER_ANY_MEANS_2D = 0x00E3;
constexpr int32_t OP_LOCATE_PLAYER_ANY_MEANS_3D = 0x00F5;
constexpr int32_t OP_IS_PLAYER_SHOOTING         = 0x02DF;
uint32_t          g_dealBlownFrame              = 0;
bool              g_dealBlown                   = false;

bool WatcherOf(uint8_t id, stealth::Watcher *w) {
	if (id == LocalId() || !InOwnMission(id))
		return false;
	const RemotePlayer &p = g_client->PlayerSlot(id);
	if (!p.active || !p.haveState)
		return false;
	w->x      = p.last.pos.x;
	w->y      = p.last.pos.y;
	w->z      = p.last.pos.z;
	w->firing = (p.last.flags & PF_FIRING) != 0;
	const RemoteVehicle *car = p.Seated() ? g_client->VehicleByNetId(p.seatedVehicleNetId) : nullptr;
	w->seated   = car != nullptr;
	w->carModel = car ? static_cast<int32_t>(car->modelId) : -1;
	return true;
}

int8_t StealthLocate(void *script, int32_t command, RangeFn original) {
	PeekParams(script, 6);
	const float    x = Params()[1], y = Params()[2], rx = Params()[3], ry = Params()[4];
	const uint16_t andOr  = At<uint16_t>(script, layout::SCRIPT_AND_OR);
	const uint8_t  before = At<uint8_t>(script, layout::SCRIPT_COND_RESULT);
	const int8_t   r      = Location(script, command, original);
	// Not NOTed, of the session's mission here: the rendezvous alone, the
	// decoy alone or in an `if and` beside the script's own flag (two of its
	// three locates, once the ambulance has turned for the hospital).
	if (!g_client || !g_own.running || !IsMissionScript(script) ||
	    At<uint8_t>(script, layout::SCRIPT_NOT) != 0)
		return r;
	const bool deal  = g_own.number == stealth::DEAL_STEAL && andOr == ANDOR_NONE &&
	                  stealth::IsDealRendezvous(x, y);
	const bool decoy = g_own.number == stealth::PLASTER_BLASTER && stealth::IsDecoyLocate(rx, ry) &&
	                   stealth::DecoyMayWidenUnder(andOr);
	if (!deal && !decoy)
		return r;
	for (uint8_t id = 0; id < MAX_PLAYERS; ++id) {
		stealth::Watcher w;
		if (!WatcherOf(id, &w))
			continue;
		if (deal ? !stealth::BlowsDealCover(w, x, y, rx, ry) : !stealth::SpotsDecoy(w, x, y, rx, ry))
			continue;
		// The owner there already is the locate's answer; what the owner's
		// car and trigger finger say is the group's, widened below. The
		// decoy's yes goes through its block's own arithmetic.
		if (decoy)
			At<uint8_t>(script, layout::SCRIPT_COND_RESULT) = CompareFlagIfTrue(before, andOr, false);
		else if (!RawResult(script))
			ForceTrue(script);
		if (deal) {
			g_dealBlown      = true;
			g_dealBlownFrame = g_frame;
		}
		static uint32_t saidAt = 0;
		if (saidAt == 0 || g_frame - saidAt > 600) {
			saidAt = g_frame | 1;
			Log("missions: %s gave %s away (%s)", g_client->PlayerSlot(id).nick.c_str(),
			    MissionName(g_own.number),
			    deal ? (w.firing ? "shooting at the rendezvous" : "at the rendezvous in the wrong car")
			         : "within 25 m of the decoy");
		}
		break;
	}
	return r;
}

// Evidence Dash's files and S.A.M.'s cargo (standin.h, CollectSlack): a
// locate of the player at the object the owner's copy dropped is yes for a
// participant on it.
bool g_saidEvidence = false;

int8_t EvidenceLocate(void *script, int32_t command, RangeFn original) {
	const bool in3d = command == OP_LOCATE_PLAYER_ANY_MEANS_3D;
	PeekParams(script, in3d ? 7 : 5);
	const float    x = Params()[1], y = Params()[2], z = in3d ? Params()[3] : 0.0f;
	const float    rx = Params()[in3d ? 4 : 3], ry = Params()[in3d ? 5 : 4];
	const float    rz = in3d ? Params()[6] : 0.0f;
	const uint8_t  condBefore = At<uint8_t>(script, layout::SCRIPT_COND_RESULT);
	const uint16_t andOr      = At<uint16_t>(script, layout::SCRIPT_AND_OR);
	const bool     notFlag    = At<uint8_t>(script, layout::SCRIPT_NOT) != 0;
	const int8_t   r          = in3d ? Location(script, command, original)
	                                 : StealthLocate(script, command, original);
	const float slack = standin::CollectSlack(g_own.number, command, rx, ry);
	if (!g_client || !g_own.running || g_own.finishing || !IsMissionScript(script) || slack < 0.0f)
		return r;
	const uint8_t yes = CompareFlagIfTrue(condBefore, andOr, notFlag);
	if (At<uint8_t>(script, layout::SCRIPT_COND_RESULT) == yes)
		return r;
	for (uint8_t id = 0; id < MAX_PLAYERS; ++id) {
		if (id == LocalId() || !InOwnMission(id))
			continue;
		const RemotePlayer &p = g_client->PlayerSlot(id);
		if (!p.active || !p.haveState ||
		    !standin::OnTheObject(p.last.pos.x - x, p.last.pos.y - y, p.last.pos.z - z, rx, ry, rz, in3d,
		                          slack))
			continue;
		At<uint8_t>(script, layout::SCRIPT_COND_RESULT) = yes;
		if (!g_saidEvidence) {
			g_saidEvidence = true;
			Log("missions: %s picks up one of %s's %s", p.nick.c_str(), MissionName(g_own.number),
			    g_own.number == standin::SAM ? "packages" : "files");
		}
		break;
	}
	return r;
}

// IS_PLAYER_SHOOTING right after Deal Steal's rendezvous locate found a
// participant giving the deal away: true, alone or in its OR group.
int8_t DealShooting(void *script, int32_t command, RangeFn original) {
	const uint16_t andOr = At<uint16_t>(script, layout::SCRIPT_AND_OR);
	const bool     notOn = At<uint8_t>(script, layout::SCRIPT_NOT) != 0;
	const int8_t   r     = original(script, nullptr, command);
	if (!g_client || !g_own.running || g_own.number != stealth::DEAL_STEAL || !IsMissionScript(script))
		return r;
	if (!g_dealBlown || g_frame - g_dealBlownFrame > 1 || notOn || !stealth::MayForceTrueUnder(andOr))
		return r;
	g_dealBlown                                     = false;
	At<uint8_t>(script, layout::SCRIPT_COND_RESULT) = 1;
	return r;
}

// ---- the coronas the mission draws every frame (game/effectshape.h) ----------------

// DRAW_CORONA, DRAW_LIGHT or DRAW_SHADOW in the owner's mission: drawn here as
// the engine draws it, and noted, by where in the script it is drawn, for
// everybody to be told.
int8_t Corona(void *script, int32_t command, RangeFn original) {
	const size_t count = shape::FrameDrawOperands(static_cast<uint16_t>(command));
	if (g_client && g_own.running && IsMissionScript(script) && count != 0) {
		const uint32_t start = At<uint32_t>(script, layout::SCRIPT_IP);
		uint32_t       ip    = start;
		int32_t        values[shape::FRAME_DRAW_MAX];
		bool           read  = true;
		for (size_t i = 0; i < count; ++i) {
			uint32_t bits = 0, len = 0;
			if (!replay::ReadValue(Space(), SCRIPT_SPACE_SIZE, ip, Locals(script), &bits, &len)) {
				read = false;
				break;
			}
			values[i] = static_cast<int32_t>(bits);
			ip += len;
		}
		shape::CoronaDraw c;
		if (read && shape::FrameDrawFrom(static_cast<uint16_t>(command), values, count, &c) &&
		    !g_ownCoronas.Drawn(start, c, WallClock::NowMs()) && !g_saidCoronasFull) {
			g_saidCoronasFull = true;
			Log("missions: %s draws more than %u coronas, lights and shadows at once; the rest "
			    "stay on this screen",
			    MissionName(g_own.number), static_cast<unsigned>(shape::MAX_CORONAS));
		}
	}
	return original(script, nullptr, command);
}

const char *FrameDrawName(const shape::CoronaDraw &c) {
	return c.opcode == shape::DRAW_LIGHT ? "light" : c.opcode == shape::DRAW_SHADOW ? "shadow" : "corona";
}

// The owner's: what its mission's coronas did, to everybody. A corona the
// script puts on the ground wherever that is goes where the ground is here.
void TellCoronas(uint32_t nowMs) {
	g_ownCoronas.Tick(nowMs, [&](uint32_t id, const shape::CoronaDraw &c, bool up, bool first) {
		shape::CoronaDraw d = c;
		if (up && d.z <= SCRIPT_Z_FIND_GROUND)
			d.z = Func<world::FindGroundZFn>(world::CWorld__FindGroundZForCoord)(d.x, d.y);
		g_client->Missions().SendEffect(shape::CoronaEffect(g_own.number, id, d, up), nowMs);
		if (first || !up)
			Log("missions: %s %s a %s at (%.1f, %.1f, %.1f)%s", MissionName(g_own.number),
			    up ? "lit" : "put out", FrameDrawName(d), d.x, d.y, d.z, up ? "; everybody draws it" : "");
	});
}

// A participant's: the owner's machine says one went up or came down.
void HeardCorona(uint32_t id, const shape::CoronaDraw &c, bool up) {
	const bool was = g_shownCoronas.Has(id);
	if (!g_shownCoronas.Heard(id, c, up, WallClock::NowMs())) {
		if (!g_saidCoronasFull) {
			g_saidCoronasFull = true;
			Log("missions: the owner's mission has more than %u coronas lit; the rest are not drawn "
			    "here", static_cast<unsigned>(shape::MAX_CORONAS));
		}
		return;
	}
	if (was != up)
		Log("missions: the owner's %s at (%.1f, %.1f, %.1f) is %s here", FrameDrawName(c), c.x, c.y, c.z,
		    up ? "lit" : "out");
}

// ---- the power pills (mission-audit.md R7, replay.h PillCode) ----------------------

uint8_t *PillSlot(size_t slot) { return Ptr<uint8_t>(PM_PICKUPS + slot * PM_PICKUP_SIZE); }

replay::PillRow ReadOwnPill(size_t slot, bool active) {
	replay::PillRow row;
	const uint8_t  *p = PillSlot(slot);
	const uint8_t   t = p[PM_PICKUP_TYPE];
	if (!active || (t != replay::PILL_SCRAMBLE && t != replay::PILL_RACE))
		return row;
	row.type = t;
	std::memcpy(&row.x, p + 0, 4);
	std::memcpy(&row.y, p + 4, 4);
	std::memcpy(&row.z, p + 8, 4);
	return row;
}

void ForgetSentPills() {
	for (replay::PillRow &r : g_sentPills)
		r = replay::PillRow{};
}

// The owner's: each slot of its table that changed since it last said, a
// few a frame.
void TellPills(uint32_t nowMs) {
	const bool active = Global<uint8_t>(PM_ACTIVE) != 0;
	size_t     told   = 0;
	for (size_t i = 0; i < replay::PILL_SLOTS && told < replay::PILLS_PER_FRAME; ++i) {
		const replay::PillRow now = ReadOwnPill(i, active);
		if (!replay::PillChanged(now, g_sentPills[i]))
			continue;
		MissionEffectBody e{};
		e.missionNumber = g_own.number;
		e.kind          = MISSION_EFFECT_RUN;
		e.handleAt      = 0xFF;
		e.ownerBlip     = static_cast<int32_t>(i);
		const size_t n  = replay::PillCode(e.code, sizeof e.code, static_cast<uint16_t>(i), now);
		if (n == 0)
			return;
		e.length = static_cast<uint8_t>(n);
		g_client->Missions().SendEffect(e, nowMs);
		static bool said = false;
		if (!said && now.type != replay::PILL_NONE) {
			said = true;
			Log("missions: %s's %s are on everybody's screen; only our car collects them",
			    MissionName(g_own.number), now.type == replay::PILL_RACE ? "pills" : "gold");
		}
		g_sentPills[i] = now;
		++told;
	}
}

// A participant's: the owner's slot in ours, to be drawn by our own
// CPacManPickups::Render and never collected (HookedPillUpdate). A slot our
// own engine has an object in is its own, and left alone.
void ShowPill(uint16_t slot, const replay::PillRow &row) {
	if (!g_pillCallRedirected)
		return;   // our car would eat them (InstallPillCall said so)
	uint8_t *const p = PillSlot(slot);
	if (Field<void *>(p, PM_PICKUP_OBJECT) != nullptr) {
		static bool said = false;
		if (!said) {
			said = true;
			Log("missions: the owner's power pill %u is in a slot our own engine uses; not drawn here",
			    static_cast<unsigned>(slot));
		}
		return;
	}
	if (row.type == replay::PILL_NONE) {
		p[PM_PICKUP_TYPE] = replay::PILL_NONE;
		return;
	}
	std::memcpy(p + 0, &row.x, 4);
	std::memcpy(p + 4, &row.y, 4);
	std::memcpy(p + 8, &row.z, 4);
	p[PM_PICKUP_TYPE] = row.type;
	Global<uint8_t>(PM_ACTIVE) = 1;
	if (!g_shown.pills)
		Log("missions: the owner's power pills are drawn here; only the owner's car collects them");
	g_shown.pills = true;
}

// CPacManPickups::Update's call to CPacManPickup::Update, one pill at a time:
// on a participant drawing the owner's, nothing, so our car never eats a pill
// the owner's script is counting.
void __fastcall HookedPillUpdate(void *pill, void * /*edx*/) {
	if (g_shown.pills && !g_own.running)
		return;
	Func<void(__thiscall *)(void *)>(PM_PICKUP_UPDATE)(pill);
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

void InstallPillCall() {
	if (g_pillCallRedirected)
		return;
	g_pillCallRedirected = RedirectRelCall(PM_PICKUP_UPDATE_CALL, PM_PICKUP_UPDATE,
	                                       reinterpret_cast<uintptr_t>(&HookedPillUpdate));
	if (g_pillCallRedirected)
		Log("missions: CPacManPickups::Update's call to CPacManPickup::Update at 0x%08X comes to us; "
		    "the owner's power pills can be drawn here without being eaten",
		    static_cast<unsigned>(PM_PICKUP_UPDATE_CALL));
	else
		Log("missions: the call at 0x%08X is not CPacManPickup::Update's; the owner's power pills "
		    "stay on the owner's screen",
		    static_cast<unsigned>(PM_PICKUP_UPDATE_CALL));
}

void RemovePillCall() {
	if (!g_pillCallRedirected)
		return;
	RedirectRelCall(PM_PICKUP_UPDATE_CALL, reinterpret_cast<uintptr_t>(&HookedPillUpdate),
	                PM_PICKUP_UPDATE);
	g_pillCallRedirected = false;
}

// ---- the detours --------------------------------------------------------------

enum Range : size_t {
	R0, R100, R200, R300, R400, R500, R600, R700, R800, R900, R1000, R1100, RANGE_COUNT
};

struct RangeHook {
	const char *name;
	uintptr_t   target;
	bool        required;   // without it the session's mission cannot work at all
	Detour      detour;
};

RangeHook g_hooks[RANGE_COUNT] = {
    {"ProcessCommands0To99", RANGE_0, false, {}},
    {"ProcessCommands100To199", RANGE_100, false, {}},
    {"ProcessCommands200To299", RANGE_200, true, {}},
    {"ProcessCommands300To399", RANGE_300, false, {}},
    {"ProcessCommands400To499", RANGE_400, false, {}},
    {"ProcessCommands500To599", RANGE_500, false, {}},
    {"ProcessCommands600To699", RANGE_600, false, {}},
    {"ProcessCommands700To799", RANGE_700, false, {}},
    {"ProcessCommands800To899", RANGE_800, false, {}},
    {"ProcessCommands900To999", RANGE_900, false, {}},
    {"ProcessCommands1000To1099", RANGE_1000, true, {}},
    {"ProcessCommands1100To1199", RANGE_1100, false, {}},
};

RangeFn Original(Range range) { return g_hooks[range].detour.Original<RangeFn>(); }

// Whatever else is on the replay list, and everything that is not.
int8_t Pass(Range range, void *s, int32_t c) {
	if (replay::Listed(c))
		return Record(s, c, Original(range));
	return Original(range)(s, nullptr, c);
}

// CLEAR_AREA. From the owner's mission: the session cars inside it that the
// clear takes (mission.h, ClearTakesCar) are emptied of players and taken
// away on every machine first, and then the engine clears the rest with every
// session car locked. From anybody else's script, as it was.
int8_t ClearArea(void *script, int32_t command) {
	if (!g_client || !g_own.running || g_own.finishing || !IsMissionScript(script))
		return Pass(R900, script, command);
	PeekParams(script, 5);
	const float x = Params()[0], y = Params()[1], radius = Params()[3];
	void *const me = Func<PlayerFn>(FindPlayerPed)();
	TakenCar    cars[MAX_TAKEN_CARS];
	size_t      count = 0;
	ForEachVehicle([&](void *car) {
		const Vec3 at = EntityPos(car);
		if (count == MAX_TAKEN_CARS || !InClearCircle(at.x, at.y, x, y, radius))
			return;
		const int32_t handle = CarHandle(car);
		ClearCarFacts f;
		const uint16_t netId = g_client->SessionCarNetIdOf(handle);
		f.sessionCar         = netId != INVALID_NETID;
		f.missionCar         = IsMissionCar(handle);
		f.wrecked            = Wrecked(car);
		f.ownerInside        = me && Field<uint8_t>(me, offs::PED_IN_VEHICLE) != 0 &&
		                Field<void *>(me, offs::PED_MY_VEHICLE) == car;
		f.missionPed = f.sessionCar && MissionPedIn(car);
		// Two-Faced Tanner's clears take in the checkpoint everybody stopped
		// at, and the chase starts a second after the second one (standin.h).
		if (f.sessionCar && standin::ClearSparesRiders(g_own.number) && OtherPlayerIn(car, netId))
			return;
		if (ClearTakesCar(f) && !Spared(netId))
			cars[count++] = TakenCar{handle, netId};
	});
	if (count != 0) {
		if (HoldForPlayersIn(script, cars, count, true))
			return 1;
		TakeFreeCars(cars, count);
	}
	const SessionCarsLocked locked(true);
	return Pass(R900, script, command);
}

// DELETE_CAR from the owner's mission on a session car: everybody out of it
// first, and then it goes from every machine, not from the owner's alone to be
// put back by the session (Chaperone's Stretch, Salvatore's limo, Toni's car
// in Cipriani's Chauffeur). The owner's own player in it is the script's
// business, as ever.
int8_t MissionDeletesCar(void *script, int32_t command, RangeFn original) {
	if (!g_client || !g_own.running || !IsMissionScript(script))
		return CarLetGo(script, command, original);
	PeekParams(script, 1);
	const int32_t  handle = *reinterpret_cast<const int32_t *>(Params());
	void *const    car    = VehicleAt(handle);
	const uint16_t netId  = car ? g_client->SessionCarNetIdOf(handle) : INVALID_NETID;
	void *const    me     = Func<PlayerFn>(FindPlayerPed)();
	if (!car || netId == INVALID_NETID || Wrecked(car) || Spared(netId) ||
	    (me && Field<uint8_t>(me, offs::PED_IN_VEHICLE) != 0 &&
	     Field<void *>(me, offs::PED_MY_VEHICLE) == car))
		return CarLetGo(script, command, original);
	const TakenCar taken{handle, netId};
	if (HoldForPlayersIn(script, &taken, 1, !g_own.finishing))
		return 1;
	TakeFreeCars(&taken, 1);
	// What is left of the instruction: the mission's own list of what it made.
	// The car is gone already, unless a player is still in it, and then the
	// engine deletes this copy as it always did.
	return CarLetGo(script, command, original);
}

// $ONMISSION is the session's, never a campaign value, whichever of its two
// witnesses names it.
bool IsOnMissionGlobal(uint32_t at) {
	static uint32_t declared = 0;
	static bool     scanned  = false;
	if (!scanned && GlobalsEnd(Space(), MAIN_SCRIPT_SIZE) != 0) {
		declared = DeclaredMissionFlag();
		scanned  = true;
	}
	OnMissionVar();
	return at == g_onMissionAt || (declared != 0 && at == declared);
}

// One of main.scm's own globals, and not $ONMISSION, which is the session's.
bool CampaignGlobal(uint16_t at) {
	const uint32_t end = GlobalsEnd(Space(), MAIN_SCRIPT_SIZE);
	return at >= 8 && at + 4u <= end && !IsOnMissionGlobal(at);
}

void NoteWrite(uint16_t at, bool handle, int from) {
	if (!CampaignGlobal(at))
		return;
	const int32_t before = At<int32_t>(Space(), at);
	const bool    kept   = from >= 0 ? g_writes.NoteCopy(at, before, static_cast<uint16_t>(from))
	                                 : g_writes.Note(at, before, handle);
	if (!kept && !g_saidWritesFull) {
		g_saidWritesFull = true;
		Log("missions: %s writes more than %u globals; the rest are left out of what it leaves "
		    "behind", MissionName(g_own.number), static_cast<unsigned>(MAX_TRACKED));
	}
}

// A global the owner's mission assigns, remembered for its delta with what it
// held before. The first operand is where the result goes; a copy's second
// is where it comes from (mission.h, AssignmentOf).
void TrackAssignment(void *script, int32_t command) {
	const uint32_t ip = At<uint32_t>(script, layout::SCRIPT_IP);
	const uint8_t *s  = Space();
	if (ip + 3 > SCRIPT_SPACE_SIZE || s[ip] != PARAM_GLOBAL)
		return;
	const uint16_t at = static_cast<uint16_t>(s[ip + 1] | (s[ip + 2] << 8));
	if (AssignmentOf(command) == Assignment::Copy) {
		// Anything but a global to copy from is not what 0084 is.
		const int from = ip + 6 <= SCRIPT_SPACE_SIZE && s[ip + 3] == PARAM_GLOBAL
		                     ? s[ip + 4] | (s[ip + 5] << 8)
		                     : 0;
		NoteWrite(at, true, from > 0 ? from : -1);
		return;
	}
	NoteWrite(at, false);
}

// A main-script thread the owner's mission starts: the next mission's
// trigger, usually. The new script is the head of the active list once the
// engine has started it.
int8_t StartThread(void *script, int32_t command, RangeFn original) {
	const bool mission = g_own.running && IsMissionScript(script);
	int32_t    label   = -1;
	if (mission) {
		const uint32_t ip = At<uint32_t>(script, layout::SCRIPT_IP);
		const uint8_t *s  = Space();
		if (ip + 5 <= SCRIPT_SPACE_SIZE && s[ip] == PARAM_INT32)
			std::memcpy(&label, s + ip + 1, 4);
	}
	const int8_t r = original(script, nullptr, command);
	if (mission && IsMainThreadLabel(label) && g_startedCount < CAMPAIGN_THREADS)
		g_started[g_startedCount++] = {label, Global<uintptr_t>(ACTIVE_SCRIPTS)};
	return r;
}

int8_t __fastcall Hooked0(void *s, void *, int32_t c) {
	const MissionInstruction scope(s, c);
	if (c == op::TERMINATE_THIS_SCRIPT)
		return Terminate(s, c, Original(R0));
	if (c == op::IS_PLAYER_IN_AREA_2D || c == op::IS_PLAYER_IN_AREA_3D)
		return Location(s, c, Original(R0));
	if (c == 0x004F)
		return StartThread(s, c, Original(R0));
	if (c == OP_GOTO_IF_FALSE && g_heldRespray.script == s)
		SettleRespray(s);
	if (c == fuzzball::OP_WAIT && InFuzzBall(s))
		ForgetFuzzSubject();
	if (IsTrackedAssignment(c) && g_own.running && IsMissionScript(s))
		TrackAssignment(s, c);
	return Pass(R0, s, c);
}

int8_t __fastcall Hooked100(void *s, void *, int32_t c) {
	const MissionInstruction scope(s, c);
	if (c == op::DELETE_CHAR || c == mcombat::OP_SET_CHAR_OBJ_NO_OBJ)
		MissionLetsGoOfEnemy(s, false);
	if (c == mcombat::OP_SET_CAR_MISSION || c == mcombat::OP_CAR_SET_IDLE)
		return RetargetRam(s, c, Original(R100));
	if (c == op::DELETE_CHAR)
		return LetGoOfChar(s, c, Original(R100));
	if (c == op::CREATE_CAR)
		return CarMade(s, c, Original(R100));
	if (c == op::DELETE_CAR)
		return MissionDeletesCar(s, c, Original(R100));
	if (IsTrackedAssignment(c) && g_own.running && IsMissionScript(s))
		TrackAssignment(s, c);
	return Pass(R100, s, c);
}

int8_t __fastcall Hooked200(void *s, void *, int32_t c) {
	const MissionInstruction scope(s, c);
	if (c == OP_ANDOR && g_locationScript == s)
		g_locationScript = nullptr;   // a new block: its own conditions only
	if (c == OP_ANDOR && g_inCarScript == s)
		g_inCarScript = nullptr;
	if (c == OP_ANDOR && g_heldRespray.script == s)
		g_heldRespray = HeldRespray{};   // a block with no goto_if_false: nothing spent
	if (c == OP_ANDOR && g_wakaPrev.script == s)
		g_wakaPrev = WakaPrev{};
	if (c == op::MISSION_HAS_FINISHED)
		return Finished(s, c, Original(R200));
	if (InGetaway(s)) {
		int8_t answered = 0;
		if (GetawayCommand(s, c, Original(R200), &answered))
			return answered;
	}
	if ((c == OP_LOCATE_PLAYER_ANY_MEANS_2D || c == OP_LOCATE_PLAYER_ANY_MEANS_3D) && g_own.running &&
	    (g_own.number == standin::EVIDENCE_DASH || g_own.number == standin::SAM) && IsMissionScript(s))
		return EvidenceLocate(s, c, Original(R200));
	if (c == OP_LOCATE_PLAYER_ANY_MEANS_2D)
		return StealthLocate(s, c, Original(R200));
	if (IsLocationCondition(c))
		return Location(s, c, Original(R200));
	if (c == anyplace::OP_IS_PLAYER_IN_ZONE)
		return PlayerInZone(s, c, Original(R200));
	if (c == op::HAS_CHAR_SPOTTED_PLAYER)
		return Spotted(s, c, Original(R200));
	standin::CharLocate charLocate;
	if (standin::CharLocateOf(c, &charLocate) && g_own.running &&
	    standin::EscortMission(g_own.number) && IsMissionScript(s))
		return EscortLocate(s, c, charLocate, Original(R200));
	if (c == op::LOCATE_PLAYER_ANY_MEANS_CHAR_2D || c == op::LOCATE_PLAYER_ANY_MEANS_CHAR_3D)
		return LocateNearChar(s, c, Original(R200));
	if (c == op::IS_PLAYER_IN_MODEL)
		return InModel(s, c, Original(R200));
	if (c == op::IS_PLAYER_IN_CAR)
		return PlayerInCar(s, c, Original(R200));
	if (c == fuzzball::OP_IS_PLAYER_IN_ANY_CAR && InWaka(s))
		NoteWakaCondition(s, c);
	if (InRoundUp(s)) {
		if (c == fuzzball::OP_IS_PLAYER_IN_ANY_CAR)
			return RoundUpInAnyCar(s, c, Original(R200));
		if (c == fuzzball::OP_STORE_CAR_PLAYER_IS_IN)
			return RoundUpStoreCar(s, c, Original(R200));
	}
	if (InFuzzBall(s)) {
		if (c == fuzzball::OP_LOCATE_PLAYER_ON_FOOT_CHAR_3D)
			ForgetFuzzSubject();
		if (c == fuzzball::OP_LOCATE_PLAYER_IN_CAR_CHAR_3D)
			return FuzzLocateInCar(s, c, Original(R200));
		if (c == fuzzball::OP_IS_PLAYER_IN_ANY_CAR)
			return FuzzSubjectCondition(s, c, Original(R200));
		if (c == fuzzball::OP_STORE_CAR_PLAYER_IS_IN)
			return FuzzStoreCar(s, c, Original(R200));
	}
	return Pass(R200, s, c);
}

int8_t __fastcall Hooked300(void *s, void *, int32_t c) {
	const MissionInstruction scope(s, c);
	if (c == op::ADD_BLIP_FOR_COORD || c == op::ADD_BLIP_FOR_COORD_OLD)
		return BlipAdded(s, c, Original(R300));
	if (c == op::REMOVE_BLIP)
		return BlipRemoved(s, c, Original(R300));
	if (c == shape::DRAW_SHADOW)
		return Corona(s, c, Original(R300));
	return Pass(R300, s, c);
}

int8_t __fastcall Hooked400(void *s, void *, int32_t c) {
	const MissionInstruction scope(s, c);
	if (IsLocationCondition(c))
		return Location(s, c, Original(R400));
	if (c == getaway::OP_IS_PLAYER_SITTING_IN_ANY_CAR && InGetaway(s)) {
		int8_t answered = 0;
		if (GetawayCommand(s, c, Original(R400), &answered))
			return answered;
	}
	if (c == op::SET_CHAR_OBJ_KILL_PLAYER_ON_FOOT || c == op::SET_CHAR_OBJ_KILL_PLAYER_ANY_MEANS)
		return RetargetKill(s, c, Original(R400));
	if (c == mcombat::OP_SET_CHAR_OBJ_DESTROY_CAR)
		return RetargetDestroy(s, c, Original(R400));
	if (c == op::MARK_CHAR_AS_NO_LONGER_NEEDED || c == mcombat::OP_MARK_CAR_AS_NO_LONGER_NEEDED)
		MissionLetsGoOfEnemy(s, c == mcombat::OP_MARK_CAR_AS_NO_LONGER_NEEDED);
	if (c == op::MARK_CHAR_AS_NO_LONGER_NEEDED)
		return LetGoOfChar(s, c, Original(R400));
	if (c == op::DONT_REMOVE_CAR)
		return CarLetGo(s, c, Original(R400));
	if (c == op::SET_CHAR_OBJ_ENTER_CAR_AS_DRIVER)
		return ScriptedWheel(s, c, Original(R400));
	if (c == op::SET_CHAR_OBJ_ENTER_CAR_AS_PASSENGER)
		return PassengerOrder(s, c, Original(R400));
	if (c == op::SET_PLAYER_AS_LEADER && InGetaway(s))
		return GetawayLeader(s, c, Original(R400));
	if (c == op::SET_PLAYER_AS_LEADER || c == op::CLEAR_LEADER)
		return LeaderOrder(s, c, Original(R400));
	if (c == op::GET_NUMBER_OF_PASSENGERS)
		return PassengersBesidePlayers(s, c, Original(R400));
	if (c == op::SET_CHAR_OBJ_LEAVE_CAR || c == op::SET_PLAYER_CONTROL)
		NoteWalkOut(s, c);
	if (c == fuzzball::OP_SET_PLAYER_AS_LEADER && InFuzzBall(s))
		return FuzzLeader(s, c, Original(R400));
	return Pass(R400, s, c);
}

int8_t __fastcall Hooked500(void *s, void *, int32_t c) {
	const MissionInstruction scope(s, c);
	if (c == op::REMOVE_PICKUP)
		return RemovePickup(s, c, Original(R500));
	if (c == op::IS_CAR_IN_MISSION_GARAGE)
		return GarageCondition(s, c, Original(R500));
	if (nearchar::CarLocate carLocate; nearchar::CarLocateOf(c, &carLocate))
		return LocateNearCar(s, c, Original(R500));
	if (c == op::HAS_MODEL_LOADED || c == op::HAS_SPECIAL_CHARACTER_LOADED)
		return ModelsLoaded(s, c, Original(R500));
	if (c == world::op::DRAW_CORONA || c == shape::DRAW_LIGHT)
		return Corona(s, c, Original(R500));
	return Pass(R500, s, c);
}

int8_t __fastcall Hooked600(void *s, void *, int32_t c) {
	const MissionInstruction scope(s, c);
	if (c == op::ADD_SPRITE_BLIP_FOR_COORD)
		return BlipAdded(s, c, Original(R600));
	if (c == fuzzball::OP_IS_PLAYER_STOPPED && InFuzzBall(s))
		return FuzzSubjectCondition(s, c, Original(R600));
	return Pass(R600, s, c);
}

int8_t __fastcall Hooked700(void *s, void *, int32_t c) {
	const MissionInstruction scope(s, c);
	if (c == op::REGISTER_MISSION_PASSED)
		return Passed(s, c, Original(R700));
	if (c == op::IS_PLAYER_SHOOTING_IN_AREA)
		return ShootingInArea(s, c, Original(R700));
	if (c == OP_IS_PLAYER_SHOOTING)
		return DealShooting(s, c, Original(R700));
	return Pass(R700, s, c);
}

int8_t __fastcall Hooked800(void *s, void *, int32_t c) {
	const MissionInstruction scope(s, c);
	if (c == op::HAS_RESPRAY_HAPPENED)
		return GarageCondition(s, c, Original(R800));
	if (c == op::HAS_DRUG_PLANE_BEEN_SHOT_DOWN || c == op::HAS_DROP_OFF_PLANE_BEEN_SHOT_DOWN)
		return PlaneCondition(s, c, Original(R800));
	if (c == op::UNDRESS_CHAR)
		return Undress(s, c, Original(R800));
	// main.scm's own gates and Staunton's safehouse door, whose two halves
	// save.sc slides (game/gates.h), before anything records it.
	if (c == 0x034E) {
		int8_t r = 0;
		if (GateSlide(s, &r))
			return r;
	}
	// And the Portland and Shoreside safehouse doors, which save.sc turns.
	if (c == 0x034D) {
		int8_t r = 0;
		if (GateRotate(s, &r))
			return r;
	}
	if (c == op::WARP_PLAYER_INTO_CAR || c == op::WARP_CHAR_INTO_CAR)
		return ScriptedWheel(s, c, Original(R800));
	if (c == fuzzball::OP_IS_CHAR_IN_PLAYERS_GROUP && (InFuzzBall(s) || InGetaway(s)))
		return FuzzInGroup(s, c, Original(R800));
	return Pass(R800, s, c);
}

int8_t __fastcall Hooked900(void *s, void *, int32_t c) {
	const MissionInstruction scope(s, c);
	if (c == world::op::CLEAR_AREA)
		return ClearArea(s, c);
	if (c == OP_HAS_CATALINA_HELI_BEEN_SHOT_DOWN)
		return PlaneCondition(s, c, Original(R900));
	return Pass(R900, s, c);
}

int8_t __fastcall Hooked1000(void *s, void *, int32_t c) {
	const MissionInstruction scope(s, c);
	if (c == op::CAN_PLAYER_START_MISSION)
		return StartGate(s, c, Original(R1000));
	if (c == OP_IS_PLAYER_SITTING_IN_CAR)
		return SittingInCar(s, c, Original(R1000));
	if (c == op::LOAD_AND_LAUNCH_MISSION_INTERNAL)
		return Launch(s, c, Original(R1000));
	return Pass(R1000, s, c);
}

// 1100..1154: nothing here is intercepted, but Last Requests' and S.A.M.'s
// island loads, Lips' car and Bait's cartel car and The Exchange's music are
// on the replay list, and without this range they never reached anybody.
int8_t __fastcall Hooked1100(void *s, void *, int32_t c) {
	const MissionInstruction scope(s, c);
	return Pass(R1100, s, c);
}

void *const kReplacements[RANGE_COUNT] = {
    reinterpret_cast<void *>(&Hooked0),   reinterpret_cast<void *>(&Hooked100),
    reinterpret_cast<void *>(&Hooked200), reinterpret_cast<void *>(&Hooked300),
    reinterpret_cast<void *>(&Hooked400), reinterpret_cast<void *>(&Hooked500),
    reinterpret_cast<void *>(&Hooked600), reinterpret_cast<void *>(&Hooked700),
    reinterpret_cast<void *>(&Hooked800), reinterpret_cast<void *>(&Hooked900),
    reinterpret_cast<void *>(&Hooked1000), reinterpret_cast<void *>(&Hooked1100),
};

// A range handler opens by rebasing the opcode and jumping through a table:
// `jmp dword [reg*4 + table]` (FF 24, then a SIB of scale 4 with no base)
// within its first few instructions (addresses.h, the note above
// CRunningScript__ProcessCommands500To599).
bool LooksLikeRangeHandler(uintptr_t at) {
	const uint8_t *code = Ptr<uint8_t>(at);
	for (size_t i = 0; i + 2 < 64; ++i)
		if (code[i] == 0xFF && code[i + 1] == 0x24 && (code[i + 2] & 0xC7) == 0x85)
			return true;
	return false;
}

// Whether the dispatcher calls `at`: one of the `E8 rel32` in its body lands
// on it (missionaddr.h, DISPATCHER_BYTES). III.CLEO's jump over the
// dispatcher's first five bytes leaves the calls where they were.
bool DispatcherCalls(uintptr_t at) {
	const uint8_t *code = Ptr<uint8_t>(CRunningScript__ProcessCommands);
	for (uint32_t i = 0; i + 5 <= DISPATCHER_BYTES; ++i) {
		if (code[i] != 0xE8)
			continue;
		int32_t rel = 0;
		std::memcpy(&rel, code + i + 1, 4);
		if (CRunningScript__ProcessCommands + i + 5 + static_cast<uint32_t>(rel) == at)
			return true;
	}
	return false;
}

// DECLARE_MISSION_FLAG's operand in main.scm, the one place $ONMISSION is
// named: 0180 then a global.
uint32_t DeclaredMissionFlag() {
	const uint8_t *s = Space();
	for (uint32_t at = 0; at + 5 < MAIN_SCRIPT_SIZE; ++at)
		if (s[at] == 0x80 && s[at + 1] == 0x01 && s[at + 2] == PARAM_GLOBAL)
			return static_cast<uint32_t>(s[at + 3] | (s[at + 4] << 8));
	return 0;
}

// ---- running the owner's instructions here ----------------------------------------
//
// plugin-sdk's CallCommandById, for GTA III: a script of ours whose
// instruction pointer is our own buffer's distance from the script space, and
// the dispatcher run once over it. The engine then does exactly what it does
// for its own scripts. Longer than the engine's 0x88 because III.CLEO's
// scripts are 0xB0 and its dispatcher may look past the engine's part.

alignas(16) uint8_t g_runner[0x100];
uint8_t             g_runnerCode[MISSION_EFFECT_CODE + 16];

bool RunHere(const uint8_t *code, size_t length, int32_t *local0) {
	if (length < 2 || length > MISSION_EFFECT_CODE)
		return false;
	std::memset(g_runner, 0, sizeof g_runner);
	std::memcpy(g_runner + layout::SCRIPT_NAME, "coopiii", 8);
	std::memset(g_runnerCode, 0, sizeof g_runnerCode);
	std::memcpy(g_runnerCode, code, length);
	At<uint32_t>(g_runner, layout::SCRIPT_IP) =
	    static_cast<uint32_t>(reinterpret_cast<uintptr_t>(g_runnerCode) - SCRIPT_SPACE);
	Func<OneFn>(CRunningScript__ProcessCommands)(g_runner);
	if (local0)
		*local0 = At<int32_t>(g_runner, layout::SCRIPT_LOCALS);
	return true;
}

// An instruction that names one of this machine's own blips by the global it
// is kept in (replay.h, BlipGlobalOf), made ready to run here. False when it
// must not be: a global past main.scm's, or one holding what no blip handle
// looks like - GetActualBlipArrayIndex never checks the low half against the
// table (addresses.h), so a stray value would be a write past it. When the
// instruction makes a contact's marker, the one the global holds is taken off
// first, as the script itself always does before it makes a new one: so the
// same marker, run live and again from the campaign, is one marker, never a
// second one left on the radar with nothing to take it off.
bool ReadyBlipGlobal(const uint8_t *code, size_t length) {
	uint16_t global = 0;
	bool     makes  = false;
	if (!replay::BlipGlobalOf(code, length, &global, &makes))
		return true;   // names none
	const uint32_t end = GlobalsEnd(Space(), MAIN_SCRIPT_SIZE);
	if (global < 8 || global + 4u > end)
		return false;
	const int32_t held = At<int32_t>(Space(), global);
	if (held != -1 && static_cast<uint32_t>(held & 0xFFFF) >= NUM_RADAR_BLIPS)
		return false;
	if (makes && held != -1) {
		const uint8_t remove[5] = {static_cast<uint8_t>(op::REMOVE_BLIP & 0xFF),
		                           static_cast<uint8_t>(op::REMOVE_BLIP >> 8), PARAM_GLOBAL,
		                           static_cast<uint8_t>(global & 0xFF), static_cast<uint8_t>(global >> 8)};
		RunHere(remove, sizeof remove, nullptr);
	}
	return true;
}

// The same for a pickup made into, or taken away by, this machine's own
// global (replay.h, PickupGlobalOf): a shop's gun, Phil's armour, the
// out-of-stock sign. Making one takes away whatever the global still holds
// first, so a pickup run live and again from the campaign is one pickup.
// Taking one away that the global does not hold is not run at all: the
// handler reads the pickup table at whatever slot the global names, and
// checks the generation, never the slot (0x00433DF0).
bool ReadyPickupGlobal(const uint8_t *code, size_t length) {
	uint16_t global = 0;
	bool     makes  = false;
	if (!replay::PickupGlobalOf(code, length, &global, &makes))
		return true;   // names none
	const uint32_t end = GlobalsEnd(Space(), MAIN_SCRIPT_SIZE);
	if (global < 8 || global + 4u > end)
		return false;
	const int32_t held = At<int32_t>(Space(), global);
	if (!PickupInUse(held))
		return makes;
	if (makes)
		RunOurs(op::REMOVE_PICKUP, {held});
	return true;
}

// ---- the bridge -------------------------------------------------------------------

void SetOnMission(bool on) {
	g_mirror = on;
	int32_t *flag = OnMissionVar();
	if (!flag)
		return;
	if (on)
		*flag = 1;
	else if (Global<uint8_t>(ALREADY_RUNNING_A_MISSION) == 0)
		*flag = 0;
}

// True once the owner's mission has been failed, or there is nothing left to
// fail; false while it is to be tried again.
bool TryFailMission(uint8_t reason, bool firstTry) {
	// Passed or failed already, and in its cleanup: unwound now it would go
	// through its failure a second time, MISSION FAILED and all, whatever it
	// ended as.
	if (g_own.finishing || !g_own.running) {
		if (firstTry)
			Log("missions: told to fail %s, which is over already and cleaning up",
			    MissionName(g_own.number));
		return true;
	}
	for (uintptr_t s = Global<uintptr_t>(ACTIVE_SCRIPTS); s != 0;
	     s = *reinterpret_cast<uintptr_t *>(s + layout::SCRIPT_NEXT)) {
		if (!IsMissionScript(reinterpret_cast<void *>(s)))
			continue;
		uint8_t *const script  = reinterpret_cast<uint8_t *>(s);
		const bool     checked = script[layout::SCRIPT_DEATHARREST_ARMED] != 0;
		// A script with its check off (TAXI, HOOD1) watches its own player and
		// would never see ours: it goes to its cleanup all the same.
		if (UnwindForDeatharrest(script, OnMissionVar(), true)) {
			Log("missions: %s fails here, the way it does for our own death (%s)%s",
			    MissionName(g_own.number), reason == MISSION_FAIL_BUSTED ? "a bust" : "a death",
			    checked ? "" : "; its script has its death check off, and goes to its cleanup");
			return true;
		}
		if (firstTry)
			Log("missions: %s cannot be failed yet: its script is between its gosubs. Tried "
			    "again every frame until it can be", MissionName(g_own.number));
		return false;
	}
	if (firstTry)
		Log("missions: told to fail a mission, and none is running here");
	return true;
}

void FailMission(uint8_t reason) {
	g_failPending = !TryFailMission(reason, true);
	g_failReason  = reason;
}

void RetryFailMission() {
	if (!g_failPending)
		return;
	if (TryFailMission(g_failReason, false))
		g_failPending = false;
}

// What a replayed instruction did to this machine's screen, for EndEffects to
// put back whatever is still on at the end.
void NoteShown(const replay::Encoded &run) {
	const uint16_t opcode = static_cast<uint16_t>(run.code[0] | (run.code[1] << 8));
	int32_t        v      = 0;
	switch (opcode) {
	case op::LOAD_CUTSCENE:  g_shown.cutscene = true; break;
	case op::START_CUTSCENE:
		g_shown.cutscene = true;
		SetCutsceneScene(true);
		break;
	case op::CLEAR_CUTSCENE:
		g_shown.cutscene = false;
		g_shown.madeSafe = false;   // DeleteCutsceneData gave the controls back
		g_objectMap.Clear();
		SetCutsceneScene(false);
		break;
	case op::SET_PLAYER_CONTROL:
		if (replay::LiteralAt(run.code, run.length, 1, &v))
			g_shown.controlOff = v == 0;
		break;
	case op::SWITCH_WIDESCREEN:
		if (replay::LiteralAt(run.code, run.length, 0, &v))
			g_shown.widescreen = v != 0;
		break;
	case op::FLASH_HUD_OBJECT:
		if (replay::LiteralAt(run.code, run.length, 0, &v) &&
		    (v != HUD_ITEM_NONE) != g_shown.flashing) {
			g_shown.flashing = v != HUD_ITEM_NONE;
			Log("missions: the owner's mission %s here too",
			    v == HUD_ITEM_NONE    ? "stops flashing the HUD"
			    : v == HUD_ITEM_RADAR ? "flashes the radar"
			                          : "flashes a HUD item");
		}
		break;
	case op::DO_FADE:
		if (replay::LiteralAt(run.code, run.length, 1, &v))
			g_shown.fadedOut = v == 0;
		break;
	case op::SET_FIXED_CAMERA_POSITION:
	case op::POINT_CAMERA_AT_POINT:
	case op::CAMERA_ON_PLAYER:
	case op::CAMERA_ON_VEHICLE:
	case op::CAMERA_ON_PED:                 g_shown.fixedCamera = true; break;
	case op::RESTORE_CAMERA:
	case op::RESTORE_CAMERA_JUMPCUT:
	case op::SET_CAMERA_BEHIND_PLAYER:
	case op::SET_CAMERA_IN_FRONT_OF_PLAYER: g_shown.fixedCamera = false; break;
	case op::SET_PLAYER_VISIBLE:
		if (replay::LiteralAt(run.code, run.length, 1, &v))
			g_shown.invisible = v == 0;
		break;
	case op::SET_EVERYONE_IGNORE_PLAYER:
		if (replay::LiteralAt(run.code, run.length, 1, &v))
			g_shown.ignoredByAll = v != 0;
		break;
	case op::SET_POLICE_IGNORE_PLAYER:
		if (replay::LiteralAt(run.code, run.length, 1, &v))
			g_shown.ignoredByCops = v != 0;
		break;
	// SET_PLAYER_NEVER_GETS_TIRED is not put back: the only mission that runs
	// it is the paramedic's, and what it gives is the reward (replay.h).
	case op::APPLY_BRAKES_TO_PLAYERS_CAR:
		if (replay::LiteralAt(run.code, run.length, 1, &v))
			g_shown.brakes = v != 0;
		break;
	case op::SET_FREE_BOMB_SHOP:
		if (replay::LiteralAt(run.code, run.length, 0, &v))
			g_shown.freeBombs = v != 0;
		break;
	case op::START_DRUG_RUN:
		g_planesWatched = static_cast<uint16_t>(g_planesWatched | MISSION_SHOT_DOWN_DRUG_PLANE);
		break;
	case op::START_DRUG_DROP_OFF:
		g_planesWatched = static_cast<uint16_t>(g_planesWatched | MISSION_SHOT_DOWN_DROP_OFF);
		break;
	case OP_START_CATALINA_HELI:
		g_shown.catalina     = true;
		g_shown.catalinaPath = 0;
		g_planesWatched      = static_cast<uint16_t>(g_planesWatched | MISSION_SHOT_DOWN_CATALINA);
		Log("missions: the owner's Catalina helicopter is up; ours flies its path here");
		break;
	case OP_REMOVE_CATALINA_HELI:
		g_shown.catalina     = false;
		g_shown.catalinaPath = 0;
		g_planesWatched      = static_cast<uint16_t>(g_planesWatched & ~MISSION_SHOT_DOWN_CATALINA);
		break;
	case OP_REMOVE_ALL_SCRIPT_FIRES:
		g_fireMap.Clear();   // every one of them is out, ours with them
		break;
	case op::OVERRIDE_NEXT_RESTART:         g_shown.restartMoved = true; break;
	case op::CANCEL_OVERRIDE_RESTART:       g_shown.restartMoved = false; break;
	case op::DISPLAY_ONSCREEN_TIMER:        g_shown.timer = WidgetGlobal(run); break;
	case op::CLEAR_ONSCREEN_TIMER:          g_shown.timer = 0; break;
	case op::DISPLAY_ONSCREEN_COUNTER:
	case op::DISPLAY_ONSCREEN_COUNTER_WITH_STRING: g_shown.counter = WidgetGlobal(run); break;
	case op::CLEAR_ONSCREEN_COUNTER:        g_shown.counter = 0; break;
	case world::op::LOAD_MISSION_AUDIO:     g_shown.missionAudio = true; break;
	case world::op::CLEAR_MISSION_AUDIO:    g_shown.missionAudio = false; break;
	case world::op::START_CREDITS:          g_shown.credits = true; break;
	case world::op::STOP_CREDITS:           g_shown.credits = false; break;
	case op::MAKE_PLAYER_SAFE_FOR_CUTSCENE:
		g_shown.madeSafe   = true;
		g_shown.controlOff = true;
		break;
	case op::SET_FREE_RESPRAYS:
		if (replay::LiteralAt(run.code, run.length, 0, &v))
			g_shown.freeResprays = v != 0;
		break;
	case op::SET_TARGET_CAR_FOR_MISSION_GARAGE: {
		int32_t car = -1;
		if (replay::LiteralAt(run.code, run.length, 0, &v) && v >= 0 &&
		    v < static_cast<int32_t>(NUM_GARAGES) &&
		    replay::LiteralAt(run.code, run.length, 1, &car)) {
			const uint32_t bit  = 1u << v;
			g_shown.garageTargets = car >= 0 ? (g_shown.garageTargets | bit)
			                                 : (g_shown.garageTargets & ~bit);
		}
		break;
	}
	case op::SET_WANTED_MULTIPLIER: {
		float f = 1.0f;
		if (replay::LiteralAt(run.code, run.length, 0, &v)) {
			std::memcpy(&f, &v, 4);
			g_shown.crimeEye = f != 1.0f;
		}
		break;
	}
	case op::OVERRIDE_HOSPITAL_LEVEL:
		if (replay::LiteralAt(run.code, run.length, 0, &v))
			g_shown.hospital = v != game::LEVEL_GENERIC;
		break;
	case op::OVERRIDE_POLICE_STATION_LEVEL:
		if (replay::LiteralAt(run.code, run.length, 0, &v))
			g_shown.policeStation = v != game::LEVEL_GENERIC;
		break;
	case op::SWITCH_WORLD_PROCESSING:
		if (replay::LiteralAt(run.code, run.length, 0, &v))
			g_shown.worldHeld = v == 0;
		break;
	case op::SET_ALL_CARS_CAN_BE_DAMAGED:
		if (replay::LiteralAt(run.code, run.length, 0, &v))
			g_shown.carsUnhurt = v == 0;
		break;
	case op::SET_GENERATE_CARS_AROUND_CAMERA:
		if (replay::LiteralAt(run.code, run.length, 0, &v))
			g_shown.carsAtCamera = v != 0;
		break;
	case op::SET_NEAR_CLIP: {
		float f = NEAR_CLIP_AFTER_MISSION;
		if (replay::LiteralAt(run.code, run.length, 0, &v)) {
			std::memcpy(&f, &v, 4);
			g_shown.nearClip = f != NEAR_CLIP_AFTER_MISSION;
		}
		break;
	}
	case op::SET_MUSIC_DOES_FADE:
		if (replay::LiteralAt(run.code, run.length, 0, &v))
			g_shown.musicNoFade = v == 0;
		break;
	case op::PLAY_END_OF_GAME_TUNE: g_shown.endTune = true; break;
	case op::STOP_END_OF_GAME_TUNE: g_shown.endTune = false; break;
	case op::ADD_CONTINUOUS_SOUND:
	case op::REMOVE_SOUND: {
		uint16_t global = 0;
		bool     makes  = false;
		if (!replay::SoundGlobalOf(run.code, run.length, &global, &makes))
			break;
		for (uint8_t i = 0; i < g_shown.soundCount; ++i)
			if (g_shown.sounds[i] == global) {
				g_shown.sounds[i] = g_shown.sounds[--g_shown.soundCount];
				break;
			}
		if (makes && g_shown.soundCount < sizeof g_shown.sounds / sizeof g_shown.sounds[0])
			g_shown.sounds[g_shown.soundCount++] = global;
		break;
	}
	default: break;
	}
}

// A continuous sound's handle, as this machine's own global holds it, is one
// its pool's GetAt can be given: the handler checks nothing (missionaddr.h,
// AUDIO_SCRIPT_OBJECT_POOL), so a stray value would be read past the flags.
bool SoundHandleInPool(uint16_t global) {
	const uint32_t end = GlobalsEnd(Space(), MAIN_SCRIPT_SIZE);
	if (global < 8 || global + 4u > end)
		return false;
	const int32_t   handle = At<int32_t>(Space(), global);
	const uintptr_t pool   = Global<uintptr_t>(AUDIO_SCRIPT_OBJECT_POOL);
	if (handle < 0 || pool == 0)
		return false;
	const int32_t size = *reinterpret_cast<const int32_t *>(pool + object::POOL_SIZE);
	return (handle >> 8) < size;
}

// Before a replayed instruction changes the streets here: what they were, the
// first time each thing is changed in this mission (WorldBefore).
void NoteWorldBefore(const replay::Encoded &run) {
	const uint16_t opcode = static_cast<uint16_t>(run.code[0] | (run.code[1] << 8));
	switch (opcode) {
	case world::op::SET_PED_DENSITY_MULTIPLIER:
		if (!g_before.pedDensity) {
			g_before.pedDensity = true;
			g_before.ped        = Global<float>(world::PED_DENSITY);
		}
		break;
	case world::op::SET_CAR_DENSITY_MULTIPLIER:
		if (!g_before.carDensity) {
			g_before.carDensity = true;
			g_before.car        = Global<float>(world::CAR_DENSITY);
		}
		break;
	case world::op::SET_ZONE_CAR_INFO:
	case world::op::SET_ZONE_PED_INFO:
		if (!g_before.zones) {
			g_before.zones = true;
			std::memcpy(g_before.zoneInfo, Ptr<uint8_t>(world::ZONE_INFO_ARRAY), world::ZONE_INFO_BYTES);
		}
		break;
	case world::op::SET_GANG_WEAPONS: {
		int32_t gang = -1;
		if (!replay::LiteralAt(run.code, run.length, 0, &gang) || gang < 0 ||
		    gang >= static_cast<int32_t>(world::GANG_COUNT) || g_before.gang[gang])
			break;
		const uintptr_t at            = world::GANGS + static_cast<uintptr_t>(gang) * world::GANG_SIZE;
		g_before.gang[gang]           = true;
		g_before.gangWeapons[gang][0] = Global<int32_t>(at + world::GANG_WEAPON_1);
		g_before.gangWeapons[gang][1] = Global<int32_t>(at + world::GANG_WEAPON_2);
		break;
	}
	case world::op::SET_THREAT_FOR_PED_TYPE:
	case world::op::CLEAR_THREAT_FOR_PED_TYPE: {
		int32_t type = -1;
		if (!replay::LiteralAt(run.code, run.length, 0, &type) || type < 0 ||
		    type >= static_cast<int32_t>(world::PED_TYPES) || g_before.threat[type])
			break;
		const uintptr_t entry = Global<uintptr_t>(CPedType__ms_apPedType + 4u * static_cast<uint32_t>(type));
		if (!entry)
			break;
		g_before.threat[type]  = true;
		g_before.threats[type] = *reinterpret_cast<const uint32_t *>(entry + offs::PEDTYPE_THREATS);
		break;
	}
	default: break;
	}
}

// The streets as they were before the owner's mission: the densities always,
// which are nobody's save; the zones and the gangs when `campaignToo`, the
// mission's own cleanup never having reached this machine. Then forgotten.
void RestoreWorld(bool campaignToo) {
	if (g_before.pedDensity)
		Global<float>(world::PED_DENSITY) = g_before.ped;
	if (g_before.carDensity)
		Global<float>(world::CAR_DENSITY) = g_before.car;
	bool zones = false, gangs = false;
	if (campaignToo && g_before.zones) {
		std::memcpy(Ptr<uint8_t>(world::ZONE_INFO_ARRAY), g_before.zoneInfo, world::ZONE_INFO_BYTES);
		zones = true;
	}
	for (size_t g = 0; campaignToo && g < world::GANG_COUNT; ++g)
		if (g_before.gang[g]) {
			RunOurs(world::op::SET_GANG_WEAPONS,
			        {static_cast<int32_t>(g), g_before.gangWeapons[g][0], g_before.gangWeapons[g][1]});
			gangs = true;
		}
	for (size_t t = 0; campaignToo && t < world::PED_TYPES; ++t) {
		if (!g_before.threat[t])
			continue;
		const uintptr_t entry = Global<uintptr_t>(CPedType__ms_apPedType + 4u * static_cast<uint32_t>(t));
		if (entry)
			*reinterpret_cast<uint32_t *>(entry + offs::PEDTYPE_THREATS) = g_before.threats[t];
		gangs = true;
	}
	if (g_before.pedDensity || g_before.carDensity || zones || gangs)
		Log("missions: the streets are as they were before the mission here%s",
		    zones || gangs ? ", its zones and gangs too: its own cleanup never reached us" : "");
	g_before = WorldBefore{};
}

// One the owner handed us alone (MissionEffectBody::onlyTo) of what its
// mission has up, and up here already: what went to everybody while we came
// in reached us before the owner heard we had.
bool AlreadyUp(const MissionEffectBody &body) {
	switch (body.kind) {
	case MISSION_EFFECT_BLIP_NEW:   return g_blipMap.Ours(body.ownerBlip) != -1;
	case MISSION_EFFECT_PICKUP_NEW: return g_pickupMap.Ours(body.ownerBlip) != -1;
	case MISSION_EFFECT_FIRE_NEW:   return g_fireMap.Ours(body.ownerBlip) != -1;
	case MISSION_EFFECT_SPHERE_NEW: return g_sphereMap.Ours(body.ownerBlip) != -1;
	default:                        break;
	}
	uint16_t made = 0;
	if (replay::OutGlobalOf(body.code, body.length, &made)) {
		const uint32_t end = GlobalsEnd(Space(), MAIN_SCRIPT_SIZE);
		if (made < 8 || made + 4u > end)
			return false;
		const int32_t handle = At<int32_t>(Space(), made);
		for (size_t i = 0; i < g_madeObjectCount; ++i)
			if (g_madeObjects[i] == handle)
				return Func<GetPedFn>(CPools__GetObject)(handle) != nullptr;
		return false;
	}
	const uint16_t opcode = EffectOpcode(body);
	const int32_t  global = WidgetGlobalOf(body);
	if (opcode == op::DISPLAY_ONSCREEN_TIMER)
		return global != 0 && g_shown.timer == global;
	if (opcode == op::DISPLAY_ONSCREEN_COUNTER || opcode == op::DISPLAY_ONSCREEN_COUNTER_WITH_STRING)
		return global != 0 && g_shown.counter == global;
	return false;
}

// The owner's mission took one of its objects off its cleanup list
// (DONT_REMOVE_OBJECT): it stays in the world after the mission, the way the
// owner's own does, and is not let go of here when the mission ends. Give Me
// Liberty's wrecked police cars at the bridge are made once, behind a flag
// the campaign keeps, and a copy let go of after a failed try was never made
// again for the next.
void KeepMadeObject(uint16_t global) {
	size_t kept = 0;
	for (size_t i = 0; i < g_madeObjectCount; ++i)
		if (g_madeObjectGlobals[i] != global) {
			g_madeObjects[kept]       = g_madeObjects[i];
			g_madeObjectGlobals[kept] = g_madeObjectGlobals[i];
			++kept;
		}
	g_madeObjectCount = kept;
}

// The owner's mission changed its player's clothes: ours follow, once they
// safely can (game/outfit.h, TickOutfit). Taken even when there is nothing to
// do, so it is not waited on as a copy still being built.
bool WantOutfit(const MissionEffectBody &body) {
	char look[PLAYER_LOOK_LEN];
	if (body.length < 2 + 5 + TEXT_LABEL || !LookFromLabel(body.code + 7, look) ||
	    !LookIsClaude(look) || !LookInGameImage(look)) {
		if (!g_saidOutfitRefused) {
			g_saidOutfitRefused = true;
			Log("missions: the mission dressed its player in something that isn't one of "
			    "Claude's models here; ours keeps the clothes it has");
		}
		return true;
	}
	if (!g_outfit.Want(look, LocalPlayerModelName()))
		return true;
	g_saidOutfitSeat = false;
	Log("missions: the mission dressed its player in '%s'; ours changes too, on foot or in "
	    "their seat, as soon as nothing is half done",
	    look);
	return true;
}

// Where this machine's player is, in his car's place when he sits in one, the
// way FindPlayerCoors answers it. False with no player.
bool OurPlayerAt(float out[3]) {
	void *const me = Func<PlayerFn>(FindPlayerPed)();
	if (!me)
		return false;
	void *const car = Field<uint8_t>(me, offs::PED_IN_VEHICLE) != 0 ? Field<void *>(me, offs::PED_MY_VEHICLE)
	                                                                : nullptr;
	const float *pos = &Field<float>(car ? car : me, offs::POSITION);
	out[0] = pos[0];
	out[1] = pos[1];
	out[2] = pos[2];
	return true;
}

// ---- an instruction off the wire ---------------------------------------------------

uint16_t EffectOpcodeOf(const uint8_t *code) { return static_cast<uint16_t>(code[0] | (code[1] << 8)); }

// A model operand as the handler will build or stream from it: a negative one
// through main.scm's used-object table (addresses.h, UsedObjectArray). -1 for
// one no handler may be given: past that table, past the model infos, or a
// model this game has no info for, which RequestModel and CObject's
// SetModelIndex both read through.
int32_t ModelHere(int32_t model, bool usedObjects) {
	if (model < 0) {
		const uint32_t slot  = 0u - static_cast<uint32_t>(model);
		const uint16_t count = Global<uint16_t>(CTheScripts__NumberOfUsedObjects);
		if (!usedObjects || slot >= count || slot >= USED_OBJECTS_MAX)
			return -1;
		model = Global<int32_t>(CTheScripts__UsedObjectArray + slot * USED_OBJECT_STRIDE +
		                        USED_OBJECT_INDEX);
		if (model < 0)
			return -1;
	}
	return VehicleModelInfo(static_cast<uint32_t>(model)) != nullptr ? model : -1;
}

// Whether an instruction another machine sent may be handed to our
// interpreter at all (replay.h, WellFormed and OperandsInRange), and names a
// model we have, where it names one. Said the first few times it is not.
uint32_t g_refusedEffects = 0;

bool EffectSafeHere(const uint8_t *code, size_t length) {
	const char    *why = nullptr;
	const uint32_t end = GlobalsEnd(Space(), MAIN_SCRIPT_SIZE);
	if (!replay::WellFormed(code, length, end)) {
		why = "is not shaped as an instruction on the replay list is sent";
	} else if (!replay::OperandsInRange(code, length)) {
		why = "names a slot past the end of the engine's table for it";
	} else {
		bool          usedObjects = false;
		const int     at          = replay::ModelOperand(EffectOpcodeOf(code), &usedObjects);
		int32_t       model       = 0;
		if (at >= 0 && replay::LiteralAt(code, length, static_cast<uint8_t>(at), &model) &&
		    ModelHere(model, usedObjects) < 0)
			why = "names a model this game has no model info for";
	}
	if (!why)
		return true;
	if (g_refusedEffects++ < 8)
		Log("missions: an instruction %02X%02X off the wire %s; it is not run here", code[1], code[0],
		    why);
	return false;
}

// CREATE_OBJECT builds from its model and adds what it made to the world
// whether the model is in or not; the owner's script loaded it in the same
// go, and a participant who came in after that never asked for it. Asked for
// here and loaded, the way the script would; not made when it still is not in.
bool ObjectModelReady(const replay::Encoded &run) {
	if (!replay::BuildsFromModel(EffectOpcodeOf(run.code)))
		return true;
	int32_t model = 0;
	if (!replay::LiteralAt(run.code, run.length, 0, &model))
		return true;
	const int32_t here = ModelHere(model, true);
	if (here < 0)
		return false;
	if (HasModelLoaded(static_cast<uint32_t>(here)))
		return true;
	RunOurs(cutscene_op::REQUEST_MODEL, {here});
	LoadRequestedModelsNow();
	if (HasModelLoaded(static_cast<uint32_t>(here)))
		return true;
	const char *const name = NameOfModel(here);
	Log("missions: an object of model %d ('%s') is not made here: the model would not load",
	    static_cast<int>(here), name ? name : "?");
	return false;
}

// The owner's LOAD_COLLISION_WITH_SCREEN: the island its script decided on
// from where the owner stands, loaded here only where that cannot take our
// own player's ground away (rampagevote.h, MayLoadIslandUnder). Dropped
// otherwise, and the move that follows loads what it needs itself
// (PreloadIsland).
bool LoadIslandHere(const MissionEffectBody &body) {
	int32_t wanted = 0;
	float   here[3];
	if (!EffectSafeHere(body.code, body.length))
		return true;
	if (body.length > replay::MAX_CODE || !replay::LiteralAt(body.code, body.length, 0, &wanted) ||
	    !OurPlayerAt(here))
		return true;
	const int32_t standing = IslandAt(here[0], here[1], here[2]);
	if (!MayLoadIslandUnder(wanted, standing)) {
		Log("missions: the owner's mission loads island %d, and we stand on island %d; ours "
		    "stays loaded",
		    static_cast<int>(wanted), static_cast<int>(standing));
		return true;
	}
	if (IslandLoaded() == wanted)
		return true;
	Log("missions: the owner's mission loads island %d, and so does ours", static_cast<int>(wanted));
	return RunHere(body.code, body.length, nullptr);
}

bool RunEffect(const MissionEffectBody &body) {
	if (!g_hooks[R200].detour.IsInstalled() || body.kind == MISSION_EFFECT_TELEPORT)
		return false;
	if (EffectOpcode(body) == op::LOAD_COLLISION_WITH_SCREEN)
		return LoadIslandHere(body);
	// A blue marker is drawn every frame, never run once (mission.h).
	uint32_t   markerId = 0;
	MarkerArea marker;
	bool       markerUp = false;
	if (ReadMarkerEffect(body, &markerId, &marker, &markerUp)) {
		HeardMarker(markerId, marker, markerUp);
		return true;
	}
	// And so is a corona.
	uint32_t          coronaId = 0;
	shape::CoronaDraw corona;
	bool              coronaUp = false;
	if (shape::ReadCoronaEffect(body, &coronaId, &corona, &coronaUp)) {
		HeardCorona(coronaId, corona, coronaUp);
		return true;
	}
	// And one of the owner's power pills, written into our own table.
	uint16_t        pillSlot = 0;
	replay::PillRow pill;
	if (body.kind == MISSION_EFFECT_RUN &&
	    replay::ReadPillCode(body.code, body.length, &pillSlot, &pill)) {
		ShowPill(pillSlot, pill);
		return true;
	}
	// A session car the owner's mission takes away: our player out of it, or
	// off it (mission.h, LeaveCarEffect). The car goes by the session.
	uint16_t leaveNetId = INVALID_NETID;
	if (ReadLeaveCarEffect(body, &leaveNetId)) {
		const int32_t handle = CarForNetId(leaveNetId);
		LeaveCarHere(handle >= 0 ? VehicleAt(handle) : nullptr);
		return true;
	}
	if (!EffectSafeHere(body.code, body.length))
		return false;
	// The owner's line plays once ours is in: the owner's mission waited for
	// its own, and the engine drops a play that comes before the load
	// (cAudioManager::PlayLoadedMissionAudio). EffectAwaits holds it meanwhile.
	if (EffectOpcode(body) == world::op::PLAY_MISSION_AUDIO && g_shown.missionAudio &&
	    !AskHere(world::op::HAS_MISSION_AUDIO_LOADED, {}))
		return false;
	if (EffectOpcode(body) == op::UNDRESS_CHAR)
		return WantOutfit(body);
	// The rest of a scene that was not loaded here is not run either: its
	// objects, its animations and its start have nothing to belong to, and
	// SET_CUTSCENE_ANIM on no scene reads through the animation it did not
	// find. Its CLEAR_CUTSCENE ends the skipping.
	const uint16_t opcode0 = EffectOpcode(body);
	// The owner's Catalina taking off or flying away, before ours is in its
	// slot: held, and run the frame it is (TickMissions).
	if ((opcode0 == OP_CATALINA_HELI_TAKE_OFF || opcode0 == OP_CATALINA_HELI_FLY_AWAY) &&
	    !CatalinaHere()) {
		if (g_shown.catalina) {
			g_shown.catalinaPath = opcode0;
			Log("missions: the owner's Catalina %s before ours is up; ours does it when it is",
			    opcode0 == OP_CATALINA_HELI_TAKE_OFF ? "takes off" : "flies away");
		}
		return true;
	}
	if (g_shown.sceneSkipped && (IsCutsceneStep(opcode0) || opcode0 == op::CLEAR_CUTSCENE)) {
		if (opcode0 == op::CLEAR_CUTSCENE) {
			g_shown.sceneSkipped = false;
			g_objectMap.Clear();
		}
		return true;
	}
	if (RenameUnderLiveEntities(body))
		return false;   // EffectAwaits: until nothing here is built from it
	if (opcode0 == op::LOAD_CUTSCENE) {
		// The owner cleared its last scene before loading this one, and the
		// engine only ever takes the last scene's animations back out: one
		// still loaded here missed its CLEAR_CUTSCENE, which goes first.
		if (g_shown.cutscene) {
			Log("missions: the owner's last cutscene is still loaded here as the next one comes; "
			    "clearing it first");
			RunOurs(op::CLEAR_CUTSCENE, {});
			g_shown.cutscene = false;
			g_objectMap.Clear();
			SetCutsceneScene(false);
		}
		// The scene's Claude is built from model 0 (CREATE_CUTSCENE_OBJECT
		// #NULL), so clothes still owed go on before it is.
		if (g_outfit.Pending()) {
			TickOutfit();
			if (g_outfit.Undressing()) {
				Log("missions: not loading the owner's cutscene here: our player is out of the "
				    "world waiting for '%s', and the scene would be built from a model that isn't in",
				    g_outfit.Look());
				g_shown.sceneSkipped = true;
				return true;
			}
			if (g_outfit.Pending())
				Log("missions: the owner's cutscene starts with our player still in '%s'; the "
				    "change to '%s' waits for a moment it can be made in",
				    LocalPlayerModelName() ? LocalPlayerModelName() : "?", g_outfit.Look());
		}
		g_shown.sceneSkipped = !CutsceneModelsHere(body);
		if (g_shown.sceneSkipped)
			return true;
	}
	if (body.onlyTo != 0 && AlreadyUp(body))
		return true;
	replay::Encoded enc;
	enc.length    = body.length;
	enc.handleAt  = body.handleAt;
	enc.ownerBlip = body.ownerBlip;
	std::memcpy(enc.code, body.code, body.length <= replay::MAX_CODE ? body.length : 0);
	replay::Handles h;
	h.blips   = &g_blipMap;
	h.objects = &g_objectMap;
	h.pickups = &g_pickupMap;
	h.fires   = &g_fireMap;
	h.spheres = &g_sphereMap;
	h.charOf  = &CharForNetId;
	h.carOf   = &CarForNetId;
	replay::Encoded run;
	if (!replay::Translate(enc, h, &run))
		return false;   // a blip, an object, a pedestrian or a car we do not have
	// And each of ours still there: the roster row or the object map can name
	// one our engine took away since, and the handler would call through the
	// null its GetAt gives back (replay.h, EntitiesLive).
	if (!replay::EntitiesLive(run.code, run.length, [](replay::Arg a, int32_t handle) {
		    return (a == replay::Arg::Char     ? PedAt(handle)
		            : a == replay::Arg::Car    ? VehicleAt(handle)
		                                       : ObjectAt(handle)) != nullptr;
	    })) {
		static bool said = false;
		if (!said) {
			said = true;
			Log("missions: an instruction %02X%02X of the owner's mission names a pedestrian, "
			    "car or object ours of is already gone; not run here (said once)",
			    body.code[1], body.code[0]);
		}
		return false;
	}
	// A blip on a pickup: the handler puts it on the pickup's object without
	// asking whether the slot or the object is still there (replay.h).
	if (opcode0 == OP_ADD_BLIP_FOR_PICKUP || opcode0 == OP_ADD_SPRITE_BLIP_FOR_PICKUP) {
		int32_t pickup = -1;
		if (!replay::LiteralAt(run.code, run.length, 0, &pickup) || !PickupObjectUp(pickup))
			return true;   // taken here already: no blip to put on it
	}
	// An object named by its global is this machine's own, and the handler
	// takes whatever the global holds on trust: a live one, or nothing is run.
	uint16_t objects[4];
	const size_t objectCount = replay::ObjectGlobals(run.code, run.length, objects, 4);
	const uint32_t end       = GlobalsEnd(Space(), MAIN_SCRIPT_SIZE);
	for (size_t i = 0; i < objectCount; ++i) {
		if (objects[i] < 8 || objects[i] + 4u > end)
			return false;
		const int32_t handle = At<int32_t>(Space(), objects[i]);
		if (Func<GetPedFn>(CPools__GetObject)(handle) == nullptr)
			return false;
	}
	if (!CutsceneObjectModelReady(run) || !ObjectModelReady(run))
		return true;   // not made, and what names it is dropped with it
	// A sound taken off by our own global, which the handler takes on trust.
	uint16_t soundGlobal = 0;
	bool     soundMakes  = false;
	if (replay::SoundGlobalOf(run.code, run.length, &soundGlobal, &soundMakes) && !soundMakes &&
	    !SoundHandleInPool(soundGlobal))
		return true;
	// A contact's marker, ours in our own global (replay.h, Arg::OutBlip).
	if (!ReadyBlipGlobal(run.code, run.length))
		return false;
	// A pickup the world keeps, ours in our own global (replay.h,
	// `pickupByGlobal`).
	if (!ReadyPickupGlobal(run.code, run.length))
		return false;
	NoteWorldBefore(run);
	int32_t local0 = -1;
	{
		// The owner's clear takes no session car here: the owner's machine
		// takes the ones it means to (ClearArea).
		const SessionCarsLocked locked(opcode0 == world::op::CLEAR_AREA);
		if (!RunHere(run.code, run.length, &local0))
			return false;
	}
	uint16_t made = 0;
	if (replay::OutGlobalOf(run.code, run.length, &made) && made >= 8 && made + 4u <= end &&
	    g_madeObjectCount < MAX_MADE_OBJECTS) {
		g_madeObjectGlobals[g_madeObjectCount] = made;
		g_madeObjects[g_madeObjectCount++]     = At<int32_t>(Space(), made);
	}
	const uint16_t opcode = static_cast<uint16_t>(run.code[0] | (run.code[1] << 8));
	// The owner's bomb, which the handler has just made ours: the car names
	// the owner, on this copy and in the session's row (Client::MissionArmedCar).
	int32_t armedNetId = -1, armedType = 0;
	if (opcode == op::ARM_CAR_WITH_BOMB && g_client &&
	    ArmedCarIn(body.code, body.length, &armedNetId, &armedType))
		g_client->MissionArmedCar(static_cast<uint16_t>(armedNetId),
		                          static_cast<uint8_t>(armedType), false);
	if (body.kind == MISSION_EFFECT_BLIP_NEW)
		g_blipMap.Add(body.ownerBlip, local0);
	else if (body.kind == MISSION_EFFECT_OBJECT_NEW)
		g_objectMap.Add(body.ownerBlip, local0);
	else if (body.kind == MISSION_EFFECT_FIRE_NEW)
		g_fireMap.Add(body.ownerBlip, local0);
	else if (body.kind == MISSION_EFFECT_SPHERE_NEW) {
		g_sphereMap.Add(body.ownerBlip, local0);
		// Under an id of its own, or two of ours would share one marker
		// (missionworld.h, SCRIPT_SPHERES).
		const int32_t   slot = world::SphereSlot(local0);
		const uintptr_t at   = world::SCRIPT_SPHERES + static_cast<uintptr_t>(slot) * world::SCRIPT_SPHERE;
		if (slot >= 0 && Global<uint8_t>(at + world::SPHERE_IN_USE) != 0)
			Global<uint32_t>(at + world::SPHERE_ID) =
			    world::SHOWN_SPHERE_ID | (static_cast<uint32_t>(body.ownerBlip) & 0x0FFFFFFFu);
	} else if (opcode == world::op::REMOVE_SPHERE) {
		int32_t owner = -1;
		if (replay::LiteralAt(body.code, body.length, 0, &owner))
			g_sphereMap.Remove(owner);
	}
	else if (opcode == op::REMOVE_SCRIPT_FIRE) {
		int32_t owner = -1;
		if (replay::LiteralAt(body.code, body.length, 0, &owner))
			g_fireMap.Remove(owner);
	} else if (body.kind == MISSION_EFFECT_PICKUP_NEW) {
		g_pickupMap.Add(body.ownerBlip, local0);
		RememberStash(run, body.ownerBlip, local0);
		if (opcode == op::CREATE_FLOATING_PACKAGE)
			NoteFloating(body.ownerBlip, local0);
	} else if (opcode == op::REMOVE_PICKUP) {
		int32_t owner = -1;
		if (replay::LiteralAt(body.code, body.length, 0, &owner)) {
			g_pickupMap.Remove(owner);
			FloatingRemoved(owner);
		}
	}
	else if (body.kind == MISSION_EFFECT_BLIP_USE && opcode == op::REMOVE_BLIP)
		g_blipMap.Remove(body.ownerBlip);
	else if (opcode == op::DONT_REMOVE_OBJECT)
		for (size_t i = 0; i < objectCount; ++i)
			KeepMadeObject(objects[i]);
	NoteShown(run);
	return true;
}

// RunEffect said no, and the only reason is a pedestrian or car it names that
// the session has named and our copy of is still being built: the row is
// here, the object is not yet (its model streaming in). The owner sends an
// instruction the moment the session names what its mission made, and on this
// end that name lands with the spawn itself, a frame or more before the copy
// exists - so CREATE_CAR's colour, its blip or a camera on it used to be
// dropped here for being early (missionsync.h, MISSION_EFFECT_AWAIT_MS).
bool EffectAwaits(const MissionEffectBody &body) {
	if (!g_client || body.length > replay::MAX_CODE)
		return false;
	// The owner's line waits for ours to load (RunEffect).
	if (EffectOpcode(body) == world::op::PLAY_MISSION_AUDIO)
		return g_shown.missionAudio;
	// A special model's rename waits for what is still built from it
	// (RenameUnderLiveEntities), and is dropped if that never goes.
	if (RenameUnderLiveEntities(body))
		return true;
	uint8_t code[replay::MAX_CODE];
	std::memcpy(code, body.code, body.length);
	bool coming = false;
	replay::EachHandle(code, body.length, [&](replay::Arg a, int32_t *v) {
		// Our Catalina, started and not in its slot yet: its model streaming.
		if (a == replay::Arg::Car && *v == replay::WIRE_CATALINA_HELI) {
			if (CatalinaRef() < 0 && Global<uint8_t>(CHeli__CatalinaHeliOn) != 0)
				coming = true;
			return true;
		}
		if (*v < 0 || *v > 0xFFFF)
			return true;
		const uint16_t netId = static_cast<uint16_t>(*v);
		if (a == replay::Arg::Char && CharForNetId(*v) < 0) {
			const RemoteAmbientPed *p = g_client->AmbientPed(netId);
			if (p && p->poolHandle < 0 && !p->dead)
				coming = true;
		} else if (a == replay::Arg::Car && CarForNetId(*v) < 0) {
			const RemoteAmbientCar *c = g_client->AmbientCar(netId);
			const RemoteVehicle    *s = g_client->VehicleByNetId(netId);
			if ((c && c->poolHandle < 0 && !c->destroyed) || (s && s->poolHandle < 0 && s->spawnPending))
				coming = true;
		}
		return true;
	});
	return coming;
}

// A move to a place on another island loads that island first, through the
// engine's own LOAD_COLLISION_WITH_SCREEN, the way the owner's script did
// before its own move (Last Requests' Staunton). Which island, and whether it
// is already the one in memory, is the rampage move's reckoning
// (rampagevote.h, IslandAt and IslandToLoadFirst). The load itself is not:
// that move holds its player up in the air until CCollision::Update gets
// round to it, where this one has to happen inside the instruction, car and
// all, and on ground the line tests below can already see.
void PreloadIsland(float x, float y, float z) {
	const int32_t level = IslandToLoadFirst(IslandAt(x, y, z), IslandLoaded());
	if (level == 0)
		return;
	uint8_t code[7] = {static_cast<uint8_t>(op::LOAD_COLLISION_WITH_SCREEN & 0xFF),
	                   static_cast<uint8_t>(op::LOAD_COLLISION_WITH_SCREEN >> 8), PARAM_INT32};
	std::memcpy(code + 3, &level, 4);
	Log("missions: the owner's mission moves us to island %d and we have %d loaded; loading it "
	    "first",
	    static_cast<int>(level), static_cast<int>(IslandLoaded()));
	if (!RunHere(code, sizeof code, nullptr))
		Log("missions: island %d could not be loaded here; moved all the same",
		    static_cast<int>(level));
}

// SET_PLAYER_COORDINATES the owner's mission ran, on this machine's own
// player: beside the owner's spot rather than on it (missions.md 11.2), on
// the first of the spots SpreadSpot offers that the buildings leave in
// sight of it, and on the owner's spot when none is. A player in a car moves
// the car only when the owner moved in one too and this machine simulates it
// (MissionMoveFor): a pedestrian's move leaves the car where it stands.
bool Teleport(const MissionEffectBody &body, uint8_t rank, uint8_t count) {
	if (!g_hooks[R200].detour.IsInstalled() || body.length > replay::MAX_CODE)
		return false;
	// A move and nothing else: the instruction is run as it came, with no
	// handle in it translated.
	const replay::Entry *entry = replay::Find(EffectOpcode(body));
	if (!entry || entry->kind != replay::Kind::Teleport || !EffectSafeHere(body.code, body.length))
		return false;
	uint8_t code[replay::MAX_CODE];
	std::memcpy(code, body.code, body.length);
	// SET_PLAYER_COORDINATES and WARP_PLAYER_FROM_CAR_TO_COORD name the player
	// first; RESTART_CRITICAL_MISSION starts with the place.
	const uint8_t at = EffectOpcode(body) == world::op::RESTART_CRITICAL_MISSION ? 0 : 1;
	int32_t xb = 0, yb = 0, zb = 0;
	if (!replay::LiteralAt(code, body.length, at, &xb) ||
	    !replay::LiteralAt(code, body.length, static_cast<uint8_t>(at + 1), &yb) ||
	    !replay::LiteralAt(code, body.length, static_cast<uint8_t>(at + 2), &zb))
		return false;
	float x, y, z;
	std::memcpy(&x, &xb, 4);
	std::memcpy(&y, &yb, 4);
	std::memcpy(&z, &zb, 4);
	// A move off the wire that no island holds: the handler files our player
	// in the sector grid at it, which CWorld::Add indexes unchecked.
	if (!InsideWorld(x, y, z))
		return false;
	void *const me  = Func<PlayerFn>(FindPlayerPed)();
	void *const car = me && Field<uint8_t>(me, offs::PED_IN_VEHICLE) != 0
	                      ? Field<void *>(me, offs::PED_MY_VEHICLE)
	                      : nullptr;
	MissionMove move = MissionMove::OnFoot;
	if (at == 1 && car && g_client) {
		// What the handler would do to the car we sit in, and whether it may
		// (MissionMoveFor): only the machine that simulates a car moves it,
		// and a pedestrian's move is no car's.
		MissionMoveFacts f;
		f.warpsOut           = EffectOpcode(body) != op::SET_PLAYER_COORDINATES;
		f.inCar              = true;
		f.ownerTag           = body.ownerBlip;
		const int32_t handle = Func<int32_t(__cdecl *)(void *)>(CPools__GetVehicleRef)(car);
		const uint16_t netId = g_client->SessionCarNetIdOf(handle);
		const RemoteVehicle *row = netId != INVALID_NETID ? g_client->VehicleByNetId(netId) : nullptr;
		f.simulateHere =
		    !row || WhoSimulates(true, row->driverPlayerId, row->custodianPlayerId, INVALID_PLAYER,
		                         LocalId())
		                    .where == CarSim::Here;
		const uint16_t ownerCar = OwnerMoveCar(body.ownerBlip);
		f.ownersCar = ownerCar != INVALID_NETID && CarForNetId(ownerCar) == handle;
		const float *p = &Field<float>(me, offs::POSITION);
		f.distanceM    = std::sqrt((p[0] - x) * (p[0] - x) + (p[1] - y) * (p[1] - y));
		move           = MissionMoveFor(f);
		static bool said[static_cast<size_t>(MissionMove::Count)] = {};
		bool &once = said[static_cast<size_t>(move)];
		if (!once && move != MissionMove::CarRing) {
			once = true;
			switch (move) {
			case MissionMove::StayInCar:
				Log("missions: the owner's mission put its player, on foot, at (%.1f, %.1f), "
				    "%.0f m from the car we are in; we stay in it where it is rather than have "
				    "the car follow a pedestrian's move into a building",
				    x, y, f.distanceM);
				break;
			case MissionMove::OutOfCar:
				Log("missions: the owner's mission put its player %s at (%.1f, %.1f), %.0f m "
				    "from the car we are in; we get out and go beside them on foot, and the car "
				    "stays where it is",
				    f.warpsOut ? "out of their car" : "on foot", x, y, f.distanceM);
				break;
			case MissionMove::StaySeated:
				Log("missions: the owner's mission moved its player in a car while we ride in "
				    "car %u, which another machine simulates; that machine moves it and we "
				    "stay in our seat",
				    static_cast<unsigned>(netId));
				break;
			case MissionMove::CarOnSpot:
				Log("missions: the owner's mission moved its player in car %u, which we "
				    "simulate; the car goes onto the owner's spot itself",
				    static_cast<unsigned>(netId));
				break;
			default: break;
			}
		}
		if (move == MissionMove::StayInCar || move == MissionMove::StaySeated)
			return true;
	}
	// A z of -100 asks the engine for the ground, which is inside every
	// island's zones (map.zon's run from -133 to 467).
	PreloadIsland(x, y, z);
	// The handler moves the car of a player in one, so the ring has to fit
	// cars (SpotRadii). WARP_PLAYER_FROM_CAR_TO_COORD leaves the car where it
	// is and puts the player down on foot.
	const bool   inCar = car && (move == MissionMove::CarRing || move == MissionMove::CarOnSpot ||
	                            (at == 0 && move == MissionMove::OnFoot));
	if (move == MissionMove::OutOfCar) {
		code[0] = static_cast<uint8_t>(op::WARP_PLAYER_FROM_CAR_TO_COORD & 0xFF);
		code[1] = static_cast<uint8_t>(op::WARP_PLAYER_FROM_CAR_TO_COORD >> 8);
	}
	// A car's ring for a participant on foot too when the owner moved in a car
	// (MoveRadii): the owner's car is what stands on the spot.
	const float *radii = MoveRadii(inCar, body.ownerBlip);
	float        sx = x, sy = y;
	// The owner rides in the car we drive: it goes where the owner's engine
	// put its copy of it, with nobody else to make room for.
	bool         found = move == MissionMove::CarOnSpot;
	for (uint8_t ring = 0; ring < 2 && !found; ++ring) {
		for (uint8_t attempt = 0; attempt < SPREAD_ATTEMPTS; ++attempt) {
			// Another participant's own place is theirs, even when ours is in
			// the wall.
			if (SpreadSpotTaken(rank, count, attempt))
				continue;
			float cx = 0.0f, cy = 0.0f;
			SpreadSpot(x, y, rank, count, attempt, &cx, &cy, radii[ring]);
			// The engine finds the ground itself for a z at or below -100, and
			// nothing here knows how high it will be, so there is no line to test.
			const Vec3f from{x, y, z + 1.0f}, to{cx, cy, z + 1.0f};
			if (z <= SCRIPT_Z_FIND_GROUND ||
			    Func<LineOfSightFn>(CWorld__GetIsLineOfSightClear)(&from, &to, 1, 0, 0, 1, 0, 0, 0) !=
			        0) {
				sx    = cx;
				sy    = cy;
				found = true;
				break;
			}
		}
	}
	// Nowhere round it the buildings leave clear. A pedestrian on the owner's
	// spot steps off it (standapart.h: the physics alone pushed both players
	// round each other); a car on the owner's, or on the car the owner sits
	// in, does not, so a car keeps its own place on the ring, and so does a
	// pedestrian when the owner's car is what is on the spot
	// (MayStandOnOwnersSpot).
	if (!found && !MayStandOnOwnersSpot(inCar, body.ownerBlip))
		SpreadSpot(x, y, rank, count, 0, &sx, &sy, radii[0]);
	static bool said = false;
	if (!said) {
		said = true;
		Log("missions: moved beside the owner's spot as %u of %u, %s, %.1f m out; the owner "
		    "moved %s",
		    rank + 1u, static_cast<unsigned>(count),
		    inCar ? "in our car" : car ? "out of our car" : "on foot",
		    std::sqrt((sx - x) * (sx - x) + (sy - y) * (sy - y)),
		    OwnerMovedInCar(body.ownerBlip) ? "in a car, so we keep a car's length off it"
		                                    : "on foot");
	}
	int32_t bits = 0;
	std::memcpy(&bits, &sx, 4);
	replay::SetLiteralAt(code, body.length, at, bits);
	std::memcpy(&bits, &sy, 4);
	replay::SetLiteralAt(code, body.length, static_cast<uint8_t>(at + 1), bits);
	if (at == 0)
		Log("missions: the owner's mission starts over from (%.1f, %.1f); so does ours, beside it",
		    x, y);
	NoteStandPlacement(at == 0 ? "the owner's critical mission restart"
	                           : "the owner's mission moving its player");
	// The warp out nils m_pMyVehicle under whatever door animation he is in
	// (game/animcb.h): its callbacks come off first, on this car.
	if (car && static_cast<uint16_t>(code[0] | (code[1] << 8)) == op::WARP_PLAYER_FROM_CAR_TO_COORD)
		LetGoOfCarChain(me);
	return RunHere(code, body.length, nullptr);
}

// ---- somebody who comes in late (missions.md 11.5) -----------------------------------

// The object this machine's own global holds, or null.
void *ObjectIn(uint16_t global, uint32_t end) {
	if (global < 8 || global + 4u > end)
		return nullptr;
	return Func<GetPedFn>(CPools__GetObject)(At<int32_t>(Space(), global));
}

// Whether what a kept effect put up is still up on this machine: a pickup
// nobody has taken, the stash aside, which stays everybody's own until the
// mission ends; an object that is still there.
bool StillUpHere(const MissionEffectBody &b, uint32_t end) {
	if (b.kind == MISSION_EFFECT_PICKUP_NEW && !IsStash(b.ownerBlip) && !PickupStillUp(b.ownerBlip))
		return false;
	uint16_t     globals[4];
	size_t       n    = replay::ObjectGlobals(b.code, b.length, globals, 3);
	uint16_t     made = 0;
	if (replay::OutGlobalOf(b.code, b.length, &made))
		globals[n++] = made;
	for (size_t i = 0; i < n; ++i)
		if (!ObjectIn(globals[i], end))
			return false;
	return true;
}

// Somebody has just come into our running mission, and is handed what it has
// up, alone: each thing as the session names it now, and an object where it
// is now, turned the way it is now. What went to everybody meanwhile may
// have reached them first, and their machine skips what it has already.
void ResendStanding(uint8_t playerId) {
	if (!g_client || !g_own.running || playerId >= MAX_PLAYERS)
		return;
	const uint32_t now  = WallClock::NowMs();
	const uint32_t end  = GlobalsEnd(Space(), MAIN_SCRIPT_SIZE);
	size_t         sent = 0;
	auto to = [&](MissionEffectBody b) {
		b.onlyTo = static_cast<uint8_t>(playerId + 1);
		g_client->Missions().SendEffect(b, now);
		++sent;
	};
	for (size_t i = 0; i < g_standing.Count(); ++i) {
		const MissionEffectBody &kept = g_standing.At(i);
		if (!StillUpHere(kept, end))
			continue;
		MissionEffectBody wire = kept;
		if (EffectToWire(wire) <= 0)
			continue;
		to(wire);
		const uint16_t opcode = EffectOpcode(kept);
		uint16_t       global = 0;
		if ((opcode != op::CREATE_OBJECT && opcode != op::CREATE_OBJECT_NO_OFFSET) ||
		    !replay::OutGlobalOf(kept.code, kept.length, &global))
			continue;
		void *const object = ObjectIn(global, end);
		if (!object)
			continue;
		const float *pos = &Field<float>(object, offs::POSITION);
		const float *fwd = &Field<float>(object, offs::MATRIX_FWD);
		to(ObjectPlaceEffect(g_own.number, global, pos[0], pos[1], pos[2]));
		to(ObjectHeadingEffect(g_own.number, global, HeadingDegrees(fwd[0], fwd[1])));
	}
	const RemotePlayer &who = g_client->PlayerSlot(playerId);
	Log("missions: %s came into %s, and was handed the %u thing%s it has up",
	    who.active ? who.nick.c_str() : "a player", MissionName(g_own.number),
	    static_cast<unsigned>(sent), sent == 1 ? "" : "s");
}

// ---- the mission's objects, broken (mission-audit.md R3) ----------------------------

// The global one of the session's mission's objects is in on this machine:
// on the owner, one its mission made and still has up; on a participant, one
// the owner's mission made here. 0 for none of them.
uint16_t GlobalOfMissionObject(void *object) {
	if (!object)
		return 0;
	const uint32_t end = GlobalsEnd(Space(), MAIN_SCRIPT_SIZE);
	if (g_own.running)
		for (size_t i = 0; i < g_standing.Count(); ++i) {
			const MissionEffectBody &kept   = g_standing.At(i);
			const uint16_t           opcode = EffectOpcode(kept);
			uint16_t                 global = 0;
			if ((opcode == op::CREATE_OBJECT || opcode == op::CREATE_OBJECT_NO_OFFSET) &&
			    replay::OutGlobalOf(kept.code, kept.length, &global) && ObjectIn(global, end) == object)
				return global;
		}
	for (size_t i = 0; i < g_madeObjectCount; ++i)
		if (ObjectIn(g_madeObjectGlobals[i], end) == object)
			return g_madeObjectGlobals[i];
	return 0;
}

// object.cpp: one of them broke here. What this machine's own player or car
// did goes to everybody else; what nobody did is the owner's to say.
void MissionObjectBroken(void *object, float amount, uint8_t state, bool ours) {
	if (!g_client || !(ours || g_own.running))
		return;
	const uint16_t global = GlobalOfMissionObject(object);
	if (global == 0)
		return;
	g_client->Missions().ObjectBroken(global, amount, state, WallClock::NowMs());
	static bool said = false;
	if (!said) {
		said = true;
		Log("missions: one of the mission's objects broke here, and breaks on everybody's "
		    "machine");
	}
}

// Somebody else's copy of the object this machine's `global` holds broke: ours
// breaks as far.
void BreakObject(uint16_t global, float amount, uint8_t state) {
	void *const object = ObjectIn(global, GlobalsEnd(Space(), MAIN_SCRIPT_SIZE));
	if (object && GlobalOfMissionObject(object) == global)
		ApplyMissionObjectBreak(object, amount, state);
}

// The value behind a widget the owner's HUD shows, into ours: only a global
// our own HUD is reading for that widget.
void SetWidget(uint16_t offset, int32_t value) {
	if (offset == 0 || (offset != g_shown.timer && offset != g_shown.counter))
		return;
	const uint32_t end = GlobalsEnd(Space(), MAIN_SCRIPT_SIZE);
	if (offset < 8 || offset + 4u > end || IsOnMissionGlobal(offset))
		return;
	At<int32_t>(Space(), offset) = value;
}

// ---- the campaign ------------------------------------------------------------------

// Which life of main.scm runs here, for MissionSync to know when to look at
// the campaign again: a new game or a load starts another. The player's pool
// reference changes then (moneysync.h reads the same), and the game's clock,
// which a load sets to the save's and a new game to its start, jumps. A jump
// that was not a load only costs a look that finds nothing to do.
constexpr uint32_t LIFE_CLOCK_JUMP_MS = 5000;
uint32_t           g_life      = 0;
bool               g_haveLife  = false;
int32_t            g_lifeRef   = 0;
uint32_t           g_lifeClock = 0;

uint32_t ScriptLife() {
	if (!g_hooks[R200].detour.IsInstalled())
		return 0;
	void *const ped = Func<PlayerFn>(FindPlayerPed)();
	if (!ped || GlobalsEnd(Space(), MAIN_SCRIPT_SIZE) == 0) {
		g_haveLife = false;
		return 0;
	}
	const int32_t  ref   = Func<int32_t(__cdecl *)(void *)>(CPools__GetPedRef)(ped);
	const uint32_t clock = Global<uint32_t>(CTimer__m_snTimeInMilliseconds);
	const uint32_t step  = clock - g_lifeClock;
	if (!g_haveLife || ref != g_lifeRef || static_cast<int32_t>(step) < 0 ||
	    step > LIFE_CLOCK_JUMP_MS) {
		if (++g_life == 0)
			g_life = 1;
		// A load's key, before the campaign runs anything over it.
		NoteOwnMissionName();
	}
	g_haveLife  = true;
	g_lifeRef   = ref;
	g_lifeClock = clock;
	return g_life;
}

bool ReadGlobal(uint16_t at, int32_t *value) {
	const uint32_t end = GlobalsEnd(Space(), MAIN_SCRIPT_SIZE);
	if (at < 8 || at + 4u > end)
		return false;
	*value = At<int32_t>(Space(), at);
	return true;
}

// Every object a campaign instruction names by its global is a live one of
// ours: the handler takes what the global holds on trust. A main-script
// barrier this game took away already (live, during the mission) leaves a
// handle whose slot has moved on, and is not taken away twice; 0 is never a
// handle of ours (a pool generation starts at 1).
bool ObjectGlobalsLive(const uint8_t *code, size_t length, uint32_t end) {
	uint16_t     globals[4];
	const size_t n = replay::ObjectGlobals(code, length, globals, 4);
	for (size_t i = 0; i < n; ++i)
		if (!ObjectIn(globals[i], end) || At<int32_t>(Space(), globals[i]) == 0)
			return false;
	return true;
}

// A delta somebody else's mission left, or one this machine's game lacks after
// a load: its globals written, its threads started unless one of them runs
// here already, under the name it gives itself or its label's.
bool ApplyCampaign(const CampaignDeltaBody &body) {
	if (!g_hooks[R200].detour.IsInstalled())
		return false;
	uint8_t       *space = Space();
	const uint32_t end   = GlobalsEnd(space, MAIN_SCRIPT_SIZE);
	if (end == 0)
		return false;
	// A REGISTER_MISSION_PASSED below writes over the game's own key.
	NoteOwnMissionName();
	if (body.opLength >= 2 && body.opLength <= MISSION_EFFECT_CODE &&
	    EffectSafeHere(body.op, body.opLength) && ReadyBlipGlobal(body.op, body.opLength) &&
	    ReadyPickupGlobal(body.op, body.opLength) && ObjectGlobalsLive(body.op, body.opLength, end))
		RunHere(body.op, body.opLength, nullptr);
	for (uint8_t i = 0; i < body.valueCount && i < CAMPAIGN_VALUES; ++i) {
		const uint16_t at = body.values[i].offset;
		// Every global is a whole, aligned word: a write half across two would
		// also reach round the $ONMISSION test.
		if (at < 8 || at + 4u > end || (at & 3u) != 0 || IsOnMissionGlobal(at))
			continue;
		std::memcpy(space + at, &body.values[i].value, 4);
	}
	for (uint8_t i = 0; i < body.threadCount && i < CAMPAIGN_THREADS; ++i) {
		const CampaignThread &t = body.threads[i];
		// In main.scm's code, not its variables, which the values above may
		// just have written.
		if (!IsMainThreadLabel(t.label) || static_cast<uint32_t>(t.label) < end)
			continue;
		char       tag[8];
		ThreadTag(t.label, tag);
		const bool named = t.name[0] != '\0';
		if (ThreadNamed(tag) || (named && ThreadNamed(t.name)))
			continue;
		uint8_t      code[8];
		const size_t length = StartThreadCode(t.label, code);
		RunHere(code, length, nullptr);
		// The engine puts a new script at the head of the active list.
		const uintptr_t fresh = Global<uintptr_t>(ACTIVE_SCRIPTS);
		if (!named && fresh != 0 &&
		    At<uint32_t>(reinterpret_cast<void *>(fresh), layout::SCRIPT_IP) ==
		        static_cast<uint32_t>(t.label))
			std::memcpy(reinterpret_cast<char *>(fresh + layout::SCRIPT_NAME), tag, 8);
	}
	return true;
}

// One instruction of ours with int32 literals, run here.
void RunOurs(uint16_t opcode, std::initializer_list<int32_t> values) {
	uint8_t code[2 + 5 * 4] = {static_cast<uint8_t>(opcode & 0xFF), static_cast<uint8_t>(opcode >> 8)};
	size_t  n               = 2;
	for (int32_t v : values) {
		if (n + 5 > sizeof code)
			break;
		code[n] = PARAM_INT32;
		std::memcpy(code + n + 1, &v, 4);
		n += 5;
	}
	RunHere(code, n, nullptr);
}

// One condition of ours with int32 literals, asked of this machine's engine:
// what the compare flag says after it, with no NOT and no and/or around it.
bool AskHere(uint16_t opcode, std::initializer_list<int32_t> values) {
	uint8_t code[2 + 5 * 4] = {static_cast<uint8_t>(opcode & 0xFF), static_cast<uint8_t>(opcode >> 8)};
	size_t  n               = 2;
	for (int32_t v : values) {
		if (n + 5 > sizeof code)
			break;
		code[n] = PARAM_INT32;
		std::memcpy(code + n + 1, &v, 4);
		n += 5;
	}
	return RunHere(code, n, nullptr) && At<uint8_t>(g_runner, layout::SCRIPT_COND_RESULT) != 0;
}

// A participant's own engine, asked the owner's questions (protocol.h,
// C_MissionAnswers), for the owner's mission to hear: the two garage ones of
// every garage main.scm made (mission-audit.md R5), which is the same list on
// every machine, and whether a mission Cessna flying here for the owner's
// mission has gone down (R7). A garage main.scm did not make is one of no
// type, and answers no.
void AskOwnEngine(uint32_t nowMs) {
	uint32_t hasCar = 0, resprayed = 0;
	uint16_t shotDown = 0;
	for (uint8_t g = 0; g < MISSION_GARAGES; ++g) {
		if (AskHere(op::IS_CAR_IN_MISSION_GARAGE, {g}))
			hasCar |= 1u << g;
		if (AskHere(op::HAS_RESPRAY_HAPPENED, {g}))
			resprayed |= 1u << g;
	}
	if ((g_planesWatched & MISSION_SHOT_DOWN_DRUG_PLANE) != 0 &&
	    AskHere(op::HAS_DRUG_PLANE_BEEN_SHOT_DOWN, {}))
		shotDown |= MISSION_SHOT_DOWN_DRUG_PLANE;
	if ((g_planesWatched & MISSION_SHOT_DOWN_DROP_OFF) != 0 &&
	    AskHere(op::HAS_DROP_OFF_PLANE_BEEN_SHOT_DOWN, {}))
		shotDown |= MISSION_SHOT_DOWN_DROP_OFF;
	if ((g_planesWatched & MISSION_SHOT_DOWN_CATALINA) != 0 &&
	    AskHere(static_cast<uint16_t>(OP_HAS_CATALINA_HELI_BEEN_SHOT_DOWN), {})) {
		shotDown |= MISSION_SHOT_DOWN_CATALINA;
		Log("missions: our Catalina went down; the owner's mission hears it");
	}
	g_planesWatched = static_cast<uint16_t>(g_planesWatched & ~shotDown);
	g_client->Missions().Answers(hasCar, resprayed, shotDown, LocalId(), nowMs);
}

// A widget's DISPLAY taken off again: `02 lo hi` names its global.
void ClearWidget(uint16_t opcode, uint16_t global) {
	const uint8_t code[5] = {static_cast<uint8_t>(opcode & 0xFF), static_cast<uint8_t>(opcode >> 8),
	                         PARAM_GLOBAL, static_cast<uint8_t>(global & 0xFF),
	                         static_cast<uint8_t>(global >> 8)};
	RunHere(code, sizeof code, nullptr);
}

// ---- a passenger the engine gave up on (mission.h, NextSeatOrderStep) --------------

struct SeatOrder {
	int32_t  ped       = -1;
	int32_t  car       = -1;
	uint32_t droppedMs = 0;
	bool     dropped   = false;
};
SeatOrder g_seatOrders[MAX_SEAT_ORDERS];
size_t    g_seatOrderCount = 0;
// And the pedestrians the mission tied to our player (SET_PLAYER_AS_LEADER).
int32_t   g_followers[MAX_SEAT_ORDERS];
size_t    g_followerCount = 0;

void ForgetSeatOrders() {
	ForgetGetaway();
	g_seatOrderCount      = 0;
	g_followerCount       = 0;
	g_saidInCarForAnybody = false;
	g_saidInCarAtPlace    = false;
	g_walkedOut           = standin::WalkedOut{};
	g_saidWalkedOut       = false;
	g_locationScript      = nullptr;
	g_carHold             = CarHold{};
	g_sparedCarCount      = 0;
	ForgetAnyplace();
}

void ForgetSeatOrder(size_t i) {
	g_seatOrders[i] = g_seatOrders[--g_seatOrderCount];
}

// Whether every passenger seat of `car` is taken here, and whether one of
// them by a player: another player's copy, or our own player riding beside
// somebody else at the wheel (mission.h, RiderInSeat).
void SeatsOf(void *car, bool &full, bool &playerRiding) {
	void *const me = Func<PlayerFn>(FindPlayerPed)();
	uint8_t max = Field<uint8_t>(car, offs::VEH_NUM_MAX_PASSENGERS);
	if (max > offs::VEH_MAX_PASSENGERS)
		max = static_cast<uint8_t>(offs::VEH_MAX_PASSENGERS);
	full         = Field<uint8_t>(car, offs::VEH_NUM_PASSENGERS) >= max;
	playerRiding = false;
	for (uint8_t seat = 0; seat < max; ++seat) {
		void *const in = Field<void *>(car, offs::VEH_PASSENGERS + seat * 4u);
		if (in && RiderInSeat(in == me, PlayerOfReplica(in), LocalId()) != INVALID_PLAYER)
			playerRiding = true;
	}
}

bool SeatedIn(void *ped, void *car) {
	return Field<uint8_t>(ped, offs::PED_IN_VEHICLE) != 0 && Field<void *>(ped, offs::PED_MY_VEHICLE) == car;
}

// SET_CHAR_OBJ_ENTER_CAR_AS_PASSENGER from our mission: remembered, one per
// pedestrian, the newest order standing.
int8_t PassengerOrder(void *script, int32_t command, RangeFn original) {
	const bool mission = g_client && g_own.running && IsMissionScript(script);
	if (mission)
		PeekParams(script, 2);
	const int32_t ped = mission ? reinterpret_cast<const int32_t *>(Params())[0] : -1;
	const int32_t car = mission ? reinterpret_cast<const int32_t *>(Params())[1] : -1;
	const int8_t  r   = original(script, nullptr, command);
	if (!mission)
		return r;
	size_t i = 0;
	while (i < g_seatOrderCount && g_seatOrders[i].ped != ped)
		++i;
	if (i == g_seatOrderCount) {
		if (g_seatOrderCount == MAX_SEAT_ORDERS)
			return r;
		++g_seatOrderCount;
	}
	g_seatOrders[i] = SeatOrder{ped, car, 0, false};
	return r;
}

// SET_PLAYER_AS_LEADER and CLEAR_LEADER from our mission.
int8_t LeaderOrder(void *script, int32_t command, RangeFn original) {
	const bool mission = g_client && g_own.running && IsMissionScript(script);
	if (mission)
		PeekParams(script, 1);
	const int32_t ped = mission ? reinterpret_cast<const int32_t *>(Params())[0] : -1;
	const int8_t  r   = original(script, nullptr, command);
	if (!mission)
		return r;
	size_t i = 0;
	while (i < g_followerCount && g_followers[i] != ped)
		++i;
	if (command == op::CLEAR_LEADER) {
		if (i < g_followerCount)
			g_followers[i] = g_followers[--g_followerCount];
	} else if (i == g_followerCount && g_followerCount < MAX_SEAT_ORDERS) {
		g_followers[g_followerCount++] = ped;
	}
	return r;
}

// 01DF for the guest's robbers: his copy is their leader, and the engine's own
// follower logic walks them to his car and into it.
int8_t GetawayLeader(void *script, int32_t command, RangeFn original) {
	void *const guest = GetawayGuestPed();
	if (!guest)
		return LeaderOrder(script, command, original);
	PeekParams(script, 1);
	const int32_t ped = reinterpret_cast<const int32_t *>(Params())[0];
	uint32_t      ip  = At<uint32_t>(script, layout::SCRIPT_IP);
	Func<CollectFn>(CTheScripts__CollectParameters)(script, &ip, 2);
	At<uint32_t>(script, layout::SCRIPT_IP) = ip;
	RunOurs(fuzzball::OP_SET_CHAR_AS_LEADER, {ped, PedRef(guest)});
	size_t i = 0;
	while (i < g_followerCount && g_followers[i] != ped)
		++i;
	if (i == g_followerCount && g_followerCount < MAX_SEAT_ORDERS)
		g_followers[g_followerCount++] = ped;
	return 0;
}

// Once a frame on the owner: which of the remembered orders the engine has
// dropped, and which of those can be given again.
void WatchSeatOrders(uint32_t nowMs) {
	for (size_t i = 0; i < g_seatOrderCount;) {
		SeatOrder  &o   = g_seatOrders[i];
		void *const ped = Func<GetPedFn>(CPools__GetPed)(o.ped);
		void *const car = Func<GetPedFn>(CPools__GetVehicle)(o.car);
		const bool  gone = !ped || !car || DyingOrDead(ped) || Wrecked(car);
		bool full = false, playerRiding = false;
		if (!gone)
			SeatsOf(car, full, playerRiding);
		const bool heading =
		    !gone && Field<uint32_t>(ped, offs::PED_OBJECTIVE) == OBJECTIVE_ENTER_CAR_AS_PASSENGER &&
		    Field<void *>(ped, offs::PED_CAR_IN_OBJECTIVE) == car;
		const SeatOrderStep step = NextSeatOrderStep(gone, !gone && SeatedIn(ped, car), heading, o.dropped,
		                                             full, playerRiding, nowMs - o.droppedMs);
		switch (step) {
		case SeatOrderStep::Forget:
			if (o.dropped && !gone && !SeatedIn(ped, car))
				Log("missions: %s's pedestrian %d never got a seat in car %d, which nobody left in %u s",
				    MissionName(g_own.number), o.ped, o.car, SEAT_ORDER_GIVE_UP_MS / 1000);
			ForgetSeatOrder(i);
			continue;
		case SeatOrderStep::Dropped:
			o.dropped   = true;
			o.droppedMs = nowMs;
			Log("missions: %s's pedestrian %d gave up on car %d, full with a player riding in it; "
			    "its seat is kept and the order stands",
			    MissionName(g_own.number), o.ped, o.car);
			break;
		case SeatOrderStep::Reissue:
			RunOurs(static_cast<uint16_t>(op::SET_CHAR_OBJ_ENTER_CAR_AS_PASSENGER), {o.ped, o.car});
			o.dropped = false;
			Log("missions: a seat in car %d is free again; %s's pedestrian %d is told into it again",
			    o.car, MissionName(g_own.number), o.ped);
			break;
		case SeatOrderStep::Keep:
			if (heading)
				o.dropped = false;
			break;
		}
		++i;
	}
	for (size_t i = 0; i < g_followerCount;) {
		void *const ped = Func<GetPedFn>(CPools__GetPed)(g_followers[i]);
		if (!ped || DyingOrDead(ped)) {
			g_followers[i] = g_followers[--g_followerCount];
			continue;
		}
		++i;
	}
}

// Each of our mission's pedestrians that ought to be on its way into a car
// and is not, with that car: a dropped order, and a follower of our player
// kept out of his car (mission.h, FollowerKeptOut).
template <class Fn>
void EachPassengerKeptOut(Fn fn) {
	for (size_t i = 0; i < g_seatOrderCount; ++i) {
		const SeatOrder &o = g_seatOrders[i];
		if (!o.dropped)
			continue;
		void *const ped = Func<GetPedFn>(CPools__GetPed)(o.ped);
		void *const car = Func<GetPedFn>(CPools__GetVehicle)(o.car);
		if (ped && car && !SeatedIn(ped, car))
			fn(ped, car);
	}
	// A pedestrian following a participant: the same, in the car his copy sits in
	// (The Getaway's robbers behind a guest's car, getaway.h).
	for (size_t i = 0; i < g_followerCount; ++i) {
		void *const ped = Func<GetPedFn>(CPools__GetPed)(g_followers[i]);
		if (!ped || DyingOrDead(ped) || Field<uint8_t>(ped, offs::PED_IN_VEHICLE) != 0)
			continue;
		void *const lead = Field<void *>(ped, fuzzball::PED_LEADER);
		void *const car  = lead && PlayerOfReplica(lead) != INVALID_PLAYER ? SeatedCar(lead) : nullptr;
		bool full = false, playerRiding = false;
		if (car) {
			SeatsOf(car, full, playerRiding);
			if (FollowerKeptOut(true, false, full, playerRiding))
				fn(ped, car);
		}
	}
	void *const me = Func<PlayerFn>(FindPlayerPed)();
	if (!me || Field<uint8_t>(me, offs::PED_IN_VEHICLE) == 0 ||
	    Field<uint32_t>(me, offs::PED_STATE) != PEDSTATE_DRIVING)
		return;
	void *const car = Field<void *>(me, offs::PED_MY_VEHICLE);
	if (!car)
		return;
	bool full = false, playerRiding = false;
	SeatsOf(car, full, playerRiding);
	for (size_t i = 0; i < g_followerCount; ++i) {
		void *const ped = Func<GetPedFn>(CPools__GetPed)(g_followers[i]);
		if (ped && !DyingOrDead(ped) && Field<uint8_t>(ped, offs::PED_IN_VEHICLE) == 0 &&
		    FollowerKeptOut(true, SeatedIn(ped, car), full, playerRiding))
			fn(ped, car);
	}
}

// The session's cars one of our mission's pedestrians is heading for the
// seats of (mission-audit.md R4): whoever is on the way into one as a
// passenger, SET_CHAR_OBJ_ENTER_CAR_AS_PASSENGER's objective, which the
// engine also gives whoever follows the player into a car. Nobody else sits
// down in it meanwhile, and when fewer of its passenger seats are free here
// than are needed, whoever rides in it gets out. A car the session does not
// name is nobody else's to sit in.
//
// Besides those, the ones the engine gave up on or never started (mission.h,
// NextSeatOrderStep and FollowerKeptOut): an order it dropped because a
// player's copy filled the car, and a pedestrian following our player who
// stays on foot because his car is full with a player riding in it.
void KeepMissionSeats(uint32_t nowMs) {
	struct Boarding {
		void   *car;
		uint8_t peds;
	};
	Boarding     boarding[MISSION_SEAT_CARS];
	size_t       cars = 0;
	void        *counted[64];
	size_t       countedN = 0;
	auto board = [&](void *ped, void *car) {
		for (size_t k = 0; k < countedN; ++k)
			if (counted[k] == ped)
				return;
		size_t at = 0;
		while (at < cars && boarding[at].car != car)
			++at;
		if (at == cars) {
			if (cars == MISSION_SEAT_CARS)
				return;
			boarding[cars++] = Boarding{car, 0};
		}
		++boarding[at].peds;
		if (countedN < sizeof counted / sizeof counted[0])
			counted[countedN++] = ped;
	};
	void        *peds[64];
	const size_t n    = HostedMissionPeds(peds, sizeof peds / sizeof peds[0]);
	for (size_t i = 0; i < n; ++i) {
		void *const ped = peds[i];
		if (Field<uint32_t>(ped, offs::PED_OBJECTIVE) != OBJECTIVE_ENTER_CAR_AS_PASSENGER)
			continue;
		void *const car = Field<void *>(ped, offs::PED_CAR_IN_OBJECTIVE);
		if (!car || (Field<uint8_t>(ped, offs::PED_IN_VEHICLE) != 0 &&
		             Field<void *>(ped, offs::PED_MY_VEHICLE) == car))
			continue;
		board(ped, car);
	}
	EachPassengerKeptOut([&](void *ped, void *car) { board(ped, car); });
	// Our own player riding beside a participant at the wheel is a rider like
	// any other: out of a seat the mission's pedestrian needs, or Salvatore's
	// Called A Meeting waits for ever on Toni at the restaurant.
	void *const me = Func<PlayerFn>(FindPlayerPed)();
	MissionSeatCar out[MISSION_SEAT_CARS];
	size_t         count = 0;
	for (size_t i = 0; i < cars; ++i) {
		void *const    car    = boarding[i].car;
		const int32_t  handle = Func<int32_t(__cdecl *)(void *)>(CPools__GetVehicleRef)(car);
		const uint16_t netId  = g_client->SessionCarNetIdOf(handle);
		if (netId == INVALID_NETID)
			continue;
		uint8_t max = Field<uint8_t>(car, offs::VEH_NUM_MAX_PASSENGERS);
		if (max > offs::VEH_MAX_PASSENGERS)
			max = static_cast<uint8_t>(offs::VEH_MAX_PASSENGERS);
		uint8_t taken = 0;
		uint8_t riders[offs::VEH_MAX_PASSENGERS];
		for (uint8_t seat = 0; seat < max; ++seat) {
			void *const in = Field<void *>(car, offs::VEH_PASSENGERS + seat * 4u);
			riders[seat]   = in ? RiderInSeat(in == me, PlayerOfReplica(in), LocalId()) : INVALID_PLAYER;
			if (in)
				++taken;
		}
		const uint8_t flags = SeatFlagsFor(boarding[i].peds, max, taken);
		const uint8_t leave = (flags & MISSION_SEAT_LEAVE) != 0
		                          ? LeaveMaskFor(boarding[i].peds, max, taken, riders, max)
		                          : 0;
		out[count++] = MissionSeatCar{netId, flags, leave};
		if (OwnerGivesUpSeat(leave, LocalId()) && me && SeatedIn(me, car) && UnseatLocalPlayer())
			Log("missions: our player rode in car %d beside a participant at the wheel, in the seat "
			    "%s's pedestrian needs; out, as a participant would be", handle,
			    MissionName(g_own.number));
	}
	g_client->Missions().SeatsNeeded(out, count, LocalId(), nowMs);
}

// How many of our mission's pedestrians are on their way into `car` as
// passengers, as KeepMissionSeats counts them.
uint8_t MissionPedsHeadingFor(void *car) {
	void        *peds[64];
	const size_t n     = HostedMissionPeds(peds, sizeof peds / sizeof peds[0]);
	uint8_t      count = 0;
	void        *counted[64];
	size_t       countedN = 0;
	for (size_t i = 0; i < n; ++i)
		if (Field<uint32_t>(peds[i], offs::PED_OBJECTIVE) == OBJECTIVE_ENTER_CAR_AS_PASSENGER &&
		    Field<void *>(peds[i], offs::PED_CAR_IN_OBJECTIVE) == car &&
		    !(Field<uint8_t>(peds[i], offs::PED_IN_VEHICLE) != 0 &&
		      Field<void *>(peds[i], offs::PED_MY_VEHICLE) == car)) {
			++count;
			if (countedN < sizeof counted / sizeof counted[0])
				counted[countedN++] = peds[i];
		}
	EachPassengerKeptOut([&](void *ped, void *to) {
		if (to != car)
			return;
		for (size_t k = 0; k < countedN; ++k)
			if (counted[k] == ped)
				return;
		++count;
	});
	return count;
}

bool IsBoat(void *car) {
	return car && Field<int32_t>(car, offs::VEH_TYPE) == VEHICLE_TYPE_BOAT;
}

// A ped in a boat: sitting in it, at its wheel or in its seat.
bool InBoat(void *ped) {
	return ped && Field<uint8_t>(ped, offs::PED_IN_VEHICLE) != 0 &&
	       IsBoat(Field<void *>(ped, offs::PED_MY_VEHICLE));
}

// ---- everybody into the car the mission put its player in (protocol.h, C_MissionBoard)
//
// Last Requests puts its player into the Reefer (39_frank4.sc:281), Chaperone
// into Maria's Stretch (35_frank1.sc:330) and Cipriani's Chauffeur into Toni's
// car (27_joey4.sc:198), all with WARP_PLAYER_INTO_CAR, and then drive off. As
// the owner, once the session names the car, the free passenger seats our
// engine has in it go to the participants on foot, nearest first, less one
// for each of our mission's pedestrians on the way into it. A participant in
// a car keeps his own and follows in it.
void BoardParticipants(uint32_t nowMs) {
	void *const   car   = Func<GetPedFn>(CPools__GetVehicle)(g_boardCar);
	void *const   me    = Func<PlayerFn>(FindPlayerPed)();
	const bool    in    = me && car && Field<bool>(me, offs::PED_IN_VEHICLE) &&
	                      Field<void *>(me, offs::PED_MY_VEHICLE) == car;
	const uint16_t netId = in ? g_client->SessionCarNetIdOf(g_boardCar) : INVALID_NETID;
	if (netId == INVALID_NETID) {
		if (!car || nowMs - g_boardSinceMs >= MISSION_BOARD_WAIT_MS) {
			Log("missions: our mission's warp put our player in a car the session never named "
			    "in %u ms, so nobody else was given a seat in it",
			    static_cast<unsigned>(MISSION_BOARD_WAIT_MS));
			g_boardCar = -1;
		}
		return;
	}
	g_boardCar = -1;

	const uint8_t participants = g_client->Missions().Participants();
	const float  *at           = &Field<float>(car, offs::POSITION);
	uint8_t       order[MAX_PLAYERS];
	float         dist[MAX_PLAYERS];
	size_t        n = 0;
	for (uint8_t id = 0; id < MAX_PLAYERS; ++id) {
		if (id == LocalId() || (participants & PlayerBit(id)) == 0)
			continue;
		void *const ped = ReplicaOf(id);
		if (ped && Field<uint8_t>(ped, offs::PED_IN_VEHICLE) != 0)
			continue;   // in a car of his own, or in this one already
		float d = 1.0e9f;   // nowhere near: last
		if (ped) {
			const float *p  = &Field<float>(ped, offs::POSITION);
			const float  dx = p[0] - at[0], dy = p[1] - at[1], dz = p[2] - at[2];
			d               = dx * dx + dy * dy + dz * dz;
		}
		size_t k = n++;
		for (; k > 0 && dist[k - 1] > d; --k) {
			order[k] = order[k - 1];
			dist[k]  = dist[k - 1];
		}
		order[k] = id;
		dist[k]  = d;
	}
	if (n == 0)
		return;
	uint8_t seats[MAX_PLAYERS] = {};
	AssignBoardingSeats(FreeSeatsForWarp(car), MissionPedsHeadingFor(car), order, n, seats);
	g_client->Missions().Board(netId, seats, IsBoat(car) ? MISSION_BOARD_WATER : 0, LocalId(),
	                           nowMs);
}

// As a participant: the seat the owner handed us, straight into it, the way
// the owner's own player went in. On foot only; a player already in a car, or
// halfway into one, keeps it. The car can take a moment to be built here.
void TakeBoardSeat(uint32_t nowMs) {
	uint16_t netId = INVALID_NETID, others = 0;
	uint8_t  seat  = 0;
	uint32_t since = 0;
	if (!g_client->Missions().BoardPending(netId, seat, others, since))
		return;
	void *const me = Func<PlayerFn>(FindPlayerPed)();
	if (!me)
		return;
	if (Field<bool>(me, offs::PED_IN_VEHICLE) || EnteringACar(me)) {
		g_client->Missions().BoardDone();
		Log("missions: the owner's mission handed us seat %u in vehicle %u; we are in a car "
		    "already and keep it",
		    seat, netId);
		return;
	}
	const int32_t handle = g_client->SessionCarHandleOf(netId);
	if (handle < 0) {
		if (nowMs - since >= MISSION_BOARD_WAIT_MS) {
			g_client->Missions().BoardDone();
			Log("missions: the owner's mission handed us seat %u in vehicle %u, which was never "
			    "built here",
			    seat, netId);
		}
		return;
	}
	g_client->Missions().BoardDone();
	const int32_t got = WarpLocalPlayerIntoGivenSeat(handle, seat, others);
	if (got > 0) {
		g_client->NoteScriptedRide(netId, nowMs);
		Log("missions: put in seat %d of vehicle %u, where the owner's mission put its player",
		    got, netId);
	} else {
		Log("missions: the owner's mission handed us seat %u in vehicle %u and we couldn't be "
		    "seated in it here; we stay on foot",
		    seat, netId);
	}
}

// As the owner, every few frames: our player on the water, and who cannot
// follow him there (CheckpointOutOfReach).
void WatchOutOfReach() {
	void *const   me      = Func<PlayerFn>(FindPlayerPed)();
	const bool    onWater = InBoat(me);
	const uint8_t in      = g_client->Missions().Participants();
	uint8_t       mask    = 0;
	for (uint8_t id = 0; onWater && id < MAX_PLAYERS; ++id)
		if (id != LocalId() && (in & PlayerBit(id)) != 0 &&
		    CheckpointOutOfReach(onWater, InBoat(ReplicaOf(id))))
			mask = static_cast<uint8_t>(mask | PlayerBit(id));
	g_client->Missions().SetOutOfReach(mask, LocalId());
}

// ---- a game of its own, or started over (missions.md 11.6) ------------------------

// A mission script of this machine's runs: the session's, when this machine
// owns it, or one of the game's own, the intro of a new game, or one it
// launched that the session does not know.
bool MissionScriptRunning() { return AnyMissionSlotScript(Global<uintptr_t>(ACTIVE_SCRIPTS)); }

// Everything the owner's mission put up here, let go of without a word to
// the engine: the game it was put up in is gone, and its handles name
// nothing now, or somebody else's things.
void ForgetEffects() {
	g_blipMap.Clear();
	g_objectMap.Clear();
	g_pickupMap.Clear();
	g_fireMap.Clear();
	g_sphereMap.Clear();
	g_madeObjectCount = 0;
	g_stashCount      = 0;
	g_floatingCount   = 0;
	g_shown           = Shown{};
	g_shownMarkers.Clear();
	g_shownCoronas.Clear();
	// The densities are no save's, and a new game or a load keeps whatever
	// they were; the zones and gangs came back with the game that loaded.
	RestoreWorld(false);
	g_planesWatched   = 0;
	// Ours, not the engine's: the others are drawn again, whatever scene of
	// the owner's they were hidden for.
	SetCutsceneScene(false);
}

// The owner's mission went without MISSION_HAS_FINISHED: a load or a new
// game took its script with it. It failed, as far as the session is
// concerned, and leaves nothing behind in the campaign: the game it changed
// is not the one running now.
void OwnMissionVanished(uint32_t nowMs) {
	Log("missions: %s went without finishing, a load or a new game, and fails for everybody",
	    MissionName(g_own.number));
	g_waitingCount = 0;
	g_standing.Clear();
	g_groups.Clear();
	g_aliases.Clear();
	g_shaper.Clear();
	g_ownCoronas.Clear();
	ForgetEffects();
	g_client->Missions().Ended(g_own.number, MISSION_OUTCOME_FAILED, nowMs);
	// What the mission made that is still here goes out as ordinary crowd, and
	// the session takes the rest away (game/missionclear.h).
	NoteOwnMissionOver(nowMs);
	// Its cars are the old game's handles too.
	g_missionCarCount = 0;
	g_getBackIn.Clear();
	ForgetSeatOrders();
	g_ownMarkers.Clear();
	g_own            = OwnMission{};
	g_blipCount      = 0;
	g_madeBlipCount  = 0;
	g_madePickupCount = 0;
	g_ownTimer       = 0;
	g_ownCounter     = 0;
	g_ownTimerFrozen = false;
	SetCutsceneScene(false);
}

// CTimer::m_FrameCounter as last seen. CTimer::Initialise zeroes it, which a
// new game and a load both run (game/frame.cpp), so it going back is this
// machine's game starting over. The game clock is no such witness: a long
// hitch jumps it too, and a start-over that was not one would put the
// owner's blips up here twice.
uint32_t g_lastFrameCounter = 0;

void WatchOwnGame(uint32_t nowMs) {
	const uint32_t frames    = Global<uint32_t>(CTimer__m_FrameCounter);
	const bool     restarted = frames < g_lastFrameCounter;
	g_lastFrameCounter       = frames;
	const bool script        = MissionScriptRunning();
	// A script gone after its cleanup ended the way it meant to, whether or
	// not its TERMINATE_THIS_SCRIPT reached us.
	if (g_own.running && g_own.finishing && !restarted && !script)
		EndOwnMission(nowMs);
	if (g_own.running && (restarted || !script))
		OwnMissionVanished(nowMs);
	if (restarted)
		g_leftCarCount = 0;
	const bool sessions = g_own.running && g_client->Missions().OwnLaunchIsTheSessions(LocalId());
	const bool wasBusy  = g_client->Missions().Busy();
	const bool busy     = script && !sessions;
	g_client->Missions().SetBusy(busy, nowMs);
	// Out of the intro into somebody else's running mission, which hands over
	// no fade: nothing would take the intro's last one off.
	if (wasBusy && !busy && g_client->Missions().Running() &&
	    g_client->Missions().Owner() != LocalId())
		g_showWorld = true;
	if (restarted) {
		ForgetEffects();
		g_client->Missions().StartedOver(LocalId(), nowMs);
	}
}

// The world, while this machine's player waits for the session: the screen
// faded in, if it was left faded out (missionaddr.h, CAMERA_FADE). A fade in
// progress, or a screen already clear, is left as it is. Out here, and never
// from inside a script's instruction, since running DO_FADE takes over
// ScriptParams.
void ShowWorldWhileWaiting() {
	if (!g_showWorld)
		return;
	g_showWorld = false;
	const uint8_t *camera = Ptr<uint8_t>(TheCamera);
	float          fade   = 0.0f;
	std::memcpy(&fade, camera + CAMERA_FADE, sizeof fade);
	if (camera[CAMERA_FADING] != 0 || fade < CAMERA_FADED)
		return;
	RunOurs(op::DO_FADE, {500, 1});
	Log("missions: the session keeps us waiting and the screen was left faded out; faded it in");
}

// A participant's own skip button does nothing (missions.md 11.3).
// CCutsceneMgr::Update skips on Cross, a left click, Return, the keypad's
// Enter or the space bar going down this frame: the state now against the
// state last frame. Before CGame::Process, each is made to look held since
// the last frame, in both: CPad::UpdatePads, which runs next, copies the one
// into the other, and whatever it leaves uncopied is held in both already. A
// button that is held means the same everywhere else whether it was or not,
// and on the next frame the real states are back.
void HoldSkipButtons() {
	uint8_t *const pad0 = Ptr<uint8_t>(CPad__Pads);
	for (size_t state : {pad::NEWSTATE, pad::OLDSTATE})
		*reinterpret_cast<int16_t *>(pad0 + state + pad::CROSS * 2) = 255;
	for (uintptr_t mouse : {CPad__NewMouseControllerState, CPad__OldMouseControllerState})
		Ptr<uint8_t>(mouse)[pad::MOUSE_LMB] = 1;
	for (uintptr_t keys : {CPad__NewKeyState, CPad__OldKeyState}) {
		uint8_t *const k = Ptr<uint8_t>(keys);
		for (size_t key : {pad::KEY_ENTER, pad::KEY_EXTENTER, pad::KEY_VK_KEYS + ' ' * 2})
			*reinterpret_cast<int16_t *>(k + key) = 255;
	}
}

void EndEffects() {
	g_planesWatched = 0;
	// Our Catalina, which the owner's cleanup takes away itself when it gets
	// that far, and the owner's pills, which only we draw here.
	if (g_shown.catalina)
		RunOurs(static_cast<uint16_t>(OP_REMOVE_CATALINA_HELI), {});
	g_shown.catalina     = false;
	g_shown.catalinaPath = 0;
	if (g_shown.pills)
		RunOurs(static_cast<uint16_t>(OP_CLEAR_PACMAN), {});
	g_shown.pills = false;
	// Whatever blips the owner's mission never took off again, the way a
	// mission's own cleanup would have.
	for (size_t i = 0; i < g_blipMap.Count(); ++i)
		RunOurs(op::REMOVE_BLIP, {g_blipMap.OursAt(i)});
	g_blipMap.Clear();
	// And its blue markers and coronas, which nothing but us draws here.
	g_shownMarkers.Clear();
	g_shownCoronas.Clear();
	// Its spheres on the ground.
	for (size_t i = 0; i < g_sphereMap.Count(); ++i)
		RunOurs(world::op::REMOVE_SPHERE, {g_sphereMap.OursAt(i)});
	g_sphereMap.Clear();
	// Its objects become the engine's to tidy away, as the mission's own
	// cleanup makes them on the owner's machine.
	for (size_t i = 0; i < g_madeObjectCount; ++i)
		if (Func<GetPedFn>(CPools__GetObject)(g_madeObjects[i]) != nullptr)
			RunOurs(op::MARK_OBJECT_AS_NO_LONGER_NEEDED, {g_madeObjects[i]});
	g_madeObjectCount = 0;
	// Its fires go out.
	for (size_t i = 0; i < g_fireMap.Count(); ++i)
		RunOurs(op::REMOVE_SCRIPT_FIRE, {g_fireMap.OursAt(i)});
	g_fireMap.Clear();
	// And its pickups, the stash that nobody took with them.
	for (size_t i = 0; i < g_pickupMap.Count(); ++i)
		RunOurs(op::REMOVE_PICKUP, {g_pickupMap.OursAt(i)});
	g_pickupMap.Clear();
	g_stashCount    = 0;
	g_floatingCount = 0;
	// And its garages stop waiting for one of our cars. The owner's cleanup
	// does this with a SET_TARGET_CAR_FOR_MISSION_GARAGE of -1, which now
	// reaches us (replay.h, CarMayBeNone); a mission that ended any other way
	// never sends it, and the garage would go on holding a pointer to a copy
	// the session can delete at any time (game/teardown.h).
	for (size_t g = 0; g < NUM_GARAGES; ++g)
		if (g_shown.garageTargets & (1u << g))
			Func<void(__cdecl *)(int32_t, void *)>(CGarages__SetTargetCarForMissonGarage)(
			    static_cast<int32_t>(g), nullptr);
	// And whatever it left of a cutscene, a fade, the camera, the controls or
	// the HUD: nothing is left for this machine's player to be stuck in.
	if (g_shown.cutscene)
		RunOurs(op::CLEAR_CUTSCENE, {});
	g_objectMap.Clear();
	SetCutsceneScene(false);
	if (g_shown.fixedCamera)
		RunOurs(op::RESTORE_CAMERA_JUMPCUT, {});
	if (g_shown.widescreen)
		RunOurs(op::SWITCH_WIDESCREEN, {0});
	if (g_shown.flashing)
		RunOurs(op::FLASH_HUD_OBJECT, {HUD_ITEM_NONE});
	if (g_shown.controlOff)
		RunOurs(op::SET_PLAYER_CONTROL, {0, 1});
	if (g_shown.fadedOut)
		RunOurs(op::DO_FADE, {0, 1});
	if (g_shown.invisible)
		RunOurs(op::SET_PLAYER_VISIBLE, {0, 1});
	if (g_shown.ignoredByAll)
		RunOurs(op::SET_EVERYONE_IGNORE_PLAYER, {0, 0});
	if (g_shown.ignoredByCops)
		RunOurs(op::SET_POLICE_IGNORE_PLAYER, {0, 0});
	if (g_shown.brakes)
		RunOurs(op::APPLY_BRAKES_TO_PLAYERS_CAR, {0, 0});
	if (g_shown.freeBombs)
		RunOurs(op::SET_FREE_BOMB_SHOP, {0});
	if (g_shown.restartMoved)
		RunOurs(op::CANCEL_OVERRIDE_RESTART, {});
	if (g_shown.timer != 0)
		ClearWidget(op::CLEAR_ONSCREEN_TIMER, g_shown.timer);
	if (g_shown.counter != 0)
		ClearWidget(op::CLEAR_ONSCREEN_COUNTER, g_shown.counter);
	if (g_shown.missionAudio)
		RunOurs(world::op::CLEAR_MISSION_AUDIO, {});
	if (g_shown.credits)
		RunOurs(world::op::STOP_CREDITS, {});
	// What the mission set for a while. The launch's make-safe takes the
	// controls with the pad's CUTSCENE bit and starts cutscene processing,
	// which only a cutscene's teardown gives back: with no cutscene since,
	// the controls would stay off (the make-safe itself went with
	// SET_PLAYER_CONTROL above).
	if (g_shown.madeSafe) {
		Ptr<uint8_t>(CPad__Pads)[pad::DISABLE_PLAYER_CONTROLS] &=
		    static_cast<uint8_t>(~pad::PLAYERCONTROL_CUTSCENE);
		Global<uint8_t>(CCutsceneMgr__ms_cutsceneProcessing) = 0;
	}
	if (g_shown.freeResprays)
		RunOurs(op::SET_FREE_RESPRAYS, {0});
	if (g_shown.crimeEye) {
		const float normal = 1.0f;
		int32_t     bits   = 0;
		std::memcpy(&bits, &normal, 4);
		RunOurs(op::SET_WANTED_MULTIPLIER, {bits});
	}
	// A restart level the mission's failure set is for the death or the
	// arrest that failed it: kept while this machine's player is on the way
	// to that restart, which uses it up, and given back otherwise.
	const uint8_t wb = *Ptr<uint8_t>(CWorld__Players + offs::PLAYERINFO_WB_STATE);
	if (wb == WBSTATE_PLAYING) {
		if (g_shown.hospital)
			RunOurs(op::OVERRIDE_HOSPITAL_LEVEL, {static_cast<int32_t>(game::LEVEL_GENERIC)});
		if (g_shown.policeStation)
			RunOurs(op::OVERRIDE_POLICE_STATION_LEVEL, {static_cast<int32_t>(game::LEVEL_GENERIC)});
	}
	if (g_shown.worldHeld)
		RunOurs(op::SWITCH_WORLD_PROCESSING, {1});
	if (g_shown.carsUnhurt)
		RunOurs(op::SET_ALL_CARS_CAN_BE_DAMAGED, {1});
	if (g_shown.carsAtCamera)
		RunOurs(op::SET_GENERATE_CARS_AROUND_CAMERA, {0});
	if (g_shown.nearClip) {
		int32_t bits = 0;
		std::memcpy(&bits, &NEAR_CLIP_AFTER_MISSION, 4);
		RunOurs(op::SET_NEAR_CLIP, {bits});
	}
	if (g_shown.musicNoFade)
		RunOurs(op::SET_MUSIC_DOES_FADE, {1});
	if (g_shown.endTune)
		RunOurs(op::STOP_END_OF_GAME_TUNE, {});
	// And the streets: a mission that ended the way it meant to put its zones
	// and gangs back itself, or left them changed for good, and we ran that too.
	const uint8_t outcome = g_client ? g_client->Missions().LastOutcome() : MISSION_OUTCOME_NONE;
	const bool cleanupMissed = outcome != MISSION_OUTCOME_PASSED && outcome != MISSION_OUTCOME_FAILED;
	RestoreWorld(cleanupMissed);
	// And its sounds, which a mission that ended the way it meant took off
	// itself or meant to leave playing.
	for (uint8_t i = 0; cleanupMissed && i < g_shown.soundCount; ++i)
		if (SoundHandleInPool(g_shown.sounds[i])) {
			const uint16_t g         = g_shown.sounds[i];
			const uint8_t  remove[5] = {static_cast<uint8_t>(op::REMOVE_SOUND & 0xFF),
			                            static_cast<uint8_t>(op::REMOVE_SOUND >> 8), PARAM_GLOBAL,
			                            static_cast<uint8_t>(g & 0xFF), static_cast<uint8_t>(g >> 8)};
			RunHere(remove, sizeof remove, nullptr);
		}
	g_shown = Shown{};
}

} // namespace

bool MissionMakingEntities() { return g_missionInstruction; }

uint32_t MissionThreat(void *ped, uint32_t found) {
	// Asked for every pedestrian that looks around, every frame: the cheap
	// questions first.
	if (!g_installed || !g_client || !g_own.running || !ped)
		return found;
	// PEDTYPE_PLAYER1's own flag, the one THREAT_PLAYER1 sets.
	const uintptr_t playerType = Global<uintptr_t>(CPedType__ms_apPedType);
	const uint32_t  playerFlag =
	    playerType ? *reinterpret_cast<const uint32_t *>(playerType + offs::PEDTYPE_FLAG) : 0;
	if (playerFlag == 0 || (Field<uint32_t>(ped, offs::PED_FEAR_FLAGS) & playerFlag) == 0)
		return found;
	// A gun, an explosion or somebody else stays what the engine found.
	if ((found != 0 && found != playerFlag) || !HostedMissionEntity(ped))
		return found;
	void **const threat = &Field<void *>(ped, offs::PED_THREAT_ENTITY);
	float        best   = found != 0 && *threat ? DistanceSq(ped, *threat) : 60.0f * 60.0f;
	void        *nearer = nullptr;
	for (uint8_t id = 0; id < MAX_PLAYERS; ++id) {
		void *const replica = ReplicaOf(id);
		if (!replica || Field<float>(replica, offs::PED_HEALTH) <= 0.0f)
			continue;
		const float d = DistanceSq(ped, replica);
		if (d < best && CanSee(ped, replica)) {
			best   = d;
			nearer = replica;
		}
	}
	if (!nearer)
		return found;
	// Held the way the engine holds what it found: a registered reference,
	// which it nils if the replica goes.
	*threat = nearer;
	Func<void(__thiscall *)(void *, void **)>(CEntity__RegisterReference)(nearer, threat);
	static bool said = false;
	if (!said) {
		said = true;
		Log("missions: one of %s's pedestrians that fears the player saw a participant first",
		    MissionName(g_own.number));
	}
	return playerFlag;
}

bool MissionStashAt(float x, float y, int16_t model) {
	for (size_t i = 0; i < g_stashCount; ++i) {
		const StashPickup &s = g_stash[i];
		const float        dx = s.x - x, dy = s.y - y;
		if (dx * dx + dy * dy < 0.25f * 0.25f && (s.model == -1 || s.model == model))
			return true;
	}
	return false;
}
bool OwnMissionRunning() { return g_installed && g_own.running; }

bool MissionParticipantEntity(const void *entity) {
	if (!entity || !g_installed || !g_client || !g_own.running)
		return false;
	for (uint8_t id = 0; id < MAX_PLAYERS; ++id) {
		void *const ped = id != LocalId() && InOwnMission(id) ? ReplicaOf(id) : nullptr;
		if (!ped)
			continue;
		if (ped == entity ||
		    (Field<uint8_t>(ped, offs::PED_IN_VEHICLE) != 0 &&
		     Field<void *>(ped, offs::PED_MY_VEHICLE) == entity))
			return true;
	}
	return false;
}

bool RemotePlayerEntity(const void *entity) {
	return RemotePlayerOfEntity(entity) != INVALID_PLAYER;
}

uint8_t RemotePlayerOfEntity(const void *entity) {
	if (!entity || !g_installed || !g_client)
		return INVALID_PLAYER;
	for (uint8_t id = 0; id < MAX_PLAYERS; ++id) {
		void *const ped = ReplicaOf(id);
		if (!ped)
			continue;
		if (ped == entity ||
		    (Field<uint8_t>(ped, offs::PED_IN_VEHICLE) != 0 &&
		     Field<void *>(ped, offs::PED_MY_VEHICLE) == entity))
			return id;
	}
	return INVALID_PLAYER;
}

bool MissionCreditsRemoteKills() {
	return g_installed && g_client && g_client->Missions().CreditsRemoteKills(LocalId());
}

void MissionKillRegistered(void *victim) {
	if (!g_installed || !g_client || !victim)
		return;
	const int16_t model = Field<int16_t>(victim, offs::MODEL_INDEX);
	if (model >= 0)
		g_client->Missions().KillRegistered(static_cast<uint16_t>(model), LocalId(),
		                                    WallClock::NowMs());
}

bool InstallMissionHooks(bool enabled, Client &client) {
	if (!enabled) {
		Log("missions: off. Every mission is this machine's own; `missions = on` in "
		    "CoopIII.ini shares them (docs/missions.md)");
		return false;
	}
	if (g_installed)
		return true;
	bool usable[RANGE_COUNT] = {};
	for (size_t i = 0; i < RANGE_COUNT; ++i) {
		const RangeHook &h = g_hooks[i];
		usable[i]          = DispatcherCalls(h.target) && LooksLikeRangeHandler(h.target);
		if (!usable[i])
			Log("missions: %s at 0x%08X is not one of the dispatcher's range handlers in this "
			    "image%s", h.name, static_cast<unsigned>(h.target),
			    h.required ? "" : " - what it would have shown is not shared");
		if (!usable[i] && h.required) {
			Log("missions: without it the session's mission cannot work, so nothing is hooked "
			    "and every mission stays this machine's own");
			return false;
		}
	}
	size_t hooked = 0;
	for (size_t i = 0; i < RANGE_COUNT; ++i) {
		if (!usable[i])
			continue;
		if (g_hooks[i].detour.Install(g_hooks[i].name, reinterpret_cast<void *>(g_hooks[i].target),
		                              kReplacements[i])) {
			++hooked;
			continue;
		}
		if (g_hooks[i].required) {
			Log("missions: %s would not hook - the rest are taken off again", g_hooks[i].name);
			for (RangeHook &h : g_hooks)
				h.detour.Remove();
			return false;
		}
		Log("missions: %s would not hook - what it would have shown is not shared", g_hooks[i].name);
	}
	InstallHighlight();
	InstallPillCall();
	g_client    = &client;
	g_installed = true;
	SetMissionObjectBroken(&MissionObjectBroken);
	Log("missions: on. %u of %u script range handlers hooked; the session's mission is shared",
	    static_cast<unsigned>(hooked), static_cast<unsigned>(RANGE_COUNT));
	return true;
}

void RemoveMissionHooks() {
	SetMissionObjectBroken(nullptr);
	for (RangeHook &h : g_hooks)
		h.detour.Remove();
	g_highlight.Remove();
	g_highlightUsable = false;
	RemovePillCall();
	g_ownMarkers.Clear();
	g_shownMarkers.Clear();
	g_pedGetAt.Remove();
	g_pedGetAtTried = false;
	g_groups.Clear();
	if (g_mirror)
		SetOnMission(false);
	g_blipMap.Clear();
	g_installed = false;
	g_client    = nullptr;
}

namespace {

// The Import/Export boards are sixteen cars each, the crane's list seven.
constexpr uint32_t GARAGE_LIST_BITS = 0xFFFFu;

bool ReadCarLists(uint32_t (&out)[CAR_LISTS]) {
	if (!Func<PlayerFn>(FindPlayerPed)())
		return false;   // no game running yet, or between two
	const uint32_t *garages = Ptr<uint32_t>(CGarages__CarTypesCollected);
	for (uint8_t i = 0; i < CAR_LIST_GARAGES; ++i)
		out[i] = garages[i] & GARAGE_LIST_BITS;
	out[CAR_LIST_CRANE] = Global<uint32_t>(CCranes__CarsCollectedMilitaryCrane) & MILITARY_CRANE_ALL_CARS;
	return true;
}

// Only ever more bits: what this game has stays, and nothing is paid.
void WriteCarLists(const uint32_t (&in)[CAR_LISTS]) {
	uint32_t *garages = Ptr<uint32_t>(CGarages__CarTypesCollected);
	for (uint8_t i = 0; i < CAR_LIST_GARAGES; ++i)
		garages[i] |= in[i] & GARAGE_LIST_BITS;
	Global<uint32_t>(CCranes__CarsCollectedMilitaryCrane) |= in[CAR_LIST_CRANE] & MILITARY_CRANE_ALL_CARS;
}

} // namespace

void AddMissionsToBridge(WorldBridge &bridge) {
	if (!g_installed)
		return;
	bridge.missions.ReadCarLists  = &ReadCarLists;
	bridge.missions.WriteCarLists = &WriteCarLists;
	bridge.missions.SetOnMission = &SetOnMission;
	bridge.missions.FailMission  = &FailMission;
	bridge.missions.RunEffect     = &RunEffect;
	bridge.missions.EffectAwaits  = &EffectAwaits;
	bridge.missions.EndEffects    = &EndEffects;
	bridge.missions.ApplyCampaign = &ApplyCampaign;
	bridge.missions.ScriptLife    = &ScriptLife;
	bridge.missions.ScriptHash    = &ScriptHash;
	bridge.missions.ReadGlobal    = &ReadGlobal;
	bridge.missions.Teleport      = &Teleport;
	bridge.missions.SetWidget     = &SetWidget;
	bridge.missions.ResendStanding = &ResendStanding;
	bridge.missions.BreakObject    = &BreakObject;
	bridge.missions.TakePickup     = &TakePickup;
	bridge.missions.CountKill      = &CountKill;
	bridge.missions.ReadContactMarkers = &ReadContactMarkers;
	bridge.missions.ReadPlaceBlips     = &ReadPlaceBlips;
}

namespace {

// The clothes the owner's mission put its player in, on ours: taken apart,
// the model loaded at once the way LOAD_ALL_MODELS_NOW loads a cutscene's,
// and built again, by the engine's own UNDRESS_CHAR and DRESS_CHAR. A model
// that is not in by the end of that waits a frame at a time, as the script
// itself waits, with the ped out of the world until it is.
OutfitInputs ReadOutfitInputs(void *ped) {
	OutfitInputs in;
	in.havePed = ped != nullptr;
	if (ped) {
		in.inVehicle = Field<bool>(ped, offs::PED_IN_VEHICLE);
		in.pedState  = Field<uint32_t>(ped, offs::PED_STATE);
		if (void *const car = Field<void *>(ped, offs::PED_MY_VEHICLE)) {
			in.haveCar = true;
			in.carType = Field<int32_t>(car, offs::VEH_TYPE);
			in.driver  = Field<void *>(car, offs::VEH_DRIVER) == ped;
			for (size_t seat = 0; seat < offs::VEH_MAX_PASSENGERS && !in.driver; ++seat)
				if (Field<void *>(car, offs::VEH_PASSENGERS + seat * 4) == ped)
					in.passenger = true;
			in.lowCar = (Field<uint8_t>(car, offs::VEH_FLAGS_B_BUS) & offs::VEH_IS_LOW) != 0;
		}
	}
	in.model0       = LocalPlayerModelName();
	in.model0Loaded = HasModelLoaded(MI_PLAYER);
	return in;
}

// A ped built again in a car seat is still in it and stands there: sat back
// down the way PedSetInCarCB sits it (addresses.h, "a seated ped built
// again"), its sitting animation the one m_pVehicleAnim holds.
void SitBackDown(void *ped, const OutfitInputs &in) {
	void *const clump = Field<void *>(ped, offs::RW_OBJECT);
	if (!clump)
		return;
	using BlendFn = void *(__cdecl *)(void *, int, int, float);
	Field<void *>(ped, offs::PED_VEHICLE_ANIM) = Func<BlendFn>(CAnimManager__BlendAnimation)(
	    clump, ASSOCGRP_STD, OutfitSitAnim(in), CAR_SIT_BLEND_DELTA);
	Func<void(__thiscall *)(void *)>(CPed__StopNonPartialAnims)(ped);
}

void TickOutfit() {
	if (!g_outfit.Pending())
		return;
	void *const        ped           = Func<PlayerFn>(FindPlayerPed)();
	const OutfitInputs in            = ReadOutfitInputs(ped);
	const bool         wasUndressing = g_outfit.Undressing();
	const OutfitStep   step          = g_outfit.Next(in);
	if (step == OutfitStep::Nothing) {
		if (wasUndressing && !g_outfit.Pending())
			Log("missions: our player went while changing into '%s'; nothing is left to dress",
			    g_outfit.Look());
		else if (g_outfit.Pending() && !wasUndressing && in.inVehicle &&
		         in.pedState == PEDSTATE_DRIVING && !OutfitSeated(in) && !g_saidOutfitSeat) {
			g_saidOutfitSeat = true;
			Log("missions: our player sits in a seat the change can't sit them back down in (car "
			    "%s, type %d, %s); it waits for them to get out",
			    in.haveCar ? "known" : "none", static_cast<int>(in.carType),
			    in.driver ? "driving" : in.passenger ? "a passenger" : "in none of its seats");
		}
		return;
	}
	const int32_t ref = Func<int32_t(__cdecl *)(void *)>(CPools__GetPedRef)(ped);
	if (step == OutfitStep::Undress) {
		uint8_t label[TEXT_LABEL];
		if (!OutfitLabel(g_outfit.Look(), label)) {
			g_outfit.Clear();
			return;
		}
		uint8_t code[2 + 5 + TEXT_LABEL] = {static_cast<uint8_t>(op::UNDRESS_CHAR & 0xFF),
		                                    static_cast<uint8_t>(op::UNDRESS_CHAR >> 8),
		                                    PARAM_INT32};
		std::memcpy(code + 3, &ref, 4);
		std::memcpy(code + 7, label, TEXT_LABEL);
		Log("missions: changing our player from '%s' into '%s'%s, as the mission changed its own",
		    in.model0 ? in.model0 : "?", g_outfit.Look(), in.inVehicle ? " in their car seat" : "");
		if (!RunHere(code, sizeof code, nullptr)) {
			g_outfit.Clear();
			return;
		}
		// The association it named went with the clump.
		Field<void *>(ped, offs::PED_VEHICLE_ANIM) = nullptr;
		g_outfit.Undressed();
		RunOurs(static_cast<uint16_t>(op::LOAD_ALL_MODELS_NOW), {});
		if (!HasModelLoaded(MI_PLAYER)) {
			Log("missions: '%s' is not in yet; our player waits for it out of the world",
			    g_outfit.Look());
			return;
		}
	}
	RunOurs(static_cast<uint16_t>(op::DRESS_CHAR), {ref});
	g_outfit.Dressed();
	const OutfitInputs now = ReadOutfitInputs(ped);
	if (OutfitSeated(now))
		SitBackDown(ped, now);
	Log("missions: our player is in '%s' now%s", g_outfit.Look(),
	    OutfitSeated(now) ? ", sat back down in their seat" : "");
}

bool g_saidBringWaits = false;

// A participant who came into the running mission late, or back from the
// hospital, brought beside its owner (missionsync.h, MISSION_SUMMON_*): out of
// any car and on foot, and never while dead, arrested or in a cutscene, the
// move the rampage vote makes (rampagevote.h, MovePlayerBeside).
void BringToOwner(uint32_t nowMs) {
	MissionSync &m = g_client->Missions();
	// A mission whose checkpoints do not wait brings along whoever falls far
	// behind its owner (MISSION_BEHIND_*).
	if (!m.SummonPending() && m.ParticipantHere(LocalId()) && !g_shown.cutscene) {
		void *const me = Func<PlayerFn>(FindPlayerPed)();
		if (me) {
			const float *at = &Field<float>(me, offs::POSITION);
			m.WatchBehind(Vec3{at[0], at[1], at[2]}, LocalId(), nowMs);
		}
	}
	if (!m.SummonPending() || g_shown.cutscene || !MayMovePlayer())
		return;
	void *const ped = Func<PlayerFn>(FindPlayerPed)();
	if (!ped)
		return;
	// Not beside a car the owner is driving (BringWaitsForOwnersCar): the move
	// stays owed until it slows. Where the owner sits in it is no place to be
	// put down either way; the move itself keeps off it (rampagevote.h,
	// MoveKeepOut).
	void *const ownerCar = CarPlayerSitsIn(m.Owner());
	const float carSpeed = CarSpeedMps(ownerCar);
	if (BringWaitsForOwnersCar(ownerCar != nullptr, carSpeed)) {
		if (!g_saidBringWaits) {
			g_saidBringWaits = true;
			const int32_t  handle = Func<int32_t(__cdecl *)(void *)>(CPools__GetVehicleRef)(ownerCar);
			const uint16_t netId  = g_client->SessionCarNetIdOf(handle);
			Log("missions: owed a move beside %s, who is in vehicle %u doing %.1f m/s; it waits "
			    "for the car to slow rather than put us down in its road",
			    g_client->NickFor(m.Owner()) ? g_client->NickFor(m.Owner()) : "?",
			    static_cast<unsigned>(netId), carSpeed);
		}
		return;
	}
	const float *p = &Field<float>(ped, offs::POSITION);
	Vec3         owner{};
	uint8_t      slot = 0, count = 1;
	if (!m.TakeSummon(Vec3{p[0], p[1], p[2]}, LocalId(), nowMs, &owner, &slot, &count))
		return;
	if (!MovePlayerBeside(owner, m.Owner(), slot, count))
		Log("missions: could not bring our player to the owner after all");
	else
		NoteStandPlacement("being brought to the owner");
}

// The pause menu, shut when the session's mission starts for this machine or
// one of its cutscenes starts here (pause.h, CloseMenuForTheSession): with the
// world running under the menu, a player who had it open saw the menu while
// the scene played behind it. A start is the mission coming to a session that
// had none, so somebody who joins one running, or whose game comes out of a
// mission of its own into it, keeps his menu until its next cutscene.
bool g_menuSawIdle  = false;
bool g_menuWasIn    = false;
bool g_menuWasScene = false;

void CloseMenuForTheMission() {
	const MissionSync &m     = g_client->Missions();
	const bool         inIt  = g_own.running || m.ParticipantHere(LocalId());
	const bool         scene = inIt && g_cutsceneScene;
	if (inIt && !g_menuWasIn && g_menuSawIdle)
		CloseMenuForTheSession(g_own.running ? "the mission we started" : "the session's mission");
	else if (scene && !g_menuWasScene)
		CloseMenuForTheSession("the mission's cutscene");
	g_menuWasIn    = inIt;
	g_menuWasScene = scene;
	g_menuSawIdle  = m.Shared() && !m.Running() && !g_own.running;
}

} // namespace

void DrawMissionWait() {
	if (!g_installed || !g_client)
		return;
	char text[FEED_MESSAGE];
	if (!g_client->Missions().WaitLine(text, sizeof text, LocalId(), WallClock::NowMs()))
		return;
	// Above the cutscene's skip count when both are up.
	DrawCornerMark(text, g_client->SkipView().Crowded() ? 1 : 0);
}

void TickMissions() {
	++g_frame;
	if (!g_installed)
		return;
	UpdateScene();
	if (g_client)
		CloseMenuForTheMission();
	TickOutfit();
	// Held at 1 for as long as the session's mission runs, whatever this
	// machine's own scripts do to it.
	int32_t *flag = OnMissionVar();
	if (g_mirror && flag && *flag != 1)
		*flag = 1;
	// The owner's cutscene is the owner's to skip, and CLEAR_CUTSCENE ends
	// ours when the owner's ends. With the skip input's call taken
	// (cutsceneskip.h) a press here is a vote instead, and does nothing
	// alone; the buttons are only held when it could not be taken.
	if (g_shown.cutscene && !CutsceneSkipRedirected())
		HoldSkipButtons();
	if (g_client && g_floatingCount != 0 && (g_frame & 7) == 4)
		WatchFloating(WallClock::NowMs());
	if (g_client)
		WatchOwnGame(WallClock::NowMs());
	// A participant's death the owner's mission could not be failed for yet.
	if (g_client)
		RetryFailMission();
	// Whether a countdown is up: the owner's own, or the one replayed here.
	if (g_client)
		g_client->Missions().SetTimerUp(g_own.running ? g_ownTimer != 0 : g_shown.timer != 0);
	if (g_client && (g_frame & 7) == 5)
		BringToOwner(WallClock::NowMs());
	// A script's drive at a wheel somebody else took meanwhile, and a ride the
	// session is to hear about once our player sits down.
	if (g_client && (g_wheelWalk >= 0 || g_ride >= 0))
		WatchScriptedWheel(WallClock::NowMs());
	// Everything the owner's mission made stays on the session (KeepMissionCars,
	// KeepMissionEntitiesHosted).
	if (g_client) {
		const uint32_t now = WallClock::NowMs();
		KeepMissionCars(now);
		KeepMissionEntitiesHosted(now);
		SweepMissionEnemies(now);
	}
	// A failed mission's cars nobody sits in, out of the way of the next try.
	if (g_client && g_leftCarCount != 0 && (g_frame & 7) == 6)
		ClearLeftCars(WallClock::NowMs());
	ShowWorldWhileWaiting();
	// A participant's garages, for the owner's garage questions.
	if (g_client && (g_frame & 7) == 2 && g_client->Missions().ParticipantHere(LocalId()))
		AskOwnEngine(WallClock::NowMs());
	// The owner's Catalina took off or flew away before ours was up.
	if (g_shown.catalinaPath != 0 && CatalinaHere()) {
		RunOurs(g_shown.catalinaPath, {});
		g_shown.catalinaPath = 0;
	}
	// A seat the owner handed us in the car its mission put its player in.
	if (g_client && !g_own.running)
		TakeBoardSeat(WallClock::NowMs());
	if (!g_own.running)
		g_boardCar = -1;
	// The owner's widgets, for everybody's HUD to read the same values, and
	// whatever of its effects was waiting for a pedestrian or car to be named.
	if (g_client && (g_own.running || g_waitingCount != 0)) {
		const uint32_t now = WallClock::NowMs();
		if (g_own.running) {
			TickShape(now);
			TellCoronas(now);
			TellPills(now);
		}
		if (g_waitingCount != 0)
			FlushEffects(now, false);
		if (g_own.running && g_highlight.IsInstalled())
			TellMarkers(now);
		if (g_own.running && (g_seatOrderCount != 0 || g_followerCount != 0))
			WatchSeatOrders(now);
		if (g_own.running && (g_frame & 7) == 0)
			KeepMissionSeats(now);
		if (g_own.running && g_boardCar >= 0)
			BoardParticipants(now);
		if (g_own.running && (g_frame & 7) == 3)
			WatchOutOfReach();
		if (g_ownTimer != 0)
			g_client->Missions().WidgetValue(g_ownTimer, At<int32_t>(Space(), g_ownTimer), true,
			                                 g_ownTimerFrozen, LocalId(), now);
		if (g_ownCounter != 0)
			g_client->Missions().WidgetValue(g_ownCounter, At<int32_t>(Space(), g_ownCounter),
			                                 false, false, LocalId(), now);
	}
}

void DrawMissionMarkers() {
	if (!g_installed || !g_client || (g_shownMarkers.Count() == 0 && g_shownCoronas.Count() == 0))
		return;
	// A game in a mission of its own is not in the session's.
	if (g_client->Missions().Busy())
		return;
	const uint32_t now = WallClock::NowMs();
	if (g_highlightUsable)
		g_shownMarkers.Each(now, [](uint32_t id, const MarkerArea &a) { Highlight(id | SHOWN_MARKER_ID, a); });
	// The coronas, each registered for this frame the way DRAW_CORONA's
	// handler registers the owner's (missionworld.h). A light or a shadow is
	// the instruction itself, run through our own interpreter, which adds it
	// for this frame as the owner's did (effectshape.h).
	g_shownCoronas.Each(now, [](uint32_t id, const shape::CoronaDraw &c) {
		if (c.opcode != shape::DRAW_CORONA) {
			uint8_t      code[MISSION_EFFECT_CODE];
			const size_t n = shape::FrameDrawCode(c, code, sizeof code);
			if (n != 0)
				RunHere(code, n, nullptr);
			return;
		}
		const float pos[3] = {c.x, c.y, c.z};
		Func<world::RegisterCoronaFn>(world::CCoronas__RegisterCorona)(
		    world::SHOWN_CORONA_ID | (id & 0x3FFFFFFFu), static_cast<uint8_t>(c.r), static_cast<uint8_t>(c.g),
		    static_cast<uint8_t>(c.b), 255, pos, c.size, world::CORONA_DRAW_DISTANCE,
		    static_cast<uint8_t>(c.type), static_cast<uint8_t>(c.flare), 1, 0, 0, 0.0f);
	});
}

bool SessionPassedKey(char key[9]) {
	if (!g_client || !g_hooks[R200].detour.IsInstalled())
		return false;
	return g_client->Missions().LastPassedKey(ScriptHash(), key);
}

} // namespace coopiii::game
