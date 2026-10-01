#include "population.h"

#include "addresses.h"
#include "adopt.h"
#include "carletgo.h"
#include "crowdaddr.h"
#include "mission.h"
#include "missioncombat.h"
#include "ped.h"
#include "pedanim.h"
#include "pedspeech.h"
#include "runover.h"
#include "social.h"
#include "streampick.h"
#include "teardown.h"
#include "vehicle.h"
#include "wanted.h"
#include "wreckqueue.h"
#include "../hook/hook.h"
#include "../clock.h"
#include "../log.h"

#include <intrin.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace coopiii::game {

namespace {

using AddFn        = void(__cdecl *)(void *);
using RemoveFn     = void(__cdecl *)(void *);
using NewFn        = void *(__cdecl *)(size_t);
using CtorFn       = void(__thiscall *)(void *, int, int);
using RefFn        = int32_t(__cdecl *)(void *);
using GetPedFn     = void *(__cdecl *)(int32_t);
using ThisFn       = void(__thiscall *)(void *);
using RotateFn     = void(__thiscall *)(void *, float, float, float);
using RemoveRefsFn = void(__cdecl *)(void *);
using DtorFn       = void(__thiscall *)(void *, int);

Detour g_addDetour;
Detour g_removeDetour;
Detour g_bodyPartDetour;

// True while this file is itself calling CWorld::Add, so the detour does not
// hand a replica straight back to the client as a locally created ped and
// announce somebody else's pedestrian to them as one of ours.
//
// A plain bool rather than a thread_local: every call to CWorld::Add that
// CoopIII makes happens on the game thread, in the frame pump, and the engine
// has no other thread that adds entities. If that ever stops being true the
// failure is a replica announced as ours, which is loud - it would show up as
// a ped that exists twice on every other screen.
bool g_creatingReplica = false;

// How many replicas we are holding, for the crowd measurement below.
uint32_t g_replicas = 0;

// How often the engine has taken a replica away underneath us - the handle
// stopped resolving, or it resolved to something that is not ours any more.
// Counted rather than only acted on, because until AmbientReplicaIsAlive
// existed nothing in this project knew whether it ever happened, and the
// comments that claimed a replica was rebuilt when it did were written on an
// assumption. A session that reports zero here is one where the recovery
// never had to fire.
uint32_t g_replicaHandleLost = 0;
uint32_t g_replicaSlotStolen = 0;

// How often this machine's own engine has tried to kill a replica, and how
// often it managed it before anything could stop it.
//
// Two numbers rather than one, because they mean opposite things. The first
// is the guard working: a death refused at CPed::SetDie costs nothing and
// nobody sees it. The second is the backstop working: a replica that reached
// PED_DIE or PED_DEAD with nothing in the session having asked, which today
// has exactly one known route (CAutomobile::BlowUpCar's PED_DRIVING arm goes
// through CPed::SetDead, a different address) and is rebuilt from the host's
// stream. A session that reports a large second number against a small first
// one is one where this file is guarding the wrong door.
uint32_t g_replicaDeathRefused  = 0;
uint32_t g_replicaDeathRecovered = 0;

// True while KillAmbientReplica is carrying out a death the host reported.
// population.h, HostDeathScope, has the reason; a plain bool for the same
// reason g_creatingReplica is one.
bool g_applyingHostDeath = false;

// ---- which CPed is a replica of whose pedestrian ----------------------------
//
// The reverse index population.h promises, and it lives here rather than in
// client.cpp because this file owns every transition of
// `RemoteAmbientPed::poolHandle` - SpawnAmbientReplica sets it,
// DespawnAmbientReplica clears it, AmbientReplicaIsAlive clears it when the
// engine has taken the object away - and a table maintained anywhere else
// would be a second thing to keep in step with three writers.
//
// Shaped after `HostedPed` rather than after ped.cpp's `g_remotePeds`, and the
// difference is deliberate. `g_remotePeds` is eight entries, so it can afford a
// CPools::GetPed per entry per lookup. This is 256, and the caller is inside
// CPed::InflictDamage. So the scan is 256 pointer compares and the pool lookup
// happens once, on the one entry that matched - which is where it is
// load-bearing anyway, because a pool slot is reused immediately and "the
// pointer still matches" is not "our replica is still alive".
//
// Matched to Client::MAX_REMOTE_PEDS, which is matched to the server's
// MAX_AMBIENT_PEDS. A roster this table could not hold would silently stop
// answering for whichever replicas fell off the end, and the symptom would be
// a pedestrian that cannot be shot - which is the bug this whole file's newest
// half exists to end.
constexpr size_t MAX_REPLICA_INDEX = 256;

struct ReplicaIdentity {
	void    *ped         = nullptr;
	int32_t  poolHandle  = -1;
	uint16_t netId       = INVALID_NETID;
	// What the host's engine made it. The replica itself is always a
	// civilian; this is the only place a policeman is still one.
	uint8_t  hostPedType = 0;
};

ReplicaIdentity g_replicaIndex[MAX_REPLICA_INDEX];

// Said once. Past the table a replica still exists and still walks around; it
// simply cannot be shot, which is a visible thing and has to be said rather
// than discovered.
bool g_warnedReplicaIndexFull = false;
bool g_saidRenameTakedown     = false;

void RememberReplica(void *ped, int32_t poolHandle, uint16_t netId, uint8_t hostPedType) {
	for (ReplicaIdentity &id : g_replicaIndex) {
		if (id.ped != nullptr)
			continue;
		id = ReplicaIdentity{ped, poolHandle, netId, hostPedType};
		return;
	}
	if (!g_warnedReplicaIndexFull) {
		g_warnedReplicaIndexFull = true;
		Log("population: the replica index is full at %zu; pedestrians past it "
		    "cannot be shot on this machine (and this will not be said again)",
		    MAX_REPLICA_INDEX);
	}
}

void ForgetReplica(uint16_t netId) {
	for (ReplicaIdentity &id : g_replicaIndex)
		if (id.ped != nullptr && id.netId == netId)
			id = ReplicaIdentity{};
}

// ---- the duplicate-pedestrian instrument -----------------------------------
//
// Reported from a live session: after one player killed another, the victim's
// screen showed the killer's pursuing cops *twice*. Nothing in the logs from
// that session names a pedestrian at all, so the first job is to make the two
// candidate shapes of that bug tell themselves apart, which the counts in
// ReportCrowd cannot:
//
//   - one real ped announced under two netIds. Shows up here as two `host
//     add` lines carrying the same CPed pointer with no `host gone` between
//     them, or with a `host gone` that was never named and therefore never
//     travelled. The observer then holds two rows, builds two replicas, and
//     only one of them is being streamed - so one cop chases and one stands.
//   - one netId built twice on the observer. Shows up as two `replica spawn`
//     lines for one netId with no `replica gone` between them.
//
// Which of the two appears is the answer, so both sides are traced and the
// CPed pointer is on every line: it is the only thing that identifies one
// pedestrian across a claim it loses and a claim it gains.
//
// Off unless COOPIII_POPDEBUG is set in the environment. A pedestrian
// generator produces a line or two a second even when nothing is wrong, and
// this file's other reports are deliberately once-only for that reason. The
// environment rather than CoopIII.ini for the same reason COOPIII_NICK is
// there: two instances out of one game folder, one of them being watched.
bool PopTrace() {
	static const bool on = [] {
		size_t len = 0;
		char   buf[8]{};
		return getenv_s(&len, buf, sizeof(buf), "COOPIII_POPDEBUG") == 0 &&
		       len != 0 && buf[0] != '0';
	}();
	return on;
}

// ---- what the two streams actually cost ------------------------------------
//
// docs/population.md §2.1 says the rate has to be measured rather than
// guessed, and §3 step 5 is the work that does something about the answer.
// These are the instrument: every row and every packet the two samplers hand
// over, counted where they are produced, so the number in the log is
// arithmetic over what really went on the wire rather than a calculation from
// what the design hoped would.
//
// Cumulative; ReportCrowd differences them against the previous report and
// divides by the real interval, so a frame rate that drops or a tick that is
// skipped shows up as a lower rate rather than being averaged away.
uint32_t g_pedRowsSent     = 0;
uint32_t g_pedPacketsSent  = 0;
uint32_t g_carRowsSent     = 0;
uint32_t g_carPacketsSent  = 0;

// And whether everything we host gets its turn (game/streampick.h): the
// longest any row that went out had waited, in stream ticks, since the last
// report. ReportStreamTurns counts who had a row at all.
uint8_t g_pedLongestWait = 0;
uint8_t g_carLongestWait = 0;
bool    g_saidPedsTakeTurns = false;
bool    g_saidHostedAtDoor  = false;
bool    g_saidReplicaAtDoor = false;
bool    g_saidReplicaOutOfDoor = false;
bool    g_saidCarsTakeTurns = false;

void ReportStreamTurns(float seconds);

// The cars our engine added since the last report and what became of them,
// said only when one of this machine's own traffic cars went unhosted.
void ReportCarAdds();

// Hosted pedestrians sitting in a car the session has no name for, as of the
// last ped batch. Said once for a car we do not host at all, which is the one
// that stays, and once for one of ours not named yet, which is a round trip.
uint32_t g_pedsInUnseenCars      = 0;
bool     g_saidPedInUnseenCar    = false;
bool     g_saidPedInUnnamedCar   = false;

// ---- and whether any of it is working --------------------------------------
//
// The crowd counts above say the city is shared rather than doubled. They say
// nothing at all about whether a replicated pedestrian is *doing* anything,
// and that is precisely the failure step 2 shipped: a replica that is created
// and placed once looks, in every count, exactly like one that is being
// streamed. It takes walking down the street to tell them apart.
//
// So: how many placements actually moved the ped, against how many were made.
// A held replica is placed every frame at the same coordinate and moves on
// none of them; a walking one moves on nearly all. The ratio is the witness.
uint32_t g_pedApplies     = 0;
uint32_t g_pedApplyMoves  = 0;
// And how many replicas are sitting in a replicated car right now, which is
// the other half of this change and has no other visible number.
uint32_t g_seatedReplicas = 0;

// What the engine thinks the crowd is, next to what it really is.
//
// This is the instrument docs/population.md §1.3.1 was measured with, and
// §3 step 4 requires it to be pointed at traffic before a line of counter
// code is written for cars. Both halves are here now.
//
// For pedestrians the answer is already in: CPed::CPed calls
// CPopulation::UpdatePedCount, so a replica is counted the moment it is
// constructed and the generator was seeing the shared crowd all along.
//
// For cars the mechanism is the same - CVehicle::CVehicle calls
// CCarCtrl::UpdateCarCount (addresses.h) - with one difference that decides
// everything: UpdateCarCount switches on VehicleCreatedBy, so *which* counter
// a car lands in depends on a byte, and the traffic generator's first gate
// reads NumRandomCars alone. Replicas are created as RANDOM_VEHICLE precisely
// so they land in that one. The line below is what says whether that worked:
// if `NumRandomCars` equals hosted + replicas on both machines, the gate is
// seeing the shared street and the counter rewriting is unnecessary for
// traffic exactly as it was for pedestrians. If it only equals the hosted
// count, it is not, and §1.3's rewriting is the answer.
//
// Once every five seconds, and only while there is something to compare.
void ReportCrowd(uint32_t peds, uint32_t pedReplicas, uint32_t cars,
                 uint32_t carReplicas) {
	static uint32_t lastMs = 0;
	const uint32_t  now    = WallClock::NowMs();
	if (now - lastMs < 5000)
		return;
	lastMs = now;
	if (peds == 0 && pedReplicas == 0 && cars == 0 && carReplicas == 0)
		return;

	Log("population: the engine counts %u peds and %d random cars; we host %u "
	    "ped(s) + %u car(s) and hold %u ped replica(s) + %u car replica(s). "
	    "peds want %u, cars want %u",
	    Global<uint32_t>(CPopulation__ms_nTotalPeds),
	    Global<int32_t>(CCarCtrl__NumRandomCars), peds, cars, pedReplicas,
	    carReplicas, peds + pedReplicas, cars + carReplicas);

	ReportCarAdds();
	if (g_pedsInUnseenCars != 0)
		Log("population: %u of our pedestrian(s) sit in a car the session has no name for; "
		    "every other screen keeps its copy out of sight until it has one",
		    g_pedsInUnseenCars);

	// Said only when it is not zero, because zero is the claim being tested:
	// client.cpp says a replica the engine took away is rebuilt by the spawn
	// pass, and nothing resets the pool handle that would let it be. A
	// non-zero number here is that claim being false in a running game.
	if (g_replicaHandleLost != 0 || g_replicaSlotStolen != 0)
		Log("population: %u ped replica(s) have stopped resolving and %u have "
		    "had their pool slot taken since the session started",
		    g_replicaHandleLost, g_replicaSlotStolen);

	// Same rule: said only when it is not zero, because zero is what the
	// prevention is for. The first number is deaths this machine's engine
	// asked for and was refused; the second is replicas it killed by a route
	// that never reaches CPed::SetDie and that had to be rebuilt.
	if (g_replicaDeathRefused != 0 || g_replicaDeathRecovered != 0)
		Log("population: refused %u local death(s) of somebody else's "
		    "pedestrian and rebuilt %u replica(s) this machine killed anyway",
		    g_replicaDeathRefused, g_replicaDeathRecovered);

	// The second line, added with the ped stream (docs/population.md §3
	// step 6). The first line says whether the crowd is shared rather than
	// doubled; this one says what saying so costs.
	//
	// Differenced against the previous report and divided by the real
	// interval, so a dropped frame or a skipped tick reads as a lower rate
	// instead of disappearing into an average. Bytes are the wire structs
	// exactly - a batch is a header plus `count` rows, and a batch with no
	// rows is never sent.
	static uint32_t lastPedRows = 0, lastPedPackets = 0;
	static uint32_t lastCarRows = 0, lastCarPackets = 0;
	static uint32_t lastCostMs  = 0;

	if (lastCostMs != 0 && now > lastCostMs) {
		const float seconds = static_cast<float>(now - lastCostMs) / 1000.0f;
		const uint32_t pedRows = g_pedRowsSent - lastPedRows;
		const uint32_t carRows = g_carRowsSent - lastCarRows;
		const uint32_t pedPkts = g_pedPacketsSent - lastPedPackets;
		const uint32_t carPkts = g_carPacketsSent - lastCarPackets;

		const float pedBytes =
		    static_cast<float>(pedPkts) * (sizeof(C_PedStates) -
		                                   sizeof(AmbientPedState) * MAX_PED_STATES) +
		    static_cast<float>(pedRows) * sizeof(AmbientPedState);
		const float carBytes =
		    static_cast<float>(carPkts) * (sizeof(C_CarStates) -
		                                   sizeof(AmbientCarState) * MAX_CAR_STATES) +
		    static_cast<float>(carRows) * sizeof(AmbientCarState);

		// The third line, and the one that says whether the pedestrians are
		// walking. A replica held at a fixed coordinate is placed every
		// frame and moves on none of them; one being streamed moves on
		// nearly all. Before this change the ratio was zero by construction.
		static uint32_t lastApplies = 0, lastMoves = 0;
		const uint32_t applies = g_pedApplies - lastApplies;
		const uint32_t moves   = g_pedApplyMoves - lastMoves;
		lastApplies = g_pedApplies;
		lastMoves   = g_pedApplyMoves;
		if (applies != 0 || g_seatedReplicas != 0)
			Log("population: ped replicas - %u of %u placement(s) actually "
			    "moved the ped (%.0f%%), and %u replica(s) are sitting in a "
			    "replicated car",
			    moves, applies,
			    static_cast<double>(applies ? 100.0f * static_cast<float>(moves) /
			                                      static_cast<float>(applies)
			                                : 0.0f),
			    g_seatedReplicas);

		Log("population: outbound ambient streams over the last %.1fs - peds "
		    "%u row(s) in %u packet(s) = %.2f KB/s, cars %u row(s) in %u "
		    "packet(s) = %.2f KB/s, total %.2f KB/s",
		    static_cast<double>(seconds), pedRows, pedPkts,
		    static_cast<double>(pedBytes / seconds / 1024.0f), carRows, carPkts,
		    static_cast<double>(carBytes / seconds / 1024.0f),
		    static_cast<double>((pedBytes + carBytes) / seconds / 1024.0f));

		ReportStreamTurns(seconds);
	}

	lastPedRows    = g_pedRowsSent;
	lastPedPackets = g_pedPacketsSent;
	lastCarRows    = g_carRowsSent;
	lastCarPackets = g_carPacketsSent;
	lastCostMs     = now;
}

// One ambient pedestrian this machine hosts.
//
// `ped` is kept alongside `poolHandle` on purpose. The handle is what stops
// resolving when the engine deletes the ped, and the pointer is what says
// *which* ped the handle used to mean - a slot is reused immediately, so a
// handle that still resolves is not by itself proof that our ped is alive.
// The sweep compares both.
struct HostedPed {
	bool     active     = false;
	void    *ped        = nullptr;
	int32_t  poolHandle = -1;
	uint32_t tempId     = 0;
	uint16_t netId      = INVALID_NETID;
	// The session has named it. Until then there is nothing to tell anybody
	// when it goes - see NameLocalAmbientPed.
	bool     named      = false;
	// Kept only so the trace can say what left. The pointer alone cannot: a
	// pool slot is reused within the same frame, so `gone X` followed by
	// `add X` is usually recycling and only occasionally the same pedestrian
	// being re-filed - and telling those two apart is the whole question the
	// duplicate-cop report asks.
	uint16_t modelId    = 0;
	// Its turn in the ped batch (game/streampick.h), and whether it has had a
	// row since the last crowd report.
	StreamRow stream;
	bool      rowSinceReport = false;
	// The session's mission made it here (docs/missions.md 5.3).
	bool      mission = false;
	// When the session handed it to us (S_AmbientAdopt), on WallClock; 0 for
	// one our own engine made. A cop handed to us is not handed on again
	// inside COP_HANDOVER_HOLD_MS (game/wanted.h).
	uint32_t  heldSinceMs = 0;
};

// Matched to the engine rather than to the session: GTA III's ped pool is 140
// slots and CPopulation keeps roughly 25 pedestrians alive around the player,
// so this has room for every ambient ped that can exist here at once and then
// some. Past it, a ped stays a purely local pedestrian, which is what every
// pedestrian was before this file existed.
constexpr size_t MAX_HOSTED = 192;
HostedPed g_hosted[MAX_HOSTED];

// Births waiting to be announced, and deaths waiting to be announced. Both
// are drained by the client once a frame.
constexpr size_t MAX_QUEUED = 128;
LocalAmbientPed g_born[MAX_QUEUED];
uint32_t        g_bornCount = 0;
uint16_t        g_lost[MAX_QUEUED];
uint32_t        g_lostCount = 0;

uint32_t g_nextTempId = 1;

// One line each, not one per frame. The two things that overflow here are fed
// by a pedestrian generator, so "it happened again" is never news.
bool g_warnedHostedFull = false;
bool g_warnedBornFull   = false;
bool g_warnedLostFull   = false;

void *PlayerPed() { return Func<void *(__cdecl *)()>(FindPlayerPed)(); }

uint8_t EntityType(const void *entity) {
	return static_cast<uint8_t>(
	    Field<uint8_t>(const_cast<void *>(entity), offs::ENTITY_FLAGS) & 0x07);
}

HostedPed *FindHostedByPed(const void *ped) {
	for (HostedPed &h : g_hosted)
		if (h.active && h.ped == ped)
			return &h;
	return nullptr;
}

HostedPed *FindHostedByTempId(uint32_t tempId) {
	for (HostedPed &h : g_hosted)
		if (h.active && h.tempId == tempId)
			return &h;
	return nullptr;
}

void QueueLost(HostedPed &h, const char *why) {
	// `why` separates the two routes out of g_hosted, which behave differently
	// and fail differently: CWorld::Remove is a fast path that also fires for
	// a ped merely being re-filed (CPed::Teleport is Remove-then-Add), and the
	// sweep is the authority that notices everything else. A trace showing a
	// `remove` immediately followed by an `add` on the same pointer is the
	// engine moving a ped, not a pedestrian dying, and it costs a netId.
	if (PopTrace())
		Log("population/trace: host gone ped %p temp %u net %u model %u "
		    "named %d (%s)", h.ped, h.tempId, h.netId, h.modelId,
		    h.named ? 1 : 0, why);

	// Only a ped the session has a name for. One that died before its name
	// came back is not forgotten silently - NameLocalAmbientPed reports it
	// when the name arrives, because that is the first moment there is
	// anything to say.
	if (h.named) {
		if (g_lostCount < MAX_QUEUED) {
			g_lost[g_lostCount++] = h.netId;
		} else if (!g_warnedLostFull) {
			g_warnedLostFull = true;
			Log("population: the lost-ped queue is full; some replicas will "
			    "outlive their originals (and this will not be said again)");
		}
	}
	h = HostedPed{};
}

// Is this something the session should know about?
//
// Three tests, and each one is here because of what it lets through rather
// than what it stops:
//
//   - a ped, by the engine's own m_type bits, not by "we think we made it"
//   - created by the engine's population code, which is what RANDOM_CHAR
//     means. A mission ped belongs to the campaign and the campaign runs on
//     the host (docs/campaign.md); replicating one here would have two
//     machines' scripts both owning the same character.
//   - not the player. FindPlayerPed is cheap and the player is the one ped
//     that is never ambient.
//
// A CoopIII replica passes none of them: it is created as MISSION_CHAR, and
// the Add that registers it is wrapped in g_creatingReplica anyway.
//
// **And one mission ped does pass: the session's own** (docs/missions.md
// 5.3). A MISSION_CHAR added while the session's mission runs one of its
// instructions on this machine, its owner (game/mission.h,
// MissionMakingEntities), is that mission's enemy, target or friend, and
// everybody in the mission has to see it and be able to fight it. The
// campaign.md reason above was a campaign run on the host; the session's
// mission runs on its owner's machine, and so its pedestrians are hosted
// there. A remote player's ped is a MISSION_CHAR too, and is added from the
// frame pump, never inside a script instruction.
bool IsAmbientPedWeShouldHost(void *entity) {
	if (!entity)
		return false;
	if (EntityType(entity) != offs::ENTITY_TYPE_PED)
		return false;
	const uint8_t createdBy = Field<uint8_t>(entity, offs::PED_CHAR_CREATED_BY);
	if (createdBy != CHAR_CREATED_BY_RANDOM &&
	    !(createdBy == CHAR_CREATED_BY_MISSION && MissionMakingEntities()))
		return false;
	if (entity == PlayerPed())
		return false;
	return true;
}

// ---- ambient traffic (docs/population.md §3 step 4) ------------------------
//
// The pedestrian machinery above, applied to cars. `CWorld::Add` already saw
// every one of these going past - step 2 simply filtered them out - so what
// is new here is the bookkeeping and one thing a pedestrian never needed: a
// traffic car is going somewhere, so its host has to keep saying where.

// One traffic car this machine hosts.
//
// Same shape as HostedPed and kept separate rather than templated: the two
// have different pools behind them, different caps, and the car carries the
// transform stream's bookkeeping.
struct HostedCar {
	bool     active     = false;
	void    *vehicle    = nullptr;
	int32_t  poolHandle = -1;
	uint32_t tempId     = 0;
	uint16_t netId      = INVALID_NETID;
	bool     named      = false;
	// Said once, and only ever by the machine hosting the car. See
	// NoteWreckedHostedCars.
	bool     reportedWreck = false;
	// The same two HostedPed has, for the car batch.
	StreamRow stream;
	bool      rowSinceReport = false;
	// The dents already told to the session (DrainHostedCarDamage).
	uint32_t  sentPanels = 0;
	uint16_t  sentDoors  = 0;
	// The session's mission made it here (docs/missions.md 5.3).
	bool      mission = false;
	// Kept for the log, which may speak of it after the car has gone.
	uint16_t  modelId = 0;
	// As HostedPed::heldSinceMs.
	uint32_t  heldSinceMs = 0;
};

// GTA III's whole vehicle pool is 110 slots and CCarCtrl keeps a dozen-odd
// random cars alive around the player. 64 is that with a lot of room over;
// past it a car stays purely local to this machine, which is what every
// traffic car was before this file existed.
constexpr size_t MAX_HOSTED_CARS = 64;
HostedCar g_hostedCars[MAX_HOSTED_CARS];

// A handover's stamp for heldSinceMs: never 0, which means "our own".
uint32_t HeldSinceNow() {
	const uint32_t now = WallClock::NowMs();
	return now != 0 ? now : 1;
}

// Who had a row since the last report, out of everything named. Before the
// batches took turns the first number stopped at MAX_PED_STATES and the
// second at MAX_CAR_STATES whatever we hosted, and the rest waited for ever.
void ReportStreamTurns(float seconds) {
	uint32_t peds = 0, pedsHeard = 0, cars = 0, carsHeard = 0;
	for (HostedPed &h : g_hosted) {
		if (!h.active || !h.named)
			continue;
		++peds;
		if (h.rowSinceReport)
			++pedsHeard;
		h.rowSinceReport = false;
	}
	for (HostedCar &c : g_hostedCars) {
		if (!c.active || !c.named)
			continue;
		++cars;
		if (c.rowSinceReport)
			++carsHeard;
		c.rowSinceReport = false;
	}

	if (peds != 0 || cars != 0)
		Log("population: over the last %.1fs %u of %u hosted ped(s) and %u of %u "
		    "hosted car(s) had a row; the longest a ped waited was %u ms, a car "
		    "%u ms", static_cast<double>(seconds), pedsHeard, peds, carsHeard, cars,
		    g_pedLongestWait * STREAM_TICK_MS, g_carLongestWait * STREAM_TICK_MS);
	g_pedLongestWait = 0;
	g_carLongestWait = 0;
}

constexpr size_t MAX_QUEUED_CARS = 32;
LocalAmbientCar g_bornCars[MAX_QUEUED_CARS];
uint32_t        g_bornCarCount = 0;
uint16_t        g_lostCars[MAX_QUEUED_CARS];
uint32_t        g_lostCarCount = 0;

uint32_t g_nextCarTempId = 1;
uint32_t g_carReplicas   = 0;

bool g_warnedHostedCarsFull = false;
bool g_warnedBornCarsFull   = false;
bool g_warnedLostCarsFull   = false;

// ---- a hosted traffic car that was destroyed (docs/roadmap.md 5.8) ---------
//
// The ambient half of "a car nobody is driving has nobody to report it", and
// the smaller half, because most of it already travelled. Worked out before
// any of this was written, the way the parked half was:
//
//   - An explosion is replayed on every machine at a position everybody
//     agreed on, and CWorld::TriggerExplosion damages every car in the radius
//     through CVehicle::InflictDamage with a multiplier that is a function of
//     distance and nothing else (addresses.h, "an explosion damages every car
//     in its radius"). So a replica standing next to the rocket goes up on
//     its own, here as much as for a parked car.
//   - And more than that: a car destroyed *by the local player* - shot,
//     rammed, set alight - blows up through CVehicle::InflictDamage(culprit),
//     which passes that culprit to CExplosion::AddExplosion. game/combat.cpp
//     relays an explosion whose culprit is the local player ped, so the blast
//     that killed the car is on the wire already and the replicas usually
//     die of it.
//
// What does not travel, and is the entire reason this queue exists:
//
//   - A destruction nobody's player caused. The fire timer running out, a
//     crash between two traffic cars, a traffic car an NPC shot. combat.cpp
//     deliberately does not relay those ("a car blowing up because the city
//     simulation decided so already happens on every machine") - which is
//     true of every machine's *own* traffic and false of a hosted one,
//     because a replica's autopilot is zeroed and its transform is written
//     from the wire. The replica is not simulating the crash that killed the
//     original and never will.
//   - Health. This used to be the big one: the original and its replicas
//     had independent healths, so the same blast finished one and left the
//     other running. Since docs/protocol.md §1.23 a replica takes no damage
//     of its own and holds the health the host streams, so the replica never
//     gets there first - it waits for this report.
//   - Distance. A batch holds MAX_CAR_STATES and the cars take turns at it,
//     so a replica far from everybody can be a few rows behind its original,
//     and it lags the host by the interpolation delay even when it is not.
//     A replica a few metres from its original is not always in the same
//     blast.
//
// So this is the backstop, exactly as it is for parked cars, and it is
// asymmetric on purpose: only the machine hosting a car may say that car
// died. A replica's own wreck is this machine's local opinion about somebody
// else's property - see IsAmbientCarWeShouldHost for where that refusal
// lives and why it is a property of the object rather than a moment in time.
//
// It holds every car this machine can host. It used to hold eight, and the
// sweep below marks a car reported before it queues it, so the ninth wreck of
// one frame - which BANGBANGBANG makes routine - was dropped and never tried
// again (game/wreckqueue.h).
WreckQueue<MAX_HOSTED_CARS> g_ambientWrecks;
static_assert(WreckQueue<MAX_HOSTED_CARS>::kCapacity >= MAX_HOSTED_CARS,
              "every hosted car can wreck in the same frame");

bool g_saidAmbientWreckSent    = false;
bool g_saidAmbientWreckApplied = false;

// STATUS_WRECKED, read the way CCarCtrl::PossiblyRemoveVehicle reads it
// (`shr dl,3 / cmp eax,5`). game/vehicle.cpp has the same three lines and
// they are not shared: this file may not reach into that one's anonymous
// namespace, and three lines of shift-and-compare is a cheaper duplicate
// than a header dependency in the wrong direction.
bool IsWreckedCar(void *vehicle) {
	return static_cast<uint8_t>(Field<uint8_t>(vehicle, offs::ENTITY_FLAGS) >>
	                            ENTITY_STATUS_SHIFT) == ENTITY_STATUS_WRECKED;
}

// `vehicle` is the car itself, still in the pool, still where it blew up. The
// transform is read here rather than at drain time on purpose: the drain runs
// on the next frame's send, by which point the wreck has been settling under
// gravity and may have been rolled down a kerb by the blast it just made.
void PushAmbientWreck(uint16_t netId, void *vehicle) {
	UnownedBlast blast{};
	blast.key.kind = UNOWNED_AMBIENT;
	blast.key.pad  = 0;
	blast.key.id   = netId;
	ReadCarBlastTransform(vehicle, blast.where);

	// Already queued: one car, one report. The queue refuses the second.
	g_ambientWrecks.Push(blast);
}

HostedCar *FindHostedCar(const void *vehicle) {
	for (HostedCar &c : g_hostedCars)
		if (c.active && c.vehicle == vehicle)
			return &c;
	return nullptr;
}

HostedCar *FindHostedCarByTempId(uint32_t tempId) {
	for (HostedCar &c : g_hostedCars)
		if (c.active && c.tempId == tempId)
			return &c;
	return nullptr;
}

void QueueLostCar(HostedCar &c, const char *why) {
	// A mission car going is what a participant sees as the car vanishing, or
	// never arriving: said every time, since a mission makes a handful.
	if (c.mission)
		Log("population: the mission's car %s (temp %u, model %u) is not hosted here any more: %s",
		    c.named ? "named" : "not yet named", c.tempId, static_cast<unsigned>(c.modelId), why);
	if (c.named) {
		if (g_lostCarCount < MAX_QUEUED_CARS) {
			g_lostCars[g_lostCarCount++] = c.netId;
		} else if (!g_warnedLostCarsFull) {
			g_warnedLostCarsFull = true;
			Log("population: the lost-car queue is full; some replicas will "
			    "outlive their originals (and this will not be said again)");
		}
	}
	c = HostedCar{};
}

// ---- a car of ours dropped next to somebody else (game/carletgo.h) -----------
//
// The other players, as the roster last put them, noted every frame before
// CGame::Process. Nobody at all outside a session.
ViewerAt g_viewers[MAX_PLAYERS];
uint32_t g_viewerCount = 0;

void NoteRemoteViewers(const ViewerAt *viewers, uint32_t count) {
	g_viewerCount = count < MAX_PLAYERS ? count : MAX_PLAYERS;
	for (uint32_t i = 0; i < g_viewerCount; ++i)
		g_viewers[i] = viewers[i];
}

constexpr size_t MAX_LET_GO = 16;
LocalCarLetGo    g_letGo[MAX_LET_GO];
uint32_t         g_letGoCount = 0;
bool             g_warnedLetGoFull = false;

// What our engine took, by reason, since the last summary; and how many cars
// with another player near them have been described one by one.
struct ReapTally {
	uint32_t by[7]  = {};   // CarReap
	uint32_t nearby = 0;    // another player within AMBIENT_CAR_KEEP_RADIUS_M
	uint32_t handed = 0;    // gone out as C_CarLetGo
};
static_assert(static_cast<size_t>(CarReap::Island) < 7, "a tally for every reason");
ReapTally g_reaps;
uint32_t  g_reapsSaidMs  = 0;
uint32_t  g_reapsDetailed = 0;
constexpr uint32_t REAP_DETAIL_LINES = 12;
constexpr uint32_t REAP_SUMMARY_MS   = 30000;

// ---- a pedestrian of ours dropped next to somebody else (game/carletgo.h) -----
//
// The netIds waiting to go out as C_PedLetGo, already out of our hosting.
constexpr size_t MAX_PED_LET_GO_QUEUED = 64;
uint16_t         g_pedLetGo[MAX_PED_LET_GO_QUEUED];
uint32_t         g_pedLetGoCount       = 0;
bool             g_warnedPedLetGoFull  = false;

// A respawn's clear takes every pedestrian before any car, the ones sitting
// in cars included (carletgo.h), so a driver of ours is gone before the car
// he drives is decided. He waits here for it: handed on with it when it is
// handed on, despawned with it when it is not, and decided on his own when
// the clear left his car standing.
struct SeatedDrop {
	uint16_t netId   = INVALID_NETID;
	void    *vehicle = nullptr;
	Vec3     at{};
	uint8_t  pedType = 0;
	bool     alive   = true;
};
constexpr size_t MAX_SEATED_DROPS = 64;
SeatedDrop       g_seatedDrops[MAX_SEATED_DROPS];
uint32_t         g_seatedDropCount = 0;

// What our engine took of our pedestrians, by reason, since the last summary.
struct PedDropTally {
	uint32_t by[4]  = {};   // PedDrop
	uint32_t nearby = 0;    // another player within AMBIENT_PED_KEEP_RADIUS_M
	uint32_t handed = 0;    // gone out as C_PedLetGo, or with their car
};
static_assert(static_cast<size_t>(PedDrop::Island) < 4, "a tally for every reason");
PedDropTally g_pedDrops;
uint32_t     g_pedDropsDetailed = 0;

float NearestViewerD2(const Vec3 &at) {
	Vec3 pts[MAX_PLAYERS];
	for (uint32_t i = 0; i < g_viewerCount; ++i)
		pts[i] = g_viewers[i].pos;
	return NearestFlatD2(at.x, at.y, pts, g_viewerCount);
}

bool QueuePedLetGo(uint16_t netId) {
	if (g_pedLetGoCount >= MAX_PED_LET_GO_QUEUED) {
		if (!g_warnedPedLetGoFull) {
			g_warnedPedLetGoFull = true;
			Log("population: the pedestrian let-go queue is full; one our engine dropped "
			    "beside somebody is despawned instead (and this will not be said again)");
		}
		return false;
	}
	g_pedLetGo[g_pedLetGoCount++] = netId;
	return true;
}

void QueueLostNetId(uint16_t netId) {
	if (g_lostCount < MAX_QUEUED)
		g_lost[g_lostCount++] = netId;
}

// The first dozen pedestrians of ours our engine took with somebody near.
void DescribePedDrop(uint16_t netId, uint16_t modelId, const Vec3 &at, PedDrop drop,
                     float nearD2, bool handed) {
	if (g_pedDropsDetailed >= REAP_DETAIL_LINES)
		return;
	++g_pedDropsDetailed;
	Log("population: our engine took pedestrian %u (model %u) out of the world at "
	    "(%.0f %.0f %.0f) - %s; the nearest other player %.0f m from him; %s",
	    netId, static_cast<unsigned>(modelId), static_cast<double>(at.x),
	    static_cast<double>(at.y), static_cast<double>(at.z), PedDropName(drop),
	    static_cast<double>(std::sqrt(nearD2)),
	    handed ? "asking the session to hand him to somebody near him" : "despawned");
	if (g_pedDropsDetailed == REAP_DETAIL_LINES)
		Log("population: that is the last pedestrian described one by one; the rest are "
		    "counted every %u s", REAP_SUMMARY_MS / 1000);
}

// Why a pedestrian of ours is on his way out of the world, from the Remove
// detour's return address and the two scopes that name one.
PedDrop DropOfPed(const void *ped, uintptr_t ret) {
	if (ped == PedBeingReaped())
		return PedDrop::Reaper;
	if (ret == ISLAND_PED_REMOVE_RETURN)
		return PedDrop::Island;
	if (RespawnClearing())
		return PedDrop::Respawn;
	return PedDrop::Other;
}

CarReap ReapOfCar(const void *vehicle, uintptr_t ret) {
	if (vehicle == CarBeingReaped())
		return ReapFromReturn(ret);
	if (ret == ISLAND_CAR_REMOVE_RETURN)
		return CarReap::Island;
	if (RespawnClearing())
		return CarReap::Respawn;
	return CarReap::Other;
}

// A driver or passenger a respawn's clear took ahead of his car: whatever the
// clear did with the car, it is settled. Handed on with it, lost with it, or,
// his car left standing, decided on his own where he sat.
void SettleSeatedDrops() {
	for (uint32_t i = 0; i < g_seatedDropCount; ++i) {
		const SeatedDrop &d = g_seatedDrops[i];
		if (d.netId == INVALID_NETID)
			continue;
		const float nearD2 = NearestViewerD2(d.at);
		if (ShouldLetGoPed(PedDrop::Respawn, true, false, d.alive, d.pedType, nearD2) &&
		    QueuePedLetGo(d.netId))
			++g_pedDrops.handed;
		else
			QueueLostNetId(d.netId);
	}
	g_seatedDropCount = 0;
}

// Which car, why, and how far from everybody: the line a vanish needs.
void DescribeReap(const HostedCar &c, const Vec3 &at, CarReap reap, uintptr_t ret,
                  const char *outcome) {
	char   who[160] = "";
	size_t used     = 0;
	for (uint32_t i = 0; i < g_viewerCount && used + 32 < sizeof who; ++i) {
		const float dx = g_viewers[i].pos.x - at.x, dy = g_viewers[i].pos.y - at.y;
		const int   n  = std::snprintf(who + used, sizeof who - used, "%splayer %u at %.0f m",
		                               i == 0 ? "" : ", ",
		                               static_cast<unsigned>(g_viewers[i].playerId),
		                               static_cast<double>(std::sqrt(dx * dx + dy * dy)));
		if (n <= 0 || static_cast<size_t>(n) >= sizeof who - used) {
			used = std::strlen(who);
			break;
		}
		used += static_cast<size_t>(n);
	}
	float ours = -1.0f;
	if (PlayerPed()) {
		const float *p = Func<const float *(__cdecl *)(int32_t)>(FindPlayerCentreOfWorld)(
		    static_cast<int32_t>(Global<uint8_t>(CWorld__PlayerInFocus)));
		if (p) {
			const float dx = p[0] - at.x, dy = p[1] - at.y;
			ours = std::sqrt(dx * dx + dy * dy);
		}
	}
	char why[64];
	if (reap == CarReap::Other)
		std::snprintf(why, sizeof why, "CWorld::Remove from 0x%08X",
		              static_cast<unsigned>(ret));
	else
		std::snprintf(why, sizeof why, "%s", ReapName(reap));
	Log("population: our engine took traffic car %u (model %u) out of the world at "
	    "(%.0f %.0f %.0f) - %s; our player %.0f m from it, %s; %s",
	    c.netId, static_cast<unsigned>(c.modelId), static_cast<double>(at.x),
	    static_cast<double>(at.y), static_cast<double>(at.z), why,
	    static_cast<double>(ours), used != 0 ? who : "nobody else in the session", outcome);
}

// Every thirty seconds while our engine is taking cars of ours, and never
// otherwise. The numbers are what say whether handing on is keeping up with
// the reaper, and which of its rules is doing the reaping.
void ReportReaps() {
	const uint32_t now = WallClock::NowMs();
	if (g_reapsSaidMs == 0)
		g_reapsSaidMs = now;
	if (now - g_reapsSaidMs < REAP_SUMMARY_MS)
		return;
	const ReapTally t = g_reaps;
	uint32_t total    = 0;
	for (uint32_t n : t.by)
		total += n;
	if (total != 0)
		Log("population: over the last %us our engine took %u of our traffic car(s) out of "
		    "the world - %u stopped behind us, %u too far, %u faded out, %u wreck(s), %u on "
		    "a respawn, %u on an island we left, %u otherwise; %u of them with another "
		    "player within %.0f m, %u handed on",
		    static_cast<unsigned>((now - g_reapsSaidMs) / 1000), total,
		    t.by[static_cast<size_t>(CarReap::Stopped)], t.by[static_cast<size_t>(CarReap::Far)],
		    t.by[static_cast<size_t>(CarReap::Faded)], t.by[static_cast<size_t>(CarReap::Wreck)],
		    t.by[static_cast<size_t>(CarReap::Respawn)],
		    t.by[static_cast<size_t>(CarReap::Island)],
		    t.by[static_cast<size_t>(CarReap::Other)], t.nearby,
		    static_cast<double>(AMBIENT_CAR_KEEP_RADIUS_M), t.handed);
	const PedDropTally p = g_pedDrops;
	const uint32_t     pedTotal =
	    p.by[static_cast<size_t>(PedDrop::Reaper)] + p.by[static_cast<size_t>(PedDrop::Respawn)] +
	    p.by[static_cast<size_t>(PedDrop::Island)];
	if (pedTotal != 0)
		Log("population: over the last %us our engine dropped %u of our pedestrian(s) for "
		    "our player's sake - %u walked away from, %u on a respawn, %u on an island we "
		    "left; %u of them with another player within %.0f m, %u handed on",
		    static_cast<unsigned>((now - g_reapsSaidMs) / 1000), pedTotal,
		    p.by[static_cast<size_t>(PedDrop::Reaper)],
		    p.by[static_cast<size_t>(PedDrop::Respawn)],
		    p.by[static_cast<size_t>(PedDrop::Island)], p.nearby,
		    static_cast<double>(AMBIENT_PED_KEEP_RADIUS_M), p.handed);
	g_reaps       = ReapTally{};
	g_pedDrops    = PedDropTally{};
	g_reapsSaidMs = now;
}

// A hosted car of ours on its way out of the world, from the Remove detour.
// True when it went out as a let-go, and then the record is already cleared;
// false, and the caller despawns it as always.
//
// The pedestrians of ours sitting in it go with it: ~CVehicle (0x00551060)
// calls FlagToDestroyWhenNextProcessed through vtable slot 16 on the driver
// (+0x1A4) and on each passenger up to m_nNumMaxPassengers (+0x1CC), so our
// engine deletes them on its next CWorld::Process whatever the session does.
// Their records are let go of here, silently - the session decides them with
// the car, and a despawn of each on the next frame would only be refused.
bool LetGoHostedCar(HostedCar &c, void *vehicle, CarReap reap, uintptr_t ret) {
	const Vec3 at = ReadVec3(vehicle, offs::POSITION);
	Vec3       pts[MAX_PLAYERS];
	for (uint32_t i = 0; i < g_viewerCount; ++i)
		pts[i] = g_viewers[i].pos;
	const float nearD2 = NearestFlatD2(at.x, at.y, pts, g_viewerCount);
	const bool  nearby = AnybodyToHandTo(nearD2);
	bool        letGo  = ShouldLetGo(reap, c.named, c.mission, nearD2);
	if (letGo && g_letGoCount >= MAX_LET_GO) {
		letGo = false;
		if (!g_warnedLetGoFull) {
			g_warnedLetGoFull = true;
			Log("population: the let-go queue is full; a car our engine dropped beside "
			    "somebody is despawned instead (and this will not be said again)");
		}
	}

	++g_reaps.by[static_cast<size_t>(reap)];
	if (nearby)
		++g_reaps.nearby;
	if (letGo)
		++g_reaps.handed;
	if (nearby && g_reapsDetailed < REAP_DETAIL_LINES) {
		++g_reapsDetailed;
		const char *outcome =
		    letGo                        ? "asking the session to hand it to somebody near it"
		    : !c.named                   ? "despawned: the session had not named it yet"
		    : c.mission                  ? "despawned: the mission's own"
		    : reap == CarReap::Wreck     ? "despawned: a wreck is nobody's to keep"
		    : reap == CarReap::Other     ? "despawned: not the distance reaper's drop"
		                                 : "despawned";
		DescribeReap(c, at, reap, ret, outcome);
		if (g_reapsDetailed == REAP_DETAIL_LINES)
			Log("population: that is the last car described one by one; the rest are counted "
			    "every %u s", REAP_SUMMARY_MS / 1000);
	}
	// Its people a respawn's clear took ahead of it go its way, whichever that
	// is (SeatedDrop).
	auto seatedDrops = [vehicle](auto &&each) {
		for (uint32_t i = 0; i < g_seatedDropCount; ++i) {
			SeatedDrop &d = g_seatedDrops[i];
			if (d.netId == INVALID_NETID || d.vehicle != vehicle)
				continue;
			each(d.netId);
			d.netId = INVALID_NETID;
		}
	};
	if (!letGo) {
		seatedDrops([](uint16_t netId) { QueueLostNetId(netId); });
		return false;
	}

	LocalCarLetGo &out = g_letGo[g_letGoCount++];
	out       = LocalCarLetGo{};
	out.netId = c.netId;
	seatedDrops([&out](uint16_t netId) {
		if (out.pedCount < MAX_LET_GO_PEDS)
			out.peds[out.pedCount++] = netId;
		else
			QueueLostNetId(netId);
	});
	auto take = [&out](void *ped) {
		if (!ped)
			return;
		HostedPed *h = FindHostedByPed(ped);
		if (!h || !h->named || h->mission)
			return;
		if (out.pedCount < MAX_LET_GO_PEDS)
			out.peds[out.pedCount++] = h->netId;
		*h = HostedPed{};
	};
	take(Field<void *>(vehicle, offs::VEH_DRIVER));
	uint8_t seats = Field<uint8_t>(vehicle, offs::VEH_NUM_MAX_PASSENGERS);
	if (seats > offs::VEH_MAX_PASSENGERS)
		seats = static_cast<uint8_t>(offs::VEH_MAX_PASSENGERS);
	for (uint8_t i = 0; i < seats; ++i)
		take(Field<void *>(vehicle, offs::VEH_PASSENGERS + 4u * i));
	c = HostedCar{};
	return true;
}

uint32_t DrainLetGoAmbientCars(LocalCarLetGo *out, uint32_t max) {
	const uint32_t n = g_letGoCount < max ? g_letGoCount : max;
	for (uint32_t i = 0; i < n; ++i)
		out[i] = g_letGo[i];
	for (uint32_t i = n; i < g_letGoCount; ++i)
		g_letGo[i - n] = g_letGo[i];
	g_letGoCount -= n;
	if (g_letGoCount == 0)
		g_warnedLetGoFull = false;
	return n;
}

// A hosted pedestrian of ours on his way out of the world, from the Remove
// detour. True when he went out as a let-go, or is waiting for his car to be
// decided (SeatedDrop), and then the record is already cleared; false, and
// the caller despawns him as always.
bool LetGoHostedPed(HostedPed &h, void *ped, PedDrop drop) {
	if (drop == PedDrop::Other)
		return false;
	const Vec3     at      = ReadVec3(ped, offs::POSITION);
	const uint32_t state   = Field<uint32_t>(ped, offs::PED_STATE);
	const bool     alive   = state != PEDSTATE_DIE && state != PEDSTATE_DEAD;
	const uint8_t  pedType = static_cast<uint8_t>(Field<uint32_t>(ped, offs::PED_TYPE));
	const float    nearD2  = NearestViewerD2(at);
	++g_pedDrops.by[static_cast<size_t>(drop)];
	if (AnybodyToHandPedTo(nearD2))
		++g_pedDrops.nearby;

	// Sitting in a car of ours a respawn's clear is about to decide.
	void *const car = Field<bool>(ped, offs::PED_IN_VEHICLE)
	                      ? Field<void *>(ped, offs::PED_MY_VEHICLE)
	                      : nullptr;
	if (drop == PedDrop::Respawn && car && h.named && !h.mission && FindHostedCar(car) &&
	    g_seatedDropCount < MAX_SEATED_DROPS) {
		SeatedDrop &d = g_seatedDrops[g_seatedDropCount++];
		d.netId   = h.netId;
		d.vehicle = car;
		d.at      = at;
		d.pedType = pedType;
		d.alive   = alive;
		h         = HostedPed{};
		return true;
	}

	const bool letGo = ShouldLetGoPed(drop, h.named, h.mission, alive, pedType, nearD2);
	if (AnybodyToHandPedTo(nearD2))
		DescribePedDrop(h.netId, h.modelId, at, drop, nearD2, letGo);
	if (!letGo || !QueuePedLetGo(h.netId))
		return false;
	++g_pedDrops.handed;
	if (PopTrace())
		Log("population/trace: host let go ped %p net %u model %u (%s)", ped, h.netId,
		    h.modelId, PedDropName(drop));
	h = HostedPed{};
	return true;
}

uint32_t DrainLetGoAmbientPeds(uint16_t *out, uint32_t max) {
	SettleSeatedDrops();
	const uint32_t n = g_pedLetGoCount < max ? g_pedLetGoCount : max;
	for (uint32_t i = 0; i < n; ++i)
		out[i] = g_pedLetGo[i];
	for (uint32_t i = n; i < g_pedLetGoCount; ++i)
		g_pedLetGo[i - n] = g_pedLetGo[i];
	g_pedLetGoCount -= n;
	if (g_pedLetGoCount == 0)
		g_warnedPedLetGoFull = false;
	return n;
}

// ---- what the session's mission made, kept on the session ----------------------
//
// A pedestrian or car the owner's mission makes is hosted because the engine
// adds it inside one of the mission's instructions (MissionMakingEntities).
// The engine also takes an entity out of the world and puts it back by itself,
// for reasons that are no instruction of anybody's: CPed::Teleport and
// CAutomobile::Teleport are CWorld::Remove then CWorld::Add, and
// CWorld::RemoveFallenPeds and RemoveFallenCars call them; a pedestrian
// sitting down in a car is re-filed the same way (re3 Ped.cpp,
// PedSetInCarCB). The Remove ends the hosting, and the Add behind it is not
// inside an instruction, so the entity used to stay this machine's alone from
// then on and disappear from every other screen. Every one the mission made
// is kept here, by its pool reference, until it is gone from its pool: an Add
// of one of them is hosted whoever makes it, and one that is in the pool and
// hosted by nobody is put back on the session (KeepMissionEntitiesHosted).
struct MissionEntity {
	void    *entity         = nullptr;
	int32_t  ref            = -1;
	bool     car            = false;
	uint16_t modelId        = 0;
	uint32_t missingSinceMs = 0;   // 0 while it is hosted
	uint32_t hostedAgain    = 0;
};
constexpr size_t MAX_MISSION_ENTITIES = 96;
MissionEntity    g_missionEntities[MAX_MISSION_ENTITIES];
size_t           g_missionEntityCount     = 0;
bool             g_saidMissionEntitiesFull = false;
// How long one may be hosted by nobody before it is announced again: the
// frames between a Remove and its Add, or a promotion's round trip, are not
// worth a second netId.
constexpr uint32_t MISSION_ENTITY_GRACE_MS = 500;

void *ResolveRef(int32_t ref, bool car) {
	if (ref < 0)
		return nullptr;
	return car ? AmbientCarFromRef(ref) : Func<GetPedFn>(CPools__GetPed)(ref);
}

MissionEntity *FindMissionEntity(const void *entity) {
	for (size_t i = 0; i < g_missionEntityCount; ++i)
		if (g_missionEntities[i].entity == entity)
			return &g_missionEntities[i];
	return nullptr;
}

// The same object the mission made, not whatever the pool put at its address
// since: the reference carries the slot's generation.
bool IsMissionEntity(const void *entity, bool car) {
	const MissionEntity *m = FindMissionEntity(entity);
	return m && m->car == car && ResolveRef(m->ref, car) == entity;
}

void NoteMissionEntity(void *entity, int32_t ref, bool car, uint16_t modelId) {
	if (MissionEntity *m = FindMissionEntity(entity)) {
		m->ref            = ref;
		m->car            = car;
		m->modelId        = modelId;
		m->missingSinceMs = 0;
		return;
	}
	if (g_missionEntityCount >= MAX_MISSION_ENTITIES) {
		if (!g_saidMissionEntitiesFull) {
			g_saidMissionEntitiesFull = true;
			Log("population: the mission has made more than %zu pedestrians and cars; the rest "
			    "are not watched for leaving the session",
			    MAX_MISSION_ENTITIES);
		}
		return;
	}
	MissionEntity &m = g_missionEntities[g_missionEntityCount++];
	m                = MissionEntity{};
	m.entity         = entity;
	m.ref            = ref;
	m.car            = car;
	m.modelId        = modelId;
}

void ForgetMissionEntityAt(size_t i) {
	g_missionEntities[i] = g_missionEntities[--g_missionEntityCount];
}

// Somebody's claim owns it now, a session car: not the mission's to announce.
void ForgetMissionEntity(const void *entity) {
	for (size_t i = 0; i < g_missionEntityCount; ++i)
		if (g_missionEntities[i].entity == entity) {
			ForgetMissionEntityAt(i);
			return;
		}
}

// Is this something the session should know about?
//
// Three tests, and the middle one is the whole filter:
//
//   - a vehicle, by the engine's own m_type bits
//   - created by the traffic generator, which is what RANDOM_VEHICLE means.
//     A PARKED_VEHICLE is scenery the map places the same way on every
//     machine and is left alone; a MISSION_VEHICLE belongs to the campaign,
//     which runs on the host (docs/campaign.md); a PERMANENT_VEHICLE is a
//     car a player claimed and the M2 path already owns it.
//   - not a car the local player is already sitting in, which a freshly
//     added one never is, but the test costs nothing and closes the case
//     where the engine adds a car back into the world under a driver.
//
// A CoopIII replica of somebody else's traffic is also RANDOM_VEHICLE, on
// purpose (game/vehicle.cpp, SpawnAmbientCarReplica) - so the Add that
// registers one *would* pass all three. What keeps it out is the
// ReplicaScope around that Add, which is the same guard the ped side uses and
// the reason that guard is a shared type rather than a local flag.
//
// **And a fourth test, which is the one the wreck report rests on.**
// ReplicaScope only covers the moment CoopIII calls CWorld::Add; it says
// nothing about a later Add on the same object. A replica that ever got
// adopted into g_hostedCars would be announced to the session as this
// machine's own traffic *and* would get to report its own destruction - a
// machine deciding that somebody else's car died because its own copy of it
// blew up here. So the refusal is a property of the object rather than of a
// moment: every replica is created with bIsLocked set, because CanBeDeleted
// is open for a RANDOM_VEHICLE and bIsLocked carries the whole reaping gate
// alone (docs/population.md §1.3.2), and the engine's traffic generator
// never locks a car it made - a locked one could never be recycled and the
// streets would fill up and stay full.
//
// Measured rather than assumed, because the cheap answer would have been to
// trust the scope: CPhysical::RemoveAndAdd (0x00495540), which the per-frame
// correction calls through PlaceVehicle, does **not** call CWorld::Add. It
// re-files the entity in the sector grid itself, recycling its own entry-info
// nodes - the only calls in the whole function are the entry-info pool's
// alloc/free (0x004A3DD0 / 0x004A3DE0) and 0x00475A40 / 0x00475A50, and
// CWorld::Add is 0x004AE930. So no replica walks back through the Add detour
// today. The flag test is what makes that a fact about this file rather than
// a fact about that function.
//
// **And the session's mission's cars pass too**, for the reason a mission
// pedestrian does (IsAmbientPedWeShouldHost): a MISSION_VEHICLE added while
// the session's mission runs one of its instructions here. The script may
// well lock one, and a replica cannot be what is being added inside one of
// its instructions, so the lock test is not asked of them.
bool IsMissionCarWeShouldHost(void *entity) {
	return MissionMakingEntities() &&
	       Field<uint8_t>(entity, offs::VEH_CREATED_BY) == VEHICLE_CREATED_BY_MISSION;
}

bool IsAmbientCarWeShouldHost(void *entity) {
	if (!entity)
		return false;
	if (EntityType(entity) != ENTITY_TYPE_VEHICLE)
		return false;
	if (Field<void *>(entity, offs::VEH_DRIVER) == PlayerPed())
		return false;
	if (IsMissionCarWeShouldHost(entity))
		return true;
	if (Field<uint8_t>(entity, offs::VEH_CREATED_BY) != VEHICLE_CREATED_BY_RANDOM)
		return false;
	if ((Field<uint8_t>(entity, offs::VEH_FLAGS_A) & offs::VEH_IS_LOCKED) != 0)
		return false;
	return true;
}

// What became of an attempt to host a car or a pedestrian here.
enum class Hosting : uint8_t { Hosted, Already, TableFull, QueueFull, NoModel };

const char *HostingWhy(Hosting h) {
	switch (h) {
	case Hosting::Hosted:    return "hosted";
	case Hosting::Already:   return "hosted already";
	case Hosting::TableFull: return "the table of what this machine hosts is full";
	case Hosting::QueueFull: return "the queue of announcements is full";
	case Hosting::NoModel:   return "it has no model index yet";
	}
	return "?";
}

// A car into this machine's hosting, the session to be told of it at the next
// drain. `mission` marks it as the session's mission's own (AMBIENT_MISSION).
Hosting HostCar(void *entity, bool mission) {
	if (HostedCar *old = FindHostedCar(entity)) {
		if (AmbientCarFromRef(old->poolHandle) == entity) {
			if (mission && !old->mission)
				old->mission = true;
			return Hosting::Already;
		}
		// A record for a car that went without the Remove detour seeing it,
		// whose pool slot this car has taken since. It used to stop this car
		// being hosted at all, with nothing said.
		Log("population: a hosted car at %p had gone without being removed, and another car is "
		    "at its address now; the old one is let go and the new one hosted",
		    entity);
		QueueLostCar(*old, "its pool slot was reused");
	}

	HostedCar *slot = nullptr;
	for (HostedCar &c : g_hostedCars)
		if (!c.active) {
			slot = &c;
			break;
		}
	if (!slot) {
		if (!g_warnedHostedCarsFull) {
			g_warnedHostedCarsFull = true;
			Log("population: hosting %zu ambient cars already; the rest stay "
			    "local to this machine (and this will not be said again)",
			    MAX_HOSTED_CARS);
		}
		return Hosting::TableFull;
	}

	if (g_bornCarCount >= MAX_QUEUED_CARS) {
		if (!g_warnedBornCarsFull) {
			g_warnedBornCarsFull = true;
			Log("population: the new-car queue is full; dropping claims until "
			    "it drains (and this will not be said again)");
		}
		return Hosting::QueueFull;
	}

	AmbientCarBody body{};
	if (!SampleAmbientCarIdentity(entity, body))
		return Hosting::NoModel;   // no model index yet; not a car worth announcing

	*slot            = HostedCar{};
	slot->active     = true;
	slot->vehicle    = entity;
	slot->poolHandle = AmbientCarRef(entity);
	slot->mission    = mission;
	slot->modelId    = body.modelId;
	slot->tempId     = g_nextCarTempId++;
	// 0 is what a backfilled S_CarSpawn carries and must never be a real
	// claim.
	if (g_nextCarTempId == 0)
		g_nextCarTempId = 1;

	LocalAmbientCar &out = g_bornCars[g_bornCarCount++];
	out.tempId     = slot->tempId;
	out.body       = body;
	out.body.flags = mission ? AMBIENT_MISSION : 0;
	if (mission)
		NoteMissionEntity(entity, slot->poolHandle, true, body.modelId);
	return Hosting::Hosted;
}

// One of the mission's own cars coming back into the world after the engine
// took it out, whoever puts it back (the MissionEntity note above). Not one the
// local player is driving, which is the claim path's.
bool ReturningMissionCar(void *entity) {
	return OwnMissionRunning() && EntityType(entity) == ENTITY_TYPE_VEHICLE &&
	       IsMissionEntity(entity, true) && Field<void *>(entity, offs::VEH_DRIVER) != PlayerPed();
}

// What became of every car this machine's own engine added to the world since
// the last crowd report. A machine that hosts no traffic at all looks, on every
// other screen, like a machine whose drivers float down the road in cars that
// are not there - and until these existed its log said nothing about why.
struct CarAddTally {
	uint32_t hosted    = 0;
	uint32_t already   = 0;
	uint32_t notRandom = 0;   // parked, permanent, a mission's not being hosted
	uint32_t locked    = 0;
	uint32_t player    = 0;   // the local player at its wheel
	uint32_t noModel   = 0;
	uint32_t full      = 0;   // table or announcement queue
};
CarAddTally g_carAdds;
bool        g_saidCarNoModel = false;

void TallyCarRefused(void *entity) {
	if (Field<void *>(entity, offs::VEH_DRIVER) == PlayerPed())
		++g_carAdds.player;
	else if (Field<uint8_t>(entity, offs::VEH_CREATED_BY) != VEHICLE_CREATED_BY_RANDOM)
		++g_carAdds.notRandom;
	else
		++g_carAdds.locked;
}

void TallyCarHosting(void *entity, Hosting h) {
	switch (h) {
	case Hosting::Hosted:    ++g_carAdds.hosted; break;
	case Hosting::Already:   ++g_carAdds.already; break;
	case Hosting::TableFull:
	case Hosting::QueueFull: ++g_carAdds.full; break;
	case Hosting::NoModel:
		++g_carAdds.noModel;
		if (!g_saidCarNoModel) {
			g_saidCarNoModel = true;
			Log("population: our engine added car %p and it is not hosted - no model index "
			    "(the dword at +0x5C reads %08X, created by %u); every other screen goes "
			    "without it (said once)",
			    entity, static_cast<unsigned>(Field<uint32_t>(entity, offs::MODEL_INDEX)),
			    static_cast<unsigned>(Field<uint8_t>(entity, offs::VEH_CREATED_BY)));
		}
		break;
	}
}

void ReportCarAdds() {
	const CarAddTally t = g_carAdds;
	g_carAdds           = CarAddTally{};
	if (t.locked + t.noModel + t.full == 0)
		return;
	Log("population: since the last report our engine added %u car(s) - %u hosted, %u hosted "
	    "already, %u not traffic, %u at our wheel, and NOT hosted though traffic: %u locked, "
	    "%u with no model index, %u for a full table",
	    t.hosted + t.already + t.notRandom + t.player + t.locked + t.noModel + t.full,
	    t.hosted, t.already, t.notRandom, t.player, t.locked, t.noModel, t.full);
}

// The car half of the Add detour. Called from AddHook after the engine's own
// work, on an entity that is already properly in the world.
void NoteCarAdded(void *entity) {
	bool mission = false;
	if (IsAmbientCarWeShouldHost(entity))
		mission = IsMissionCarWeShouldHost(entity);
	else if (ReturningMissionCar(entity))
		mission = true;
	else {
		TallyCarRefused(entity);
		return;
	}
	const Hosting h = HostCar(entity, mission);
	TallyCarHosting(entity, h);
	if (mission && h != Hosting::Hosted && h != Hosting::Already)
		Log("population: the mission's car at %p (model %u) was added to the world and is not "
		    "hosted: %s",
		    entity, static_cast<unsigned>(Field<uint32_t>(entity, offs::MODEL_INDEX) & 0xFFFF),
		    HostingWhy(h));
}

// The authority, and the answer to the Add/Remove asymmetry, for cars.
//
// Two tests rather than the ped sweep's one. The handle has to still resolve
// to the same object, as before - and the car has to still be one this
// machine may speak for.
//
// That second test is what happens when the local player gets into a car this
// machine was hosting as traffic. From that moment two different parts of
// CoopIII believe they own it: this seam, which is streaming its transform to
// everybody, and the M2 claim path, which is about to introduce the same car
// to the session under a netId of its own. Retiring it from ambient hosting
// is the cheap half of that argument and the right one - the claim path is
// older, it carries the driver, the seat and the damage, and this seam only
// ever carried a transform. Every observer drops the replica and builds the
// claimed car instead, which is one flicker rather than two cars.
void SweepHostedCars() {
	void *const player = PlayerPed();
	ReportReaps();

	for (HostedCar &c : g_hostedCars) {
		if (!c.active)
			continue;

		void *const now =
		    c.poolHandle >= 0 ? AmbientCarFromRef(c.poolHandle) : nullptr;
		if (now != c.vehicle) {
			QueueLostCar(c, "gone from the vehicle pool");
			continue;
		}
		if (player && Field<void *>(now, offs::VEH_DRIVER) == player) {
			Log("population: the local player got into ambient car %u; handing "
			    "it to the vehicle claim path", c.netId);
			ForgetMissionEntity(c.vehicle);
			QueueLostCar(c, "the local player took its wheel");
			continue;
		}

		// A car this machine hosts that this machine's engine has destroyed.
		//
		// Polled rather than hooked, and the poll is the better instrument
		// here. game/vehicle.cpp already detours both BlowUpCar bodies, but
		// this file does not own that detour and, more to the point, a detour
		// answers "BlowUpCar was called" while the question is "is this car a
		// wreck" - which is the status byte, whichever of the three callers
		// got there (CVehicle::InflictDamage, the fire timer,
		// ProcessDelayedExplosion) and whether or not bCanBeDamaged let the
		// call do anything. It costs a load, a shift and a compare per hosted
		// car per frame.
		//
		// Only a car the session has named. One that is wrecked before its
		// netId comes back keeps its status, so the next sweep after
		// NameLocalAmbientCar reports it - the condition is a fact about the
		// car, not an edge it could miss.
		if (c.named && !c.reportedWreck && IsWreckedCar(now)) {
			c.reportedWreck = true;
			PushAmbientWreck(c.netId, now);
		}
	}
}

// ---- the bridge, car half --------------------------------------------------

uint32_t DrainLocalAmbientCars(LocalAmbientCar *out, uint32_t max) {
	const uint32_t n = g_bornCarCount < max ? g_bornCarCount : max;
	for (uint32_t i = 0; i < n; ++i) {
		out[i] = g_bornCars[i];
		// What the car looks like now, not at the CWorld::Add that noticed it.
		// A mission's CREATE_CAR adds the car inside its own instruction and
		// the script's next ones, run in the same frame, paint it and turn it
		// (Give Me Liberty: CREATE_CAR, CHANGE_CAR_COLOUR 58 1, SET_CAR_HEADING).
		// Sampled at the Add, the Kuruma went out in whatever colours the
		// constructor rolled, and the colour instruction behind it reached the
		// others before their copy existed and was dropped: a red Kuruma on
		// one screen and a blue one on the other. This runs after
		// CGame::Process, so the script's frame is over.
		if (HostedCar *c = FindHostedCarByTempId(out[i].tempId)) {
			void *const now = c->poolHandle >= 0 ? AmbientCarFromRef(c->poolHandle) : nullptr;
			AmbientCarBody body{};
			if (now != nullptr && now == c->vehicle && SampleAmbientCarIdentity(now, body)) {
				body.flags  = out[i].body.flags;
				out[i].body = body;
			}
		}
	}
	for (uint32_t i = n; i < g_bornCarCount; ++i)
		g_bornCars[i - n] = g_bornCars[i];
	g_bornCarCount -= n;
	if (g_bornCarCount == 0)
		g_warnedBornCarsFull = false;
	return n;
}

uint32_t DrainLostAmbientCars(uint16_t *out, uint32_t max) {
	// The sweep runs here, for the same reason the ped sweep runs in its
	// drain: this is already called once a frame and what the sweep finds is
	// exactly what this hands over.
	SweepHostedCars();

	const uint32_t n = g_lostCarCount < max ? g_lostCarCount : max;
	for (uint32_t i = 0; i < n; ++i)
		out[i] = g_lostCars[i];
	for (uint32_t i = n; i < g_lostCarCount; ++i)
		g_lostCars[i - n] = g_lostCars[i];
	g_lostCarCount -= n;
	if (g_lostCarCount == 0)
		g_warnedLostCarsFull = false;
	return n;
}

// Our own traffic cars that have taken a dent the session has not heard
// about, the way SendLocalVehicleDamage does it for the car we drive: absolute
// words, only when they grew. A cursor walks the table so one pile-up does
// not keep the rest waiting behind it.
uint32_t g_carDamageCursor = 0;
bool     g_saidCarDamageSent = false;

uint32_t DrainHostedCarDamage(VehicleDamageBody *out, uint32_t max) {
	uint32_t n = 0;
	for (uint32_t step = 0; step < MAX_HOSTED_CARS && n < max; ++step) {
		HostedCar &c = g_hostedCars[(g_carDamageCursor + step) % MAX_HOSTED_CARS];
		if (!c.active || !c.named || c.reportedWreck || c.poolHandle < 0)
			continue;
		if (AmbientCarFromRef(c.poolHandle) != c.vehicle)
			continue;   // SweepHostedCars will say it went

		VehicleDamageBody now{};
		if (!SampleHostedCarDamage(c.poolHandle, now))
			continue;
		if (!DamageGrew(c.sentPanels, c.sentDoors, now.panels, now.doors))
			continue;
		MergeDamage(c.sentPanels, c.sentDoors, now.panels, now.doors);

		out[n]        = VehicleDamageBody{};
		out[n].netId  = c.netId;
		out[n].panels = c.sentPanels;
		out[n].doors  = c.sentDoors;
		++n;
	}
	g_carDamageCursor = (g_carDamageCursor + 1) % MAX_HOSTED_CARS;

	if (n != 0 && !g_saidCarDamageSent) {
		g_saidCarDamageSent = true;
		Log("population: our traffic car %u took our first dent the session had "
		    "not heard about (panels %08X doors %04X); telling it",
		    out[0].netId, static_cast<unsigned>(out[0].panels),
		    static_cast<unsigned>(out[0].doors));
	}
	return n;
}

uint8_t DrainAmbientWrecks(UnownedBlast *out, uint8_t max) {
	const uint8_t n = g_ambientWrecks.Drain(out, max);

	// Once, the first time it works, and this project has paid for that rule
	// twice: a seam that only ever logs its failures looks identical to a
	// seam nobody installed. "A traffic car burned out here and stayed whole
	// on every other screen" is precisely the symptom this exists to end.
	if (n != 0 && !g_saidAmbientWreckSent) {
		g_saidAmbientWreckSent = true;
		Log("population: ambient car %u burned out here; telling the session",
		    static_cast<unsigned>(out[0].key.id));
	}
	return n;
}

// The other end of it: somebody else's hosted car died and this machine holds
// a replica of it.
//
// Through the engine's own BlowUpCar, off the object's own vtable, for the
// reason every other replay in this codebase goes that way - the blast, the
// burnt shell, the flagged occupants and the sound are decided in one
// function, and CoopIII inventing any of them separately gets them subtly
// wrong. No health is written into the car first: writing health destroys
// nothing, and a low one arms the five-second fire timer, which is an
// observer deciding to destroy somebody else's car later.
//
// The transform *is* written, and the first version of this function had it
// the other way round on an argument that play refuted - see the version 16
// note in protocol.h. The short version: a host stops streaming a car the
// frame it becomes a wreck, so the newest state a replica holds is from
// before the explosion, and the replica renders an interpolation buffer
// behind even that. The two errors compound and the replica detonated in the
// wrong lane.
UnownedWreckOutcome WreckAmbientCarReplica(RemoteAmbientCar &car,
                                           const BlastTransform &where) {
	if (car.poolHandle < 0)
		return UnownedWreckOutcome::NotHere;   // model still streaming in

	void *const v = AmbientCarFromRef(car.poolHandle);
	if (!v) {
		// Gone from the pool under us. Same recovery CorrectAmbientCarReplica
		// does, minus the re-arm: Client marks the row destroyed, so nothing
		// builds this car again.
		car.poolHandle = -1;
		return UnownedWreckOutcome::NotHere;
	}

	// The ordinary answer, and the one that says the explosion replay did the
	// work: our own engine got there first.
	if (IsWreckedCar(v))
		return UnownedWreckOutcome::Already;

	if (!Field<void *>(v, 0))
		return UnownedWreckOutcome::NotHere;

	// Placed first, then blown up. Everything BlowUpCar decides - where the
	// explosion goes off, which way the camera shakes, where the fire burns -
	// it reads out of the car's own matrix, so this has to happen before the
	// call and not after it.
	PlaceCarForBlast(v, where);

	// Through game/vehicle.cpp, because its BlowUpCar detour refuses a replica
	// unless the call says it comes from the host (docs/protocol.md §1.23).
	// Null culprit, the same thing the script's own BLOW_UP_CAR passes.
	if (!BlowUpCarAsOwnerSaid(v))
		return UnownedWreckOutcome::NotHere;

	// BlowUpCar returns having done nothing when bCanBeDamaged is clear,
	// which the campaign uses during cutscenes. Saying "wrecked" then would
	// retire the instruction with the car still whole, so the answer is "not
	// now" and the loop asks again next frame.
	if (!IsWreckedCar(v))
		return UnownedWreckOutcome::NotHere;

	if (!g_saidAmbientWreckApplied) {
		g_saidAmbientWreckApplied = true;
		Log("population: blew up ambient car %u because the machine hosting it "
		    "did", static_cast<unsigned>(car.netId));
	}
	return UnownedWreckOutcome::Wrecked;
}

bool NameLocalAmbientCar(uint32_t tempId, uint16_t netId) {
	HostedCar *c = FindHostedCarByTempId(tempId);
	if (!c)
		return false;   // recycled while the round trip was in flight

	c->netId = netId;
	c->named = true;

	// And check it is still there, for the reason NameLocalAmbientPed does:
	// the sweep runs once a frame and the record could have been stale for
	// most of this one. Traffic reaches this more often than pedestrians do -
	// CCarCtrl removes a car as soon as it is far enough behind.
	void *const now = c->poolHandle >= 0 ? AmbientCarFromRef(c->poolHandle) : nullptr;
	if (now == c->vehicle) {
		// The one line that says a mission's car reached the session, from
		// the machine that made it. Its absence on the owner is what the
		// participants' "built here" lines cannot tell apart.
		static uint32_t said = 0;
		if (c->mission && said < 16) {
			++said;
			Log("population: the mission's car (model %u, temp %u) is car %u in the session",
			    static_cast<unsigned>(c->modelId), tempId, netId);
		}
		return true;
	}

	if (c->mission)
		Log("population: the mission's car (model %u, temp %u) was named %u after it had gone "
		    "from the pool",
		    static_cast<unsigned>(c->modelId), tempId, netId);
	*c = HostedCar{};
	return false;
}

// This tick's car batch.
//
// docs/population.md §2.1, and game/streampick.h is the rule: every hosted car
// takes its turn, more often the nearer it is to another player. This used to
// be the eight nearest our own player, with no memory between ticks, which
// sent the same eight every time and left the ninth frozen on every other
// screen for as long as we hosted it - and ranked by the one player who never
// reads the batch.
//
// A honking car goes out every tick, because its replica lets the honk lapse
// HORN_FRESH_MS after the last row that said so, and a car whose health, siren
// or horn just changed goes out on the next one.
uint32_t SampleHostedCars(AmbientCarState *out, uint32_t max, const Vec3 *viewers,
                          uint32_t viewerCount, uint8_t &hornMask, uint8_t &sirenMask) {
	hornMask  = 0;
	sirenMask = 0;
	if (max == 0)
		return 0;
	const uint32_t want = max < MAX_CAR_STATES ? max : MAX_CAR_STATES;

	AmbientCarState rows[MAX_HOSTED_CARS];
	HostedCar      *hosts[MAX_HOSTED_CARS];
	bool            horns[MAX_HOSTED_CARS];
	bool            sirens[MAX_HOSTED_CARS];
	StreamCandidate cand[MAX_HOSTED_CARS];
	uint32_t        n = 0;

	for (HostedCar &c : g_hostedCars) {
		if (!c.active || !c.named || c.netId == INVALID_NETID)
			continue;
		void *const v = c.poolHandle >= 0 ? AmbientCarFromRef(c.poolHandle) : nullptr;
		if (v != c.vehicle)
			continue;   // the sweep will deal with it; nothing to send

		rows[n] = AmbientCarState{};
		if (!SampleHostedCar(c.poolHandle, rows[n]))
			continue;
		rows[n].netId = c.netId;
		// What its driver is doing with the wheel and the pedals, which the
		// replica shows as front wheels, brake lights and an engine note.
		rows[n].steer = EncodeCarSteer(Field<float>(v, offs::VEH_STEER_ANGLE));
		rows[n].gas   = EncodeCarGas(Field<float>(v, offs::VEH_GAS_PEDAL));
		rows[n].brake = EncodeCarBrake(Field<float>(v, offs::VEH_BRAKE_PEDAL));
		hosts[n]      = &c;
		horns[n]      = HostedCarHonking(c.poolHandle);
		sirens[n]     = HostedCarSirenOn(c.poolHandle);

		StreamCandidate &k = cand[n];
		k          = StreamCandidate{};
		k.row      = &c.stream;
		k.index    = n;
		k.says     = CarRowSays(rows[n].health, sirens[n], horns[n]);
		k.deadline = horns[n] ? STREAM_HONK_DEADLINE : uint8_t{0};
		const bool seen = NearestViewerDist2(rows[n].pos, viewers, viewerCount, k.dist2);
		k.weight   = StreamWeight(seen, k.dist2);
		++n;
	}

	const uint32_t written = PickStreamRows(cand, n, want);
	for (uint32_t i = 0; i < written; ++i) {
		const uint32_t j = cand[i].index;
		out[i] = rows[j];
		if (horns[j])
			hornMask |= CarStateHornBit(static_cast<uint8_t>(i));
		if (sirens[j])
			sirenMask |= CarStateSirenBit(static_cast<uint8_t>(i));
		hosts[j]->rowSinceReport = true;
		if (cand[i].waited > g_carLongestWait)
			g_carLongestWait = cand[i].waited;
	}

	if (n > want && !g_saidCarsTakeTurns) {
		g_saidCarsTakeTurns = true;
		Log("population: hosting %u named cars and a batch holds %u, so they take "
		    "turns now, the nearest another player most often (%u other player "
		    "position(s) known)", n, want, viewerCount);
	}

	// The same instrument the ped sampler carries, and the reason it is here
	// too rather than only there: a cost for one stream is a number nobody
	// can judge. Step 4 argued 3.5 KB/s for traffic from the design; this is
	// what the session actually sends.
	g_carRowsSent += written;
	if (written != 0)
		++g_carPacketsSent;
	return written;
}

// Thin wrappers, so the replica count the crowd report needs is kept in one
// place. The engine work is game/vehicle.cpp's, because everything it needs -
// the CREATE_CAR sequence, the extras override, the matrix push - already
// lives there.
bool SpawnAmbientCarReplicaCounted(RemoteAmbientCar &car) {
	if (car.poolHandle >= 0)
		return true;
	if (!SpawnAmbientCarReplica(car))
		return false;
	++g_carReplicas;
	return true;
}

void DespawnAmbientCarReplicaCounted(RemoteAmbientCar &car) {
	if (car.poolHandle < 0)
		return;
	DespawnAmbientCarReplica(car);
	if (g_carReplicas > 0)
		--g_carReplicas;
}


// What a birth says about a pedestrian: its model, its type and where it is.
void FillPedBirth(void *entity, AmbientPedBody &body) {
	body.modelId = static_cast<uint16_t>(Field<uint32_t>(entity, offs::MODEL_INDEX));
	body.pedType = static_cast<uint8_t>(Field<uint32_t>(entity, offs::PED_TYPE));
	body.flags   = 0;
	const float *const p = &Field<float>(entity, offs::POSITION);
	body.pos     = Vec3{p[0], p[1], p[2]};
	body.heading = Field<float>(entity, offs::PED_ROT_CUR);
}

// A pedestrian into this machine's hosting, the session to be told of it at
// the next drain. `mission` marks it as the session's mission's own.
Hosting HostPed(void *entity, bool mission) {
	if (HostedPed *old = FindHostedByPed(entity)) {
		if (Func<GetPedFn>(CPools__GetPed)(old->poolHandle) == entity) {
			if (mission && !old->mission)
				old->mission = true;
			return Hosting::Already;   // a second Add on one entity is its own bug
		}
		// The ped that was here went without the Remove detour letting go of
		// it (the mission's own are kept past a Remove), and this one has its
		// pool slot. It used to stop this one being hosted at all.
		QueueLost(*old, "its pool slot was reused");
	}

	HostedPed *slot = nullptr;
	for (HostedPed &h : g_hosted)
		if (!h.active) {
			slot = &h;
			break;
		}
	if (!slot) {
		if (!g_warnedHostedFull) {
			g_warnedHostedFull = true;
			Log("population: hosting %zu ambient peds already; the rest stay "
			    "local to this machine (and this will not be said again)",
			    MAX_HOSTED);
		}
		return Hosting::TableFull;
	}

	if (g_bornCount >= MAX_QUEUED) {
		if (!g_warnedBornFull) {
			g_warnedBornFull = true;
			Log("population: the new-ped queue is full; dropping claims until "
			    "it drains (and this will not be said again)");
		}
		return Hosting::QueueFull;
	}

	*slot            = HostedPed{};
	slot->active     = true;
	slot->ped        = entity;
	slot->poolHandle = Func<RefFn>(CPools__GetPedRef)(entity);
	slot->tempId     = g_nextTempId++;
	// 0 is what a backfilled S_PedSpawn carries and must never be a real
	// claim. Wrapping is not realistic at one per pedestrian, but the skip
	// costs one compare and removes the question.
	if (g_nextTempId == 0)
		g_nextTempId = 1;

	LocalAmbientPed &out = g_born[g_bornCount++];
	out.tempId       = slot->tempId;
	FillPedBirth(entity, out.body);
	slot->mission    = mission;
	out.body.flags   = mission ? AMBIENT_MISSION : 0;
	const float *const p = &Field<float>(entity, offs::POSITION);
	slot->modelId    = out.body.modelId;
	if (mission)
		NoteMissionEntity(entity, slot->poolHandle, false, slot->modelId);
	if (PopTrace())
		Log("population/trace: host add ped %p temp %u model %u type %u at "
		    "(%.1f %.1f %.1f)", entity, slot->tempId, out.body.modelId,
		    out.body.pedType, static_cast<double>(p[0]),
		    static_cast<double>(p[1]), static_cast<double>(p[2]));
	return Hosting::Hosted;
}

void __cdecl AddHook(void *entity) {
	// The engine's work happens first, always. Whatever this file decides,
	// it decides about an entity that is already properly in the world -
	// CWorld::Add files it into the sector grid and the moving list, and
	// nothing here is worth delaying that for.
	g_addDetour.Original<AddFn>()(entity);

	if (g_creatingReplica || !entity)
		return;

	// Cars walk through the same door, and step 2 filtered them out here
	// because it was peds only. This is step 4 (docs/population.md).
	if (EntityType(entity) == ENTITY_TYPE_VEHICLE) {
		NoteCarAdded(entity);
		return;
	}

	// One of the mission's own coming back into the world, whoever put it
	// back (the MissionEntity note above).
	bool mission = false;
	if (IsAmbientPedWeShouldHost(entity))
		mission = Field<uint8_t>(entity, offs::PED_CHAR_CREATED_BY) == CHAR_CREATED_BY_MISSION;
	else if (OwnMissionRunning() && EntityType(entity) == offs::ENTITY_TYPE_PED &&
	         IsMissionEntity(entity, false) && entity != PlayerPed())
		mission = true;
	else
		return;
	const Hosting h = HostPed(entity, mission);
	if (mission && h != Hosting::Hosted && h != Hosting::Already)
		Log("population: the mission's pedestrian at %p (model %u) was added to the world and is "
		    "not hosted: %s",
		    entity, static_cast<unsigned>(Field<uint32_t>(entity, offs::MODEL_INDEX) & 0xFFFF),
		    HostingWhy(h));
}

// Whether an entity handed to CWorld::Remove has already been through its
// destructor. CWorld::Remove's virtual Remove through what is left of it
// calls CPlaceable's slot 2, which is zero (game/teardown.h). This cannot make
// that call safe - whoever passed it will delete it next - but it can say who
// passed it, which the crash itself never does.
bool g_saidRemoveDead = false;

void NoteRemoveOfDeadEntity(void *entity, uintptr_t from) {
	if (g_saidRemoveDead || !entity)
		return;
	EntityState s = VehicleState(entity);
	if (s == EntityState::NotInPool)
		s = PedState(entity);
	if (s != EntityState::FreedSlot && s != EntityState::Destructed)
		return;
	g_saidRemoveDead = true;
	Log("world: CWorld::Remove was handed %p, which is %s, from 0x%08X; the engine is "
	    "about to call through the vtable its destructor left behind",
	    entity, EntityStateName(s), static_cast<unsigned>(from));
}

void __cdecl RemoveHook(void *entity) {
	NoteRemoveOfDeadEntity(entity, reinterpret_cast<uintptr_t>(_ReturnAddress()));
	// Noticed before the engine's own teardown, because ~CPed calls
	// CWorld::Remove as its first statement and by the time it returns the
	// object is on its way to being freed. Reading the pointer's identity is
	// all this needs and it does that here.
	//
	// Not for the session's mission's own, while it runs. The engine takes a
	// pedestrian or car out of the world and puts it straight back to move it
	// (CPed::Teleport, CAutomobile::Teleport, a pedestrian sitting down in a
	// car), and that is no reason for everybody else's copy to go and come
	// back under another name, with every instruction of the mission that
	// named the old one left pointing at nothing. The sweep still lets go of
	// one whose pool slot is freed, a frame later at most.
	if (entity && !g_creatingReplica) {
		const bool      keep = OwnMissionRunning();
		const uintptr_t ret  = reinterpret_cast<uintptr_t>(_ReturnAddress());
		// Our engine dropping him for our player's sake - walked away from,
		// a respawn, an island left - with somebody else beside him hands him
		// on (game/carletgo.h). Anything else is a despawn as always.
		if (HostedPed *h = FindHostedByPed(entity))
			if (!(keep && h->mission) && !LetGoHostedPed(*h, entity, DropOfPed(entity, ret)))
				QueueLost(*h, "CWorld::Remove");
		if (HostedCar *c = FindHostedCar(entity))
			if (!(keep && c->mission)) {
				// Where the engine called Remove from is the reason: the
				// distance reaper's four calls are told apart by it, and so
				// are the respawn's clear and the island's (game/carletgo.h).
				// Anything else is a despawn as always.
				const CarReap reap = ReapOfCar(entity, ret);
				if (!LetGoHostedCar(*c, entity, reap, ret))
					QueueLostCar(*c, "CWorld::Remove");
			}
	}
	g_removeDetour.Original<RemoveFn>()(entity);
}

// The authority, and the answer to the asymmetry in the header comment.
//
// CWorld::Remove is not guaranteed to run for everything that leaves, and it
// is not the only way a ped can stop being ours. So once a frame every hosted
// record is checked against the pool: the handle has to still resolve, and it
// has to still resolve to the same object. Either failing means the ped is
// gone, whatever route it took and whether or not Remove was ever called.
//
// This is cheap - a load and two compares per hosted ped, at most 192 of them
// - and it is what makes the Remove hook an optimisation rather than a
// correctness requirement.
void SweepHosted() {
	for (HostedPed &h : g_hosted) {
		if (!h.active)
			continue;
		void *const now = h.poolHandle >= 0
		                      ? Func<GetPedFn>(CPools__GetPed)(h.poolHandle)
		                      : nullptr;
		if (now == h.ped)
			continue;
		QueueLost(h, "the sweep");
	}

	ReportCrowd(HostedAmbientPedCount(), g_replicas, HostedAmbientCarCount(),
	            g_carReplicas);
}

// ---- the bridge ------------------------------------------------------------

uint32_t DrainLocalAmbientPeds(LocalAmbientPed *out, uint32_t max) {
	const uint32_t n = g_bornCount < max ? g_bornCount : max;
	for (uint32_t i = 0; i < n; ++i)
		out[i] = g_born[i];
	// Keep whatever did not fit, oldest first. A ped announced late is still
	// the right ped; a ped dropped because a burst overflowed one frame is a
	// pedestrian only one machine can see, forever.
	for (uint32_t i = n; i < g_bornCount; ++i)
		g_born[i - n] = g_born[i];
	g_bornCount -= n;
	if (g_bornCount == 0)
		g_warnedBornFull = false;
	return n;
}

uint32_t DrainLostAmbientPeds(uint16_t *out, uint32_t max) {
	// The sweep runs here rather than on its own frame hook, because this is
	// already called once a frame and the two belong together: what the sweep
	// finds is exactly what this hands over.
	SweepHosted();
	SettleSeatedDrops();

	const uint32_t n = g_lostCount < max ? g_lostCount : max;
	for (uint32_t i = 0; i < n; ++i)
		out[i] = g_lost[i];
	for (uint32_t i = n; i < g_lostCount; ++i)
		g_lost[i - n] = g_lost[i];
	g_lostCount -= n;
	if (g_lostCount == 0)
		g_warnedLostFull = false;
	return n;
}

bool NameLocalAmbientPed(uint32_t tempId, uint16_t netId) {
	HostedPed *h = FindHostedByTempId(tempId);
	if (!h)
		return false;   // reaped while the round trip was in flight

	h->netId = netId;
	h->named = true;

	// And now that it has a name, check it is still there. The sweep only
	// runs once a frame and the record could have been stale for most of
	// this one; without this the caller is told "named" for a ped that no
	// longer exists, and the death is only noticed on the next sweep - by
	// which point every other machine has built a replica of it.
	void *const now = h->poolHandle >= 0
	                      ? Func<GetPedFn>(CPools__GetPed)(h->poolHandle)
	                      : nullptr;
	if (now == h->ped)
		return true;

	*h = HostedPed{};
	return false;
}

// ---- a limb coming off (protocol version 17) --------------------------------
//
// CPed::RemoveBodyPart runs on the machine hosting a pedestrian and nowhere
// else. The shot or blast that calls it is resolved against the real ped, and
// every replica is bullet- and explosion-proof, so an observer's engine never
// reaches the limb. What the observer can do is call the same function on its
// replica with the same two arguments, once somebody tells it to.
//
// The detour is the only reliable witness. InflictDamage decides on a limb
// from a CGeneral::GetRandomNumber roll and several flags, and only the call
// itself says it happened.

using BodyPartFn = void(__fastcall *)(void *, void *, int, int8_t);

// A limb is rare - a burst of five from one rocket at most - so a short queue
// is plenty. It is drained every frame alongside the despawns.
constexpr size_t MAX_QUEUED_LIMBS = 32;
PedBodyPartBody  g_limbs[MAX_QUEUED_LIMBS];
uint32_t         g_limbCount = 0;
bool             g_warnedLimbsFull = false;
bool             g_saidLimbSent    = false;

// Same __fastcall trick as game/combat.cpp's hooks: it is how a free function
// receives `this` in ecx. edx is unused.
void __fastcall BodyPartHook(void *self, void * /*edx*/, int node, int8_t direction) {
	// The engine first, always. Whatever is said about this limb is said
	// about one that has actually come off.
	g_bodyPartDetour.Original<BodyPartFn>()(self, nullptr, node, direction);

	if (!self || node < 0 || !IsRemovableBodyPart(static_cast<uint8_t>(node)))
		return;
	// With CGame::nastyGame off the function returns before touching the
	// ped, and there is nothing to show anybody. The build that ships with
	// it off would say nothing either.
	if (Global<uint8_t>(CGame__nastyGame) == 0)
		return;

	// Only a pedestrian this machine hosts, and only once the session has a
	// name for it. A replica passes neither test - which is what keeps an
	// observer applying a limb from bouncing it straight back - and a ped
	// that loses a limb inside the round trip of its own naming keeps it on
	// other screens, which is a rare and harmless miss.
	//
	// Or our own player. Their machine is the only one that shoots them for
	// real, so it is the only one that sees a limb go; it goes out under
	// INVALID_NETID and Client puts our netId on it.
	const HostedPed *h    = FindHostedByPed(self);
	const bool       ours = self == PlayerPed();
	if (!ours && (!h || !h->named || h->netId == INVALID_NETID))
		return;

	if (g_limbCount >= MAX_QUEUED_LIMBS) {
		if (!g_warnedLimbsFull) {
			g_warnedLimbsFull = true;
			Log("population: the limb queue is full; some limbs will stay on "
			    "for everybody else (and this will not be said again)");
		}
		return;
	}
	PedBodyPartBody &out = g_limbs[g_limbCount++];
	out.netId     = ours ? INVALID_NETID : h->netId;
	out.node      = static_cast<uint8_t>(node);
	out.direction = direction;
}

uint32_t DrainAmbientBodyParts(PedBodyPartBody *out, uint32_t max) {
	const uint32_t n = g_limbCount < max ? g_limbCount : max;
	for (uint32_t i = 0; i < n; ++i)
		out[i] = g_limbs[i];
	for (uint32_t i = n; i < g_limbCount; ++i)
		g_limbs[i - n] = g_limbs[i];
	g_limbCount -= n;
	if (g_limbCount == 0)
		g_warnedLimbsFull = false;

	if (n != 0 && !g_saidLimbSent) {
		g_saidLimbSent = true;
		Log("population: one of our pedestrians lost a limb (node %u, ped %u); "
		    "telling the session so it comes off on every screen (and this will "
		    "not be said again)", out[0].node, out[0].netId);
	}
	return n;
}

// ---- a pedestrian dying ----------------------------------------------------
//
// The same shape as the limb above and for the same reason: the only witness
// is the engine's own call, and only the machine hosting the ped ever makes
// it. Every replica is bullet-, fire-, melee- and collision-proof, so an
// observer's engine can never reach the death by itself.
//
// The detour is game/combat.cpp's - CPed::SetDie is one address and it can
// carry one hook, and combat.cpp has held it since M4 because it is where a
// *player's* death animation is captured. This is the ambient half of the
// same call, and it lives here because "which peds am I entitled to talk
// about" is a question only this file can answer.
//
// A death is rarer than a limb and the burst is smaller: a rocket into a
// crowd kills a handful at once where it takes five limbs off one body. Eight
// would do; sixteen is the same order as the limb queue so the two overflow
// alike.
constexpr size_t MAX_QUEUED_DEATHS = 16;
PedDeathBody     g_deaths[MAX_QUEUED_DEATHS];
uint32_t         g_deathCount = 0;
bool             g_warnedDeathsFull = false;
bool             g_saidDeathSent    = false;

} // namespace

// Called from game/combat.cpp's CPed::SetDie detour, once per ped the local
// engine has actually just killed - it has already made the transition test,
// because SetDie returns without doing anything for a ped that is already
// dying and "it was called" is not "it died".
//
// The filter is BodyPartHook's, verbatim, and for the same three reasons. A
// replica fails it, which is what stops an observer bouncing a death it was
// told about straight back at the session. A ped this machine never
// announced fails it, because nobody else has one to kill. And a ped killed
// inside the round trip of its own naming fails it too: it has no netId yet,
// so there is nothing to put in the packet. That last one is a real miss and
// it is the same rare, harmless one the limbs already accept - the ped is
// about to be reaped by its own engine and despawned everywhere anyway.
void NoteHostedPedDeath(void *ped, uint16_t animId) {
	if (!ped)
		return;
	const HostedPed *h = FindHostedByPed(ped);
	if (!h || !h->named || h->netId == INVALID_NETID)
		return;

	// Once per ped, not once per SetDie. The engine can reach SetDie twice
	// for one pedestrian - a corpse shot again, a body caught in a second
	// blast - and the transition test in the detour catches most of that,
	// but a duplicate here costs an observer a second SetStoredState over a
	// state that is already the death's.
	for (uint32_t i = 0; i < g_deathCount; ++i)
		if (g_deaths[i].netId == h->netId)
			return;

	if (g_deathCount >= MAX_QUEUED_DEATHS) {
		if (!g_warnedDeathsFull) {
			g_warnedDeathsFull = true;
			Log("population: the death queue is full; some pedestrians will stay "
			    "on their feet for everybody else (and this will not be said "
			    "again)");
		}
		return;
	}
	PedDeathBody &out = g_deaths[g_deathCount++];
	out.netId  = h->netId;
	out.animId = animId;
}

HostDeathScope::HostDeathScope() { g_applyingHostDeath = true; }
HostDeathScope::~HostDeathScope() { g_applyingHostDeath = false; }

// Called from game/combat.cpp's CPed::SetDie detour, before the original, for
// every ped that is not the local player.
//
// **Why this is a refusal and not a report**, which is the decision the whole
// change turns on. CoopIII is host-authoritative: an entity's own machine
// decides what happens to it and everybody else is written to. A replica
// reaching CPed::SetDie on an observer is that rule being broken, not a fact
// the observer has discovered - the host's copy of that pedestrian is alive
// and walking, and it is the observer that is wrong. Telling the session
// would be one machine announcing its own mistake as news, and the session
// would then have to pick a winner between two machines for a pedestrian
// neither of them is authoritative about together. So nothing goes on the
// wire; the call simply does not happen.
//
// **Why it is stated on the object and not on the cause.** `SpawnAmbientReplica`
// already sets the four CEntity proof flags plus bExplosionProof, and they
// are not a mechanism: population.h's PedProofForDamageCause has the jump
// table out of the retail image, and six of CPed::InflictDamage's causes read
// no flag at all. A guard that has to enumerate causes is a guard that will
// be wrong again the next time somebody finds a seventh.
//
// **Why here and not at CPed::InflictDamage**, which already refuses damage
// to a replica since protocol 23. Because most of what can still kill one
// does not go through InflictDamage: `CPed::ProcessControl`'s
// `m_fHealth <= 1.0f` arm at 0x004C8DDA, `CPed::SetGetUp`'s crush at
// 0x004D0F95 and `CAutomobile::BlowUpCar`'s on-foot arm at 0x0053BDFE all
// call SetDie directly. Guarding the damage was the right thing to do and it
// was never going to be enough.
bool RefuseLocalReplicaDeath(void *ped, uint16_t &netId) {
	if (!ped)
		return false;
	// The session's own kill, through KillAmbientReplica. It goes through the
	// engine's address and therefore through the same detour.
	if (g_applyingHostDeath)
		return false;
	if (!AmbientReplicaForPed(ped, netId))
		return false;
	if (PlanReplicaSetDie(/*isReplica=*/true, /*sessionAsked=*/false) !=
	    SetDieVerdict::RefuseAndHeal)
		return false;

	// The health goes back before the refusal is reported, because two of the
	// callers zeroed it on the way in and a replica left on zero health is one
	// CPed::ProcessControl will bring straight back here on the next frame,
	// sixty times a second, for the rest of the session. 100.0f is what
	// CPed::CPed gives a new one (0x004C4225).
	Field<float>(ped, offs::PED_HEALTH) = REPLICA_FULL_HEALTH;

	++g_replicaDeathRefused;
	if (PopTrace())
		Log("population/trace: refused a local death of replica net %u", netId);

	static bool said = false;
	if (!said) {
		said = true;
		Log("population: our own engine tried to kill ambient ped replica %u "
		    "and was refused - only the machine hosting a pedestrian decides "
		    "that he is dead, and it tells us on C_PedDeath. His health has "
		    "been put back (and this will not be said again)", netId);
	}
	return true;
}

bool AmbientReplicaForPed(const void *ped, uint16_t &netId) {
	if (!ped)
		return false;
	for (const ReplicaIdentity &id : g_replicaIndex) {
		if (id.ped != ped)
			continue;
		// Matched on the pointer; now the half that makes it safe. A pool slot
		// is reused the instant it frees up, so a row whose replica the engine
		// has already deleted would otherwise go on matching whatever moved in
		// - and the next civilian to take that slot would have its wounds
		// reported to the session under somebody else's pedestrian's name.
		// AmbientReplicaIsAlive clears the entry on the next frame; until it
		// runs, this is what says no.
		if (id.poolHandle < 0 ||
		    Func<GetPedFn>(CPools__GetPed)(id.poolHandle) != ped)
			return false;
		netId = id.netId;
		return true;
	}
	return false;
}

bool AmbientReplicaHostPedType(const void *ped, uint16_t &netId, uint8_t &pedType) {
	if (!AmbientReplicaForPed(ped, netId))
		return false;
	for (const ReplicaIdentity &id : g_replicaIndex)
		if (id.ped == ped && id.netId == netId) {
			pedType = id.hostPedType;
			return true;
		}
	return false;
}

size_t TakeDownReplicasOfModel(uint16_t modelId) {
	size_t taken = 0;
	for (const ReplicaIdentity &id : g_replicaIndex) {
		if (!id.ped || id.poolHandle < 0)
			continue;
		void *const mem = Func<GetPedFn>(CPools__GetPed)(id.poolHandle);
		if (mem != id.ped || Field<uintptr_t>(mem, offs::VTABLE) != CCivilianPed__vtable ||
		    Field<uint16_t>(mem, offs::MODEL_INDEX) != modelId)
			continue;
		// Out of any car first, as MakeRoomForSpecialModel does for a player:
		// the seat's reference into this ped is the car's to give back.
		if (Field<bool>(mem, offs::PED_IN_VEHICLE))
			UnseatReplicaPed(mem);
		bool gone = false;
		{
			ReplicaScope scope;
			// Not counted here: AmbientReplicaIsAlive finds the handle gone on
			// its next pass and gives back the mission-ped count, the replica
			// count and the index entry, as for any replica the engine took.
			gone = DestroyPed(mem, /*countedMissionPed=*/false,
			                  "a pedestrian replica built from a model being renamed");
		}
		if (!gone)
			continue;
		++taken;
		if (!g_saidRenameTakedown) {
			g_saidRenameTakedown = true;
			Log("population: model %u is being renamed while replica %u is built from it; "
			    "took the replica down first, the host's stream builds it again (said once)",
			    static_cast<unsigned>(modelId), id.netId);
		}
	}
	return taken;
}

int32_t HostedCarHandle(uint16_t netId) {
	if (netId == INVALID_NETID)
		return -1;
	for (const HostedCar &c : g_hostedCars)
		if (c.active && c.named && c.netId == netId && c.poolHandle >= 0 &&
		    AmbientCarFromRef(c.poolHandle) == c.vehicle)
			return c.poolHandle;
	return -1;
}

uint16_t HostedCarNetId(int32_t handle) {
	if (handle < 0)
		return INVALID_NETID;
	for (const HostedCar &c : g_hostedCars)
		if (c.active && c.named && c.poolHandle == handle &&
		    AmbientCarFromRef(c.poolHandle) == c.vehicle)
			return c.netId;
	return INVALID_NETID;
}

void *ResolveHostedCar(uint16_t netId) {
	if (netId == INVALID_NETID)
		return nullptr;
	for (const HostedCar &c : g_hostedCars) {
		if (!c.active || !c.named || c.netId != netId)
			continue;
		void *const now = c.poolHandle >= 0 ? AmbientCarFromRef(c.poolHandle) : nullptr;
		return now == c.vehicle ? now : nullptr;
	}
	return nullptr;
}

bool HostedMissionEntity(const void *entity) {
	if (!entity)
		return false;
	if (const HostedPed *h = FindHostedByPed(entity))
		return h->mission;
	if (const HostedCar *c = FindHostedCar(entity))
		return c->mission;
	return false;
}

size_t HostedMissionPeds(void **out, size_t max) {
	size_t n = 0;
	for (const HostedPed &h : g_hosted)
		if (n < max && h.active && h.mission && h.poolHandle >= 0 &&
		    Func<GetPedFn>(CPools__GetPed)(h.poolHandle) == h.ped)
			out[n++] = h.ped;
	return n;
}

MissionHitScope::MissionHitScope(void *entity) { Lift(entity, false); }

MissionHitScope::MissionHitScope(void *entity, const void *culprit, uint32_t cause) {
	if (culprit && cause <= 0xFF &&
	    (ParticipantBlastCounts(static_cast<uint8_t>(cause)) ||
	     ParticipantCollisionCounts(static_cast<uint8_t>(cause))) &&
	    MissionParticipantEntity(culprit))
		Lift(entity, true);
}

void MissionHitScope::Lift(void *entity, bool blast) {
	if (!entity || !OwnMissionRunning() || !HostedMissionEntity(entity))
		return;
	uint8_t &flags = Field<uint8_t>(entity, offs::ENTITY_FLAGS_C);
	if ((flags & offs::ENTITY_ONLY_DAMAGED_BY_PLAYER) == 0)
		return;
	flags    = static_cast<uint8_t>(flags & ~offs::ENTITY_ONLY_DAMAGED_BY_PLAYER);
	m_entity = entity;
	static bool saidHit = false, saidBlast = false;
	bool &said = blast ? saidBlast : saidHit;
	if (!said) {
		said = true;
		Log(blast ? "population: a participant's blast, fire or car reached one of the mission's own "
		            "only-the-player targets here, and counts as the player's (mission-audit.md R1)"
		          : "population: a participant's hit reached one of the mission's own "
		            "only-the-player targets, and counts as the player's (mission-audit.md R1)");
	}
}

MissionHitScope::~MissionHitScope() {
	if (m_entity)
		Field<uint8_t>(m_entity, offs::ENTITY_FLAGS_C) |= offs::ENTITY_ONLY_DAMAGED_BY_PLAYER;
}

bool HostedPedFor(const void *ped, bool &named) {
	named = false;
	const HostedPed *h = ped ? FindHostedByPed(ped) : nullptr;
	if (!h || h->poolHandle < 0 ||
	    Func<GetPedFn>(CPools__GetPed)(h->poolHandle) != ped)
		return false;
	named = h->named && h->netId != INVALID_NETID;
	return true;
}

bool HostedCarFor(const void *vehicle, bool &named) {
	named = false;
	const HostedCar *c = vehicle ? FindHostedCar(vehicle) : nullptr;
	if (!c || c->poolHandle < 0 || AmbientCarFromRef(c->poolHandle) != vehicle)
		return false;
	named = c->named && c->netId != INVALID_NETID;
	return true;
}

bool HostMissionCar(void *vehicle, bool &hostedNow, const char *&why) {
	hostedNow = false;
	why       = "";
	if (!vehicle || EntityType(vehicle) != ENTITY_TYPE_VEHICLE) {
		why = "it is not a car";
		return false;
	}
	void *const player = PlayerPed();
	if (player && Field<void *>(vehicle, offs::VEH_DRIVER) == player) {
		why = "the local player is at its wheel, and the claim path has it";
		return false;
	}
	if (IsWreckedCar(vehicle)) {
		why = "it is a wreck";
		return false;
	}
	const Hosting h = HostCar(vehicle, true);
	if (h != Hosting::Hosted && h != Hosting::Already) {
		why = HostingWhy(h);
		return false;
	}
	hostedNow = h == Hosting::Hosted;
	NoteMissionEntity(vehicle, AmbientCarRef(vehicle), true,
	                  static_cast<uint16_t>(Field<uint32_t>(vehicle, offs::MODEL_INDEX) & 0xFFFF));
	return true;
}

void KeepMissionEntitiesHosted(uint32_t nowMs) {
	if (!OwnMissionRunning()) {
		g_missionEntityCount      = 0;
		g_saidMissionEntitiesFull = false;
		return;
	}
	void *const player = PlayerPed();
	for (size_t i = 0; i < g_missionEntityCount;) {
		MissionEntity &m = g_missionEntities[i];
		if (ResolveRef(m.ref, m.car) != m.entity) {
			ForgetMissionEntityAt(i);   // gone from its pool: the sweep said so
			continue;
		}
		bool hosted = false;
		if (m.car) {
			bool named = false;
			hosted     = HostedCarFor(m.entity, named);
		} else {
			bool named = false;
			hosted     = HostedPedFor(m.entity, named);
		}
		// Somebody at the wheel is the claim path's; a wreck or a corpse
		// would come back to the others whole.
		const bool may = m.car ? !(player && Field<void *>(m.entity, offs::VEH_DRIVER) == player) &&
		                             !IsWreckedCar(m.entity)
		                       : m.entity != player && Field<float>(m.entity, offs::PED_HEALTH) > 0.0f;
		if (hosted || !may) {
			m.missingSinceMs = 0;
			++i;
			continue;
		}
		if (m.missingSinceMs == 0) {
			m.missingSinceMs = nowMs != 0 ? nowMs : 1;
			++i;
			continue;
		}
		const uint32_t missing = nowMs - m.missingSinceMs;
		if (missing < MISSION_ENTITY_GRACE_MS) {
			++i;
			continue;
		}
		const Hosting h = m.car ? HostCar(m.entity, true) : HostPed(m.entity, true);
		++m.hostedAgain;
		Log("population: the mission's %s (model %u) was in the pool and hosted by nobody for %u "
		    "ms; %s (%u time%s)",
		    m.car ? "car" : "pedestrian", static_cast<unsigned>(m.modelId),
		    static_cast<unsigned>(missing),
		    h == Hosting::Hosted ? "announced to the session again" : HostingWhy(h),
		    static_cast<unsigned>(m.hostedAgain), m.hostedAgain == 1 ? "" : "s");
		m.missingSinceMs = 0;
		++i;
	}
}

size_t MissionEntitiesKept(size_t *cars) {
	size_t n = 0;
	for (size_t i = 0; i < g_missionEntityCount; ++i)
		if (g_missionEntities[i].car)
			++n;
	if (cars)
		*cars = n;
	return g_missionEntityCount;
}

void *ResolveHostedPed(uint16_t netId) {
	if (netId == INVALID_NETID)
		return nullptr;
	for (const HostedPed &h : g_hosted) {
		// `named` is the entitlement half. A ped this machine has not yet had a
		// netId for cannot be the one anybody asked about, and a row that is
		// merely active is not one the session has a name for.
		if (!h.active || !h.named || h.netId != netId)
			continue;
		// And the liveness half, exactly as the sweep does it: the handle has to
		// still resolve, and it has to still resolve to the same object. The
		// sweep runs once a frame and this row could have been stale for most of
		// this one, which is the same race NameLocalAmbientPed re-checks for.
		void *const now = h.poolHandle >= 0
		                      ? Func<GetPedFn>(CPools__GetPed)(h.poolHandle)
		                      : nullptr;
		return now == h.ped ? now : nullptr;
	}
	return nullptr;
}

bool HostedPedNetIdFor(const void *ped, uint16_t &netId) {
	bool named = false;
	if (!HostedPedFor(ped, named) || !named)
		return false;
	netId = FindHostedByPed(ped)->netId;
	return true;
}

bool HostedCarNetIdFor(const void *vehicle, uint16_t &netId) {
	bool named = false;
	if (!HostedCarFor(vehicle, named) || !named)
		return false;
	netId = FindHostedCar(vehicle)->netId;
	return true;
}

void *AmbientReplicaPed(const RemoteAmbientPed &ped) {
	if (ped.poolHandle < 0)
		return nullptr;
	void *const mem = Func<GetPedFn>(CPools__GetPed)(ped.poolHandle);
	if (!mem || Field<uintptr_t>(mem, offs::VTABLE) != CCivilianPed__vtable)
		return nullptr;
	return mem;
}

uint8_t AmbientBeingPulledOut(const RemoteAmbientPed &ped) {
	return PullOutOf(AmbientReplicaPed(ped));
}

// Where our engine has a replica, for a desync probe (protocol.h,
// C_DesyncProbe). Read only; a ped the engine is seating is left to its car.
bool SampleReplicaPosition(int32_t poolHandle, bool car, Vec3 &out) {
	if (poolHandle < 0)
		return false;
	void *mem = nullptr;
	if (car) {
		mem = Func<void *(__cdecl *)(int32_t)>(CPools__GetVehicle)(poolHandle);
	} else {
		mem = Func<GetPedFn>(CPools__GetPed)(poolHandle);
		if (mem && (Field<uintptr_t>(mem, offs::VTABLE) != CCivilianPed__vtable ||
		            Field<bool>(mem, offs::PED_IN_VEHICLE)))
			return false;
	}
	if (!mem)
		return false;
	out = Vec3{Field<float>(mem, offs::POSITION + 0), Field<float>(mem, offs::POSITION + 4),
	           Field<float>(mem, offs::POSITION + 8)};
	return true;
}

// The weapon in the host's ped's hand, in the replica's. A replica holding a
// gun still decides nothing with it: it has no objective and no threat
// response (docs/protocol.md §1.13.6), and combat.cpp refuses any round it
// fires outside a replay of its host's.
bool ArmAmbientReplica(RemoteAmbientPed &ped, uint8_t weapon) {
	void *const mem = AmbientReplicaPed(ped);
	if (!mem || Field<bool>(mem, offs::PED_IN_VEHICLE))
		return false;
	const uint32_t state = Field<uint32_t>(mem, offs::PED_STATE);
	if (state == PEDSTATE_DIE || state == PEDSTATE_DEAD)
		return false;
	if (!PutReplicaWeaponInHand(mem, weapon))
		return false;

	static bool said = false;
	if (!said && weapon != WEAPONTYPE_UNARMED) {
		said = true;
		Log("population: armed our first replica - pedestrian net %u holds weapon %u, "
		    "as his host has him", static_cast<unsigned>(ped.netId), weapon);
	}
	return true;
}

namespace {

// Defined at the bottom of this file, next to the spawn it undoes. Needed up
// here because AmbientReplicaIsAlive has to be able to take away a replica the
// local engine killed, and a corpse it only dropped the handle to would be a
// locked MISSION_CHAR body lying in the street for the rest of the session -
// CanBeDeleted refuses it, so nothing would ever come for it.
void DespawnAmbientReplica(RemoteAmbientPed &ped);

uint32_t DrainAmbientPedDeaths(PedDeathBody *out, uint32_t max) {
	const uint32_t n = g_deathCount < max ? g_deathCount : max;
	for (uint32_t i = 0; i < n; ++i)
		out[i] = g_deaths[i];
	for (uint32_t i = n; i < g_deathCount; ++i)
		g_deaths[i - n] = g_deaths[i];
	g_deathCount -= n;
	if (g_deathCount == 0)
		g_warnedDeathsFull = false;

	if (n != 0 && !g_saidDeathSent) {
		g_saidDeathSent = true;
		Log("population: one of our pedestrians died (ped %u, anim %u); telling "
		    "the session so he drops on every screen (and this will not be said "
		    "again)", out[0].netId, out[0].animId);
	}
	return n;
}

// The observer's half: kill a replica because the machine hosting it did.
//
// The engine decides the death, not CoopIII. CPed::SetDie is what picks the
// stored state, clears the ped's tasks, zeroes the health, blends the fall
// and leaves the body where CPed::ProcessControl can play it out - the same
// argument BlowUpRemoteVehicle makes for going through the engine's own
// BlowUpCar rather than building a wreck by hand.
//
// **The seat comes off first and that is not tidiness.** CPed::SetDie's
// PED_DRIVING arm calls CPed::IsPlayer and then, for anything that is not the
// player, vtable slot 0x40 - FlagToDestroyWhenNextProcessed (addresses.h, at
// the CPed::SetDie declaration). Every replica is a CCivilianPed, so killing
// a seated one hands it to the engine to delete on the next frame and the
// pool handle stops resolving. Client::OnDeath has done the same thing for a
// seated *player* since M4; this is the ambient copy of that rule, and the
// caller clears the standing seat request so the reconciliation loop does not
// put the corpse straight back behind the wheel.
bool KillAmbientReplica(RemoteAmbientPed &ped, uint16_t animId) {
	if (ped.poolHandle < 0)
		return false;
	void *const mem = Func<GetPedFn>(CPools__GetPed)(ped.poolHandle);
	// A pool slot is reused immediately; the vtable is what says it is still
	// the replica we built and not whatever took the slot.
	if (!mem || Field<uintptr_t>(mem, offs::VTABLE) != CCivilianPed__vtable)
		return false;

	// Out of the car before the engine is asked to kill him, whatever this
	// machine thinks it has done with him. Read off the ped rather than off
	// RemoteAmbientPed::Seated(), because `m_nPedState == PED_DRIVING` is
	// literally the dword SetDie switches on (0x004D3848, measured), and the
	// two can disagree for a frame after a seating the engine refused.
	//
	// PED_DRIVING and not bInVehicle: the other arm of that branch only
	// cancels the ped's vehicle animation, so unseating on bInVehicle would
	// take peds out of cars that were never in danger.
	const uint32_t state = Field<uint32_t>(mem, offs::PED_STATE);
	if (state == PEDSTATE_DRIVING)
		UnseatReplicaPed(mem);

	// Already a corpse. SetDie returns early for this itself, so this is not
	// a correctness guard - it is so a duplicate is reported as done rather
	// than retried forever by the caller.
	if (state == PEDSTATE_DIE || state == PEDSTATE_DEAD)
		return true;

	// The id goes to CAnimManager::BlendAnimation against ASSOCGRP_STD with
	// nothing in between, so it gets bounded against that group's real size
	// first - the same PlanDeathAnim a player's death goes through, and the
	// same reason: the id becomes a subscript and nothing in the engine
	// checks it.
	using SetDieThisFn = void(__thiscall *)(void *, uint32_t, float, float);
	//
	// Still falling from our car, he dies into that fall and is not stood up
	// for his host's (game/runover.h, RunOverDeathAnim).
	const uint16_t anim = PlanDeathAnim(
	    RunOverDeathAnim(RunOverHoldsReplica(ped.netId, state), state, animId),
	    StdAnimGroupCount());
	{
		// Through the engine's own address, so through game/combat.cpp's own
		// CPed::SetDie detour - which now refuses that call for a replica.
		// This is the one caller entitled to make it, so it says so.
		HostDeathScope scope;
		Func<SetDieThisFn>(CPed__SetDie)(mem, anim, PED_DIE_DELTA, PED_DIE_SPEED);
	}

	// Whatever was driven into this ped went with the death. ClearAll and the
	// die animation replaced it, so leaving this set would have
	// ApplyAmbientPedState skip a re-blend as "already applied" - which it
	// will not reach anyway, since it refuses a dead replica, but the two
	// disagreeing is how the next person reading this gets it wrong.
	ped.appliedAnimId = ANIM_NONE;
	ped.appliedWeapon = 0xFF;

	static bool said = false;
	if (!said) {
		said = true;
		Log("population: killed a replica because its host's engine did - ped %u, "
		    "anim %u (and this will not be said again)", ped.netId, anim);
	}
	return true;
}

// The same for a remote player's ped: their own machine took the limb off
// them. Also through the detour above, which leaves it alone - the ped is
// neither hosted here nor our player.
bool RemovePlayerBodyPart(RemotePlayer &player, uint8_t node, int8_t direction) {
	if (player.poolHandle < 0 || !IsRemovableBodyPart(node))
		return false;
	void *const mem = Func<GetPedFn>(CPools__GetPed)(player.poolHandle);
	if (!mem || Field<uintptr_t>(mem, offs::VTABLE) != CCivilianPed__vtable)
		return false;

	Func<BodyPartFn>(CPed__RemoveBodyPart)(mem, nullptr, node, direction);

	static bool said = false;
	if (!said) {
		said = true;
		Log("population: took a limb off player %u's ped because their own engine did "
		    "- node %u (and this will not be said again)", player.playerId, node);
	}
	return true;
}

// The observer's half. Called through the engine's own address, so it goes
// through the detour above too - which does nothing with it, because a
// replica is not hosted here.
bool RemoveAmbientBodyPart(RemoteAmbientPed &ped, uint8_t node, int8_t direction) {
	if (ped.poolHandle < 0 || !IsRemovableBodyPart(node))
		return false;
	void *const mem = Func<GetPedFn>(CPools__GetPed)(ped.poolHandle);
	// A pool slot is reused immediately; the vtable is what says it is still
	// the replica we built and not whatever took the slot.
	if (!mem || Field<uintptr_t>(mem, offs::VTABLE) != CCivilianPed__vtable)
		return false;

	Func<BodyPartFn>(CPed__RemoveBodyPart)(mem, nullptr, node, direction);

	static bool said = false;
	if (!said) {
		said = true;
		Log("population: took a limb off a replica because its host's engine "
		    "did - node %u of ped %u (and this will not be said again)", node,
		    ped.netId);
	}
	return true;
}

// ---- the ped stream (docs/population.md §3 step 6) --------------------------
//
// Step 2's replica was created where the session said and left there, on
// purpose: "does it appear on the other screen and stay where it is put" was
// the whole of that step. It appears and it stays, and what that looks like
// in a running game is a city of statues - the host's pedestrians walk around
// its own Liberty City, and on every other machine they stand exactly where
// they were born until the host's engine reaps them.
//
// This is the traffic stream, applied to pedestrians, with one row a third
// smaller (protocol.h, AmbientPedState) and one field a car has no use for.

// Which seat of `car` holds `ped`: 0 for the driver, 1 + n for passenger n,
// or -1. Bounded by the car's own m_nNumMaxPassengers and then again by the
// array's real length - that field is a byte out of the handling data and
// the array is eight pointers whatever it says.
int SeatOfPedInCar(void *car, void *ped) {
	if (Field<void *>(car, offs::VEH_DRIVER) == ped)
		return 0;
	void *const   *seats = &Field<void *>(car, offs::VEH_PASSENGERS);
	const uint8_t  max   = Field<uint8_t>(car, offs::VEH_NUM_MAX_PASSENGERS);
	const uint8_t  n = max < offs::VEH_MAX_PASSENGERS ? max : offs::VEH_MAX_PASSENGERS;
	for (uint8_t i = 0; i < n; ++i)
		if (seats[i] == ped)
			return 1 + i;
	return -1;
}

// Is this hosted ped sitting in a car the session can name, and if so which
// one and which seat?
//
// A car we host, by the netId the session gave it, or a session car, by
// its. The second used to be left out, on the grounds that a car the claim
// path owns was not this seam's to speak for. But the claim path only ever
// says where *players* sit, so a pedestrian in a session car was said by
// nobody, and went out as a man on foot standing where his seat is. That is
// every mission passenger in a car a player drives: Misty in the owner's car,
// 8-Ball in the Kuruma once a participant took its wheel. Every other screen
// then stood him up through the roof, and on the machine of whoever was
// driving he was in the car's way sixty times a second and it could barely
// move. A car that is neither cannot be named to anybody, because only its
// host knows its netId.
// The session's name for a car: a car we host, by the netId the session gave
// it, or a session car, by its. INVALID_NETID for any other.
uint16_t SessionNameOfCar(void *car, const SessionCarRef *cars, uint32_t carCount) {
	if (!car)
		return INVALID_NETID;
	if (const HostedCar *held = FindHostedCar(car))
		return held->named ? held->netId : INVALID_NETID;
	if (carCount != 0) {
		const int32_t handle = AmbientCarRef(car);
		for (uint32_t i = 0; i < carCount; ++i)
			if (cars[i].poolHandle >= 0 && cars[i].poolHandle == handle)
				return cars[i].netId;
	}
	return INVALID_NETID;
}

bool HostedSeatOf(void *ped, const SessionCarRef *cars, uint32_t carCount,
                  uint16_t &vehicleNetId, uint8_t &seat) {
	vehicleNetId = INVALID_NETID;
	seat         = 0;
	if (!Field<bool>(ped, offs::PED_IN_VEHICLE))
		return false;
	void *const car = Field<void *>(ped, offs::PED_MY_VEHICLE);
	if (!car)
		return false;   // the car went away and left bInVehicle standing

	const uint16_t named = SessionNameOfCar(car, cars, carCount);
	if (named == INVALID_NETID)
		return false;

	const int slot = SeatOfPedInCar(car, ped);
	if (slot < 0)
		return false;   // bInVehicle set and no seat: the engine's own race

	vehicleNetId = named;
	seat         = static_cast<uint8_t>(slot);
	return true;
}

// A hosted pedestrian in a car that HostedSeatOf could not name. The first of
// each kind says why, since the why is the bug: a car of ours the session has not named
// yet is a moment's wait, one this machine does not host at all is a car only
// this screen has.
bool InUnseenCar(void *ped) {
	if (!Field<bool>(ped, offs::PED_IN_VEHICLE))
		return false;
	void *const car = Field<void *>(ped, offs::PED_MY_VEHICLE);
	if (!car)
		return false;
	const HostedCar *held = FindHostedCar(car);
	bool &said = held ? g_saidPedInUnnamedCar : g_saidPedInUnseenCar;
	if (!said) {
		said = true;
		uint16_t model = 0;
		VehicleModelIndex(car, model);
		Log("population: our pedestrian %p sits in car %p (model %u, created by %u) that the "
		    "session has no name for - %s; other screens keep him out of sight (said once)",
		    ped, car, static_cast<unsigned>(model),
		    static_cast<unsigned>(Field<uint8_t>(car, offs::VEH_CREATED_BY)),
		    held ? "we host it and it is not named yet, or has no seat for him"
		         : "this machine does not host it");
	}
	return true;
}

// What this machine is saying about its own pedestrians this tick.
//
// The car batch's rule (game/streampick.h), with two kinds of ped weighted
// down to the floor whatever their distance, because an observer does nothing
// with where they are:
//
//   - a seated one. His row is the traffic-driver fix - the host is the only
//     one who knows which ped belongs in which car - but his pos/heading are
//     ignored on the far side, where CWorld::Process positions him from his
//     car's matrix (docs/protocol.md §1.13.2). The seat is held between rows,
//     and a change of seat goes out on the next tick anyway.
//   - a corpse. ApplyAmbientPedState refuses to drive a dead replica, and his
//     death travelled on its own (S_PedDeath).
//
// This replaces seated-first-up-to-half-then-nearest-our-player, which sent
// the same twelve every tick and never the thirteenth.
uint32_t SampleHostedPeds(AmbientPedState *out, uint32_t max, const Vec3 *viewers,
                          uint32_t viewerCount, const SessionCarRef *cars, uint32_t carCount) {
	if (max == 0)
		return 0;
	const uint32_t want = max < MAX_PED_STATES ? max : MAX_PED_STATES;

	struct Row {
		void      *ped;
		HostedPed *host;
		uint16_t   vehicleNetId;
		uint8_t    seat;
		uint8_t    flags;
	};
	Row             rows[MAX_HOSTED];
	StreamCandidate cand[MAX_HOSTED];
	uint32_t        n      = 0;
	uint32_t        unseen = 0;

	for (HostedPed &h : g_hosted) {
		if (!h.active || !h.named || h.netId == INVALID_NETID)
			continue;
		void *const ped = h.poolHandle >= 0
		                      ? Func<GetPedFn>(CPools__GetPed)(h.poolHandle)
		                      : nullptr;
		if (ped != h.ped)
			continue;   // the sweep will deal with it; nothing to say

		Row &r = rows[n];
		r.ped  = ped;
		r.host = &h;
		const bool inCar = HostedSeatOf(ped, cars, carCount, r.vehicleNetId, r.seat);
		// Alight, as the host's own engine has it. m_pFire is set by
		// StartFire and nilled by Extinguish, so there is no second copy of
		// the answer to go stale (the same read PF_ON_FIRE makes). And what
		// is in his hand, so the replica is holding the gun his rounds come
		// out of (C_NpcShot).
		r.flags = Field<void *>(ped, PED_FIRE) ? AMBIENT_PED_ON_FIRE : 0;
		r.flags = AmbientPedFlagsWithWeapon(r.flags, HeldWeaponType(ped));
		// Sitting in a car nobody else can be told about. His position is that
		// car's seat, so on foot on another screen he drives nothing in mid-air;
		// the bit has them keep him out of sight instead.
		if (!inCar && InUnseenCar(ped)) {
			r.flags |= AMBIENT_PED_IN_UNSEEN_CAR;
			++unseen;
		}

		const uint32_t state  = Field<uint32_t>(ped, offs::PED_STATE);
		const bool     corpse = state == PEDSTATE_DIE || state == PEDSTATE_DEAD;

		// At a car's door, either way, so every copy opens the same door
		// instead of appearing in the seat or on the pavement. Not a jack:
		// that needs somebody pulled out, and a copy never takes a seat from
		// anybody (seatplan.h).
		if (inCar) {
			if (state == PEDSTATE_EXIT_CAR)
				r.flags = static_cast<uint8_t>(r.flags | AMBIENT_PED_EXITING);
		} else {
			LocalCarEntry entry;
			if (SamplePedCarEntry(ped, entry) && !entry.jack) {
				const uint16_t named = SessionNameOfCar(
				    Field<void *>(ped, offs::PED_MY_VEHICLE), cars, carCount);
				if (named != INVALID_NETID) {
					r.vehicleNetId = named;
					r.seat         = AmbientPedEntrySeatByte(entry.seat, entry.door);
					r.flags        = static_cast<uint8_t>(r.flags | AMBIENT_PED_ENTERING);
					if (!g_saidHostedAtDoor) {
						g_saidHostedAtDoor = true;
						Log("population: pedestrian %u is getting into car %u by the door "
						    "of seat %u; the others open it with him",
						    h.netId, named, entry.door);
					}
				}
			}
		}

		StreamCandidate &k = cand[n];
		k          = StreamCandidate{};
		k.row      = &h.stream;
		k.index    = n;
		k.says     = PedRowSays(r.vehicleNetId, r.seat, r.flags);
		k.deadline = (r.flags & AMBIENT_PED_ON_FIRE) ? STREAM_FIRE_DEADLINE
		             : (r.flags & (AMBIENT_PED_ENTERING | AMBIENT_PED_EXITING))
		                 ? STREAM_DOOR_DEADLINE
		                 : uint8_t{0};
		const bool seen =
		    NearestViewerDist2(ReadVec3(ped, offs::POSITION), viewers, viewerCount, k.dist2);
		const bool hiddenElsewhere = (r.flags & AMBIENT_PED_IN_UNSEEN_CAR) != 0;
		k.weight = (inCar || corpse || hiddenElsewhere) ? uint16_t{1} : StreamWeight(seen, k.dist2);
		++n;
	}

	g_pedsInUnseenCars = unseen;

	const uint32_t written = PickStreamRows(cand, n, want);
	for (uint32_t i = 0; i < written; ++i) {
		const Row &r = rows[cand[i].index];
		AmbientPedState &s = out[i];
		s.netId        = r.host->netId;
		s.animId       = ReadPedBaseAnim(r.ped);
		s.vehicleNetId = r.vehicleNetId;
		s.seat         = r.seat;
		s.flags        = r.flags;
		s.pos          = ReadVec3(r.ped, offs::POSITION);
		s.heading      = Field<float>(r.ped, offs::PED_ROT_CUR);
		r.host->rowSinceReport = true;
		if (cand[i].waited > g_pedLongestWait)
			g_pedLongestWait = cand[i].waited;
	}

	if (n > want && !g_saidPedsTakeTurns) {
		g_saidPedsTakeTurns = true;
		Log("population: hosting %u named peds and a batch holds %u, so they take "
		    "turns now, the nearest another player most often (%u other player "
		    "position(s) known)", n, want, viewerCount);
	}

	// The measurement §3 step 6 owes the design, kept beside the thing it
	// measures. `written` is exactly what goes on the wire this tick - the
	// client drops an empty batch rather than sending it - so the cost
	// ReportCrowd prints is arithmetic over what happened, not an estimate of
	// what should.
	g_pedRowsSent += written;
	if (written != 0)
		++g_pedPacketsSent;
	return written;
}

// Put a replica where the session says it is, and play what it says it is
// playing.
//
// Both halves are necessary and the second one is the non-obvious one. A ped
// moved without an animation glides along the pavement in its idle pose, and
// the mechanism that ought to fix that - writing m_nMoveState - has never
// worked on a non-player ped: CPed::Idle's non-still arm ends in
// `if (!IsPlayer()) SetMoveState(PEDMOVE_STILL)` and runs before SetMoveAnim
// reads it (docs/protocol.md §1.13.4). Blending the wire's animId directly is
// the only thing that has ever made a replicated ped walk.
//
// Nothing here gives the ped a wander path, an objective or a threat
// response, so nothing here lets it decide where to go. §1.13.6 is the recipe
// and SpawnAmbientReplica is where it is set up; this only ever writes a
// position, a facing and an animation that somebody else chose.
// Is the replica this row names still the object this row made?
//
// **The bug this closes.** Every other recovery in CoopIII re-arms itself:
// `ResolveRemote` takes a player's ped back when the engine flags it,
// `CorrectAmbientCarReplica` clears the handle and sets `spawnPending` when a
// car has gone from the pool, and the spawn pass then builds a new one.
// Ambient *pedestrian* replicas had the comments and not the code -
// `UpdateAmbientPedSeats` and `UpdateRemoteSeats` both say a replica the
// engine took away is rebuilt by the spawn pass, and nothing anywhere reset
// `RemoteAmbientPed::poolHandle`. So a replica the engine deleted left a row
// holding a handle that would never resolve again: no ped on screen, no
// rebuild, no despawn when the session finally dropped it, and the seat pass
// quietly retiring the standing instruction to sit in a car. Silent, for the
// rest of the session.
//
// Shaped after `CorrectAmbientCarReplica` rather than invented: same two
// tests, same two writes, same "Client will build it again on the next pass".
//
// **Why it is safe to re-arm on this test, which is the part worth being
// careful about.** A liveness check that is wrong in the *other* direction -
// declaring a live replica dead - builds a second ped while the first is
// still standing there, with nothing tracking the first. That is a
// pedestrian who exists twice on one screen, which is precisely the bug this
// branch was opened to hunt. So the test only ever says "gone" on the two
// conditions the engine itself settles:
//
//   - `CPools::GetPed` returns null. `CPool::GetAt` compares the whole pool
//     flags byte, whose top bit is the slot's free flag, so a handle stops
//     resolving the moment the ped is deleted - reused slot or not.
//     There is no transient null for a live object.
//   - the vtable is not `CCivilianPed`'s. Belt and braces on top of the
//     above, and the same test `DespawnAmbientReplica` already trusts to
//     decide whether it may run a destructor.
//
// Neither can be true of a replica that is still in the world, so the
// recovery cannot be the thing that duplicates one.
bool AmbientReplicaIsAlive(RemoteAmbientPed &ped) {
	if (ped.poolHandle < 0)
		return false;

	void *const mem = Func<GetPedFn>(CPools__GetPed)(ped.poolHandle);
	if (mem != nullptr &&
	    Field<uintptr_t>(mem, offs::VTABLE) == CCivilianPed__vtable) {
		// **The third condition, and it is a different kind of gone.**
		//
		// The two below are the object disappearing. This one is the object
		// still being there and being a corpse nobody asked for: the local
		// engine has killed a pedestrian it does not host, which is an
		// observer deciding something only the host may decide
		// (docs/population.md §5.6).
		//
		// game/combat.cpp's CPed::SetDie detour refuses almost every route to
		// this, and this is the backstop for the rest. It is deliberately a
		// test on the *state* rather than on the route, which is what makes
		// it complete: `CAutomobile::BlowUpCar`'s PED_DRIVING arm kills its
		// occupants through `CPed::SetDead` (0x004D3970) and never touches
		// CPed::SetDie at all, so no amount of guarding that one address
		// would catch it - and anything else nobody has found yet lands here
		// too.
		//
		// The recovery is the one this function already performs, which is
		// why it lives here rather than in a pass of its own: put the replica
		// down, re-arm the spawn, and let the host's stream build him again
		// on his feet where the host says he is. `Client::UpdateRemoteAmbientPeds`
		// runs this before the spawn pass, the death pass and the seat pass,
		// so the rebuild, the `deathApplied` reset and the re-seating all
		// happen on the same frame.
		//
		// `ped.dead` is what stops it being a resurrection machine: a corpse
		// the session reported is a corpse that stays down.
		if (!ReplicaDiedUnasked(Field<uint32_t>(mem, offs::PED_STATE), ped.dead))
			return true;

		++g_replicaDeathRecovered;
		if (PopTrace())
			Log("population/trace: replica net %u handle %d is dead and nobody "
			    "asked - taking it away and re-arming the spawn", ped.netId,
			    ped.poolHandle);

		static bool saidKilled = false;
		if (!saidKilled) {
			saidKilled = true;
			Log("population: our own engine killed ambient ped replica %u and "
			    "nothing in the session asked it to. Taking the body away and "
			    "rebuilding him from his host's stream - an observer does not "
			    "get to decide that somebody else's pedestrian is dead (and "
			    "this will not be said again)", ped.netId);
		}

		// The full teardown, not a dropped handle. This ped is CREATED_BY
		// MISSION and bIsLocked-equivalent for the pool: `CanBeDeleted`
		// refuses it, so nothing in the engine would ever come and collect
		// the corpse.
		DespawnAmbientReplica(ped);
		ped.spawnPending       = true;
		ped.seatedVehicleNetId = INVALID_NETID;
		ped.appliedAnimId      = ANIM_NONE;
		ped.appliedWeapon      = 0xFF;
		return false;
	}

	if (mem == nullptr)
		++g_replicaHandleLost;
	else
		++g_replicaSlotStolen;
	if (PopTrace())
		Log("population/trace: replica net %u handle %d is gone (resolves to "
		    "%p) - re-arming the spawn", ped.netId, ped.poolHandle, mem);

	// Said once. A replica being taken away is not an error - the engine owns
	// the pool and is free to - but it had never been observed at all, and a
	// silent recovery is how this one stayed invisible in the first place.
	static bool said = false;
	if (!said) {
		said = true;
		Log("population: the engine took ambient ped replica %u away from us; "
		    "re-arming its spawn (and this will not be said again)", ped.netId);
	}

	// The row is not destroyed: the session still says this pedestrian
	// exists, and the identity, the owner and the interpolation buffer are
	// all still good. Only the object is gone.
	ped.poolHandle   = -1;
	// And so is the index entry that named it. The spawn pass will put a new
	// one back when it rebuilds the replica; leaving this would have the index
	// pointing at a freed pool slot until then.
	ForgetReplica(ped.netId);
	ped.spawnPending = true;
	// Nothing is sitting in a car any more, whatever this row thought. The
	// seat pass reconciles that from `poolHandle < 0` on the same frame; this
	// is here so no reader of the row in between sees a seated ped with no
	// ped in it.
	ped.seatedVehicleNetId = INVALID_NETID;
	// And the clump went with the object, so the applied animation is not a
	// fact about anything. Same reason UnseatAmbientPed clears it.
	ped.appliedAnimId = ANIM_NONE;
	ped.appliedWeapon = 0xFF;
	if (g_replicas > 0)
		--g_replicas;

	// And give the engine its counter back, which is the part of this that is
	// easy to miss and compounds.
	//
	// SpawnAmbientReplica does `++ms_nTotalMissionPeds` because CREATE_CHAR
	// does, and DespawnAmbientReplica does the matching `--`. But **nothing
	// in the engine decrements it on a delete.** Measured, not read off re3:
	// scanning gta3.exe for the 4-byte literal 0x008F5F70 finds thirteen
	// references and no more - eight `inc dword` (FF 05), four `dec dword`
	// (FF 0D) and one `mov dword ..., 0` at 0x004F37CE, which is the Init
	// reset. Every one of the four decrements is inside the script command
	// handlers (0x0043BD70, 0x0044A9D4, 0x0044AA0E, 0x004548C1); `~CPed` is
	// not among them. So a mission ped the engine deletes by any other route
	// leaves the count one too high, for good.
	//
	// Without this, every rebuild would add one and nothing would ever take
	// it away, and `CPopulation`'s generator works off the ambient total with
	// this subtracted - so the drift is a street that gets emptier the longer
	// a session runs, which is the hardest kind of wrong to notice.
	if (Global<uint32_t>(CPopulation__ms_nTotalMissionPeds) > 0)
		--Global<uint32_t>(CPopulation__ms_nTotalMissionPeds);
	return false;
}

// Back in sight and back in the collision, if ApplyAmbientPedState took him
// out of both while he had no seat.
void ShowSeatlessReplica(RemoteAmbientPed &ped, void *mem) {
	if (!ped.seatlessHidden)
		return;
	ped.seatlessHidden = false;
	Field<uint8_t>(mem, offs::ENTITY_FLAGS_B) |= offs::ENTITY_IS_VISIBLE;
	if (!Field<bool>(mem, offs::PED_IN_VEHICLE))
		Field<uint8_t>(mem, offs::ENTITY_FLAGS_A) |= offs::ENTITY_USES_COLLISION;
}

void ApplyAmbientPedState(RemoteAmbientPed &ped, const Pose &at) {
	// The liveness check ran in the spawn pass, before anything in this frame
	// looked at the row, so a handle that is still set here is still ours.
	// Re-reading it is two loads and it keeps this function honest about the
	// object it is about to write into.
	if (ped.poolHandle < 0)
		return;
	void *const mem = Func<GetPedFn>(CPools__GetPed)(ped.poolHandle);
	if (!mem)
		return;
	// A pool slot is reused immediately, so "still resolves" is not "still
	// ours". Same vtable test every other write in this file goes through.
	if (Field<uintptr_t>(mem, offs::VTABLE) != CCivilianPed__vtable)
		return;

	// Dead is the one state a replica reaches on its own that the wire has
	// nothing to say about: the local engine's `m_fHealth <= 1.0f` auto-SetDie
	// (docs/protocol.md §1.13.5) or a stray explosion can put one there.
	// Driving a walk into a corpse stands it up mid-fall, which is exactly
	// what ApplyRemotePose refuses to do for a player.
	const uint32_t state = Field<uint32_t>(mem, offs::PED_STATE);
	if (state == PEDSTATE_DIE || state == PEDSTATE_DEAD)
		return;

	// In a car on our engine's account: the car places him and he plays the
	// seat (ped.h, KeepSeatedPose), whatever the row says.
	if (KeepSeatedPose(mem))
		return;

	// Down from our car: our engine is carrying his fall the way it would its
	// own pedestrian's, and his host's row would stand him back up where his
	// host has him (game/runover.h, RunOverHoldsPose).
	if (RunOverHoldsReplica(ped.netId, state))
		return;

	// Measured before the write, because the write is what the measurement is
	// about. Half a centimetre: a pedestrian at 1.5 m/s covers 2.5 cm in a
	// 60 Hz frame, and a replica being held at one coordinate covers exactly
	// nothing, so anything between the two separates them.
	{
		const float *const now = &Field<float>(mem, offs::POSITION);
		const float dx = at.pos.x - now[0], dy = at.pos.y - now[1],
		            dz = at.pos.z - now[2];
		++g_pedApplies;
		if (dx * dx + dy * dy + dz * dz > 0.005f * 0.005f)
			++g_pedApplyMoves;
	}

	// His host has him in a car we cannot seat him in: every seat he could
	// have is held here, or our copy of the car is not built yet
	// (RemoteAmbientPed::seatless). Where he is is that car's seat, so he is
	// put there out of sight and out of the collision: standing in it, he is
	// something the car runs into every frame.
	if (ped.seatless && !ped.seatlessHidden) {
		ped.seatlessHidden = true;
		Field<uint8_t>(mem, offs::ENTITY_FLAGS_B) &= static_cast<uint8_t>(~offs::ENTITY_IS_VISIBLE);
		Field<uint8_t>(mem, offs::ENTITY_FLAGS_A) &= static_cast<uint8_t>(~offs::ENTITY_USES_COLLISION);
		static bool said = false;
		if (!said) {
			said = true;
			Log("population: pedestrian %u sits in a car we cannot seat his copy in "
			    "yet; kept out of sight and out of its way until we can "
			    "(and this will not be said again)",
			    ped.netId);
		}
	} else if (!ped.seatless) {
		ShowSeatlessReplica(ped, mem);
	}

	const float heading = WrapAngle(at.heading);
	PlaceReplicaPed(mem, at.pos, heading, /*inWorld=*/true);
	Field<float>(mem, offs::PED_ROT_CUR)  = heading;
	Field<float>(mem, offs::PED_ROT_DEST) = heading;

	// On change, like a remote player's base animation, and again when our
	// own engine has taken a looping one off (ped.h, ReplicaAnimNeedsBlend).
	// Re-blending a live one sixty times a second would keep restarting its
	// crossfade and leave the ped permanently half-way into its own walk.
	if (ReplicaAnimNeedsBlend(mem, ped.animId, ped.appliedAnimId) &&
	    BlendReplicaAnim(mem, ped.animId))
		ped.appliedAnimId = ped.animId;
}

// A pedestrian on fire, on a screen that isn't his host's.
//
// The host's engine burns him - CFire, flee, PED_ON_FIRE, the damage and the
// death - and none of it reaches a replica on its own: the replica is
// bFireProof, so nothing here can light it, and entity fires never travel
// (addresses.h, the fire section). So the host says one bit
// (AMBIENT_PED_ON_FIRE) and this puts the same visual-only fire a burning
// player gets on the replica. It decides nothing: no NPC logic, the ped's
// health is untouched because the cause-9 arm of InflictDamage stops at
// bFireProof and combat.cpp refuses anything aimed at a replica anyway.
//
// The decision is ped.cpp's PlanRemoteFire, unchanged.
bool g_saidReplicaFireLit    = false;
bool g_saidReplicaFireNoSlot = false;

void KeepAmbientSeatedPose(RemoteAmbientPed &ped) { KeepSeatedPose(AmbientReplicaPed(ped)); }

void ApplyAmbientPedFire(RemoteAmbientPed &ped) {
	if (ped.poolHandle < 0)
		return;
	void *const mem = Func<GetPedFn>(CPools__GetPed)(ped.poolHandle);
	if (!mem || Field<uintptr_t>(mem, offs::VTABLE) != CCivilianPed__vtable)
		return;

	void *const fire = Field<void *>(mem, PED_FIRE);
	const bool  want =
	    AmbientPedShouldBurn(ped.fireSaid, ped.fireSaidMs, WallClock::NowMs());
	const bool ours = WatchedPedFireIsOurs(ped.fireSlot, mem, fire);

	using InControlFn   = bool(__thiscall *)(void *);
	const bool inControl = Func<InControlFn>(CPed__IsPedInControl)(mem);

	switch (PlanRemoteFire(want, fire != nullptr, ours, inControl)) {
	case FireAction::NOTHING:
		return;

	case FireAction::LIGHT: {
		void *const lit = LightWatchedPedFire(mem);
		if (!lit) {
			ped.fireSlot = -1;
			if (!g_saidReplicaFireNoSlot) {
				g_saidReplicaFireNoSlot = true;
				Log("population: pedestrian %u is burning on his host's screen and "
				    "all %u fire slots are taken here, so he doesn't burn on this one",
				    ped.netId, static_cast<unsigned>(NUM_FIRES));
			}
			return;
		}
		ped.fireSlot = static_cast<int8_t>(FireSlotIndex(lit));
		if (!g_saidReplicaFireLit) {
			g_saidReplicaFireLit = true;
			Log("population: pedestrian %u is burning on his host's screen; lit our "
			    "own copy on the replica in slot %d. It can't hurt him or move him - "
			    "his host's fire is the one doing that",
			    ped.netId, static_cast<int>(ped.fireSlot));
		}
		return;
	}

	case FireAction::KEEP:
		Field<uint32_t>(fire, FIRE_EXTINGUISH) =
		    Global<uint32_t>(CTimer__m_snTimeInMilliseconds) + REMOTE_FIRE_MS;
		return;

	case FireAction::EXTINGUISH: {
		using ExtinguishFn = void(__thiscall *)(void *);
		Func<ExtinguishFn>(CFire__Extinguish)(fire);
		ped.fireSlot = -1;
		return;
	}
	}
}

// Seat a replica in a replica. The engine work is game/ped.cpp's - the
// SetObjective-then-WarpPedIntoCar order is a precondition rather than a
// convention, and a third hand-written copy of it is a third chance to get it
// wrong.
int32_t SeatAmbientPed(RemoteAmbientPed &ped, int32_t carHandle, uint16_t carNetId,
                       uint8_t seat) {
	if (ped.poolHandle < 0 || carHandle < 0)
		return AMBIENT_SEAT_REFUSED;
	void *const mem = Func<GetPedFn>(CPools__GetPed)(ped.poolHandle);
	if (!mem || Field<uintptr_t>(mem, offs::VTABLE) != CCivilianPed__vtable)
		return AMBIENT_SEAT_REFUSED;
	void *const vehicle = AmbientCarFromRef(carHandle);
	if (!vehicle)
		return AMBIENT_SEAT_REFUSED;

	// A seat is his to take or not before anything is written into him.
	const int got = SeatReplicaPed(mem, vehicle, seat);
	if (got >= 0) {
		ShowSeatlessReplica(ped, mem);
		++g_seatedReplicas;
		// Once, and it is the only positive witness this half of the change
		// has. Everything else about a driver shows up as an absence: a ped
		// that was never seated is a man standing in the road, which is
		// exactly what it looked like before.
		static bool said = false;
		if (!said) {
			said = true;
			Log("population: a replicated traffic driver is in his car - ped %u "
			    "in seat %d of car %u (and this will not be said again)",
			    ped.netId, got, carNetId);
		}
		return got;
	}
	if (got == AMBIENT_SEAT_NONE_FREE)
		return got;

	// Said once. A driver who will not sit down is the visible half of this
	// whole change failing, and a per-frame line at sixty frames a second
	// would bury it.
	static bool said = false;
	if (!said) {
		said = true;
		Log("population: ambient ped %u would not take seat %u of car %u; it "
		    "stays on foot (and this will not be said again)", ped.netId, seat,
		    carNetId);
	}
	return AMBIENT_SEAT_REFUSED;
}

// His host's pedestrian is at a door of that car: our copy opens the same one
// (AMBIENT_PED_ENTERING). The engine plays it and ends it by seating him, as it
// does a player's copy, and the warp stands behind it in the seat loop.
bool BeginAmbientPedEntry(RemoteAmbientPed &ped, int32_t carHandle, uint8_t seat,
                          uint8_t doorSeat) {
	void *const mem     = AmbientReplicaPed(ped);
	void *const vehicle = carHandle >= 0 ? AmbientCarFromRef(carHandle) : nullptr;
	if (!mem || !vehicle || ped.seatlessHidden)
		return false;
	if (!StartReplicaCarEntry(mem, vehicle, seat, doorSeat))
		return false;
	ped.enterWatch.Begin(WallClock::NowMs(), CarEntryMark(mem));
	if (!g_saidReplicaAtDoor) {
		g_saidReplicaAtDoor = true;
		Log("population: pedestrian %u opens the door of seat %u of the car his host "
		    "has him getting into, here too", ped.netId, doorSeat);
	}
	return true;
}

uint8_t PollAmbientPedEntry(RemoteAmbientPed &ped, int32_t carHandle, uint8_t seat,
                            int32_t &got) {
	got = -1;
	void *const mem     = AmbientReplicaPed(ped);
	void *const vehicle = carHandle >= 0 ? AmbientCarFromRef(carHandle) : nullptr;
	if (!mem || !vehicle)
		return SEAT_LOST;
	const uint8_t progress = PollCarEntry(mem, vehicle, seat);
	if (progress == SEAT_DONE) {
		got = WireSeatOf(vehicle, mem);
		++g_seatedReplicas;
		return SEAT_DONE;
	}
	if (progress == SEAT_RUNNING && ped.enterWatch.Stalled(WallClock::NowMs(), CarEntryMark(mem)))
		return SEAT_LOST;
	return progress;
}

void AbandonAmbientPedEntry(RemoteAmbientPed &ped) {
	if (void *const mem = AmbientReplicaPed(ped))
		CancelCarEntry(mem);
	// Whatever the door blended is gone, so what the row says is played again.
	ped.appliedAnimId = ANIM_NONE;
}

bool BeginAmbientPedExit(RemoteAmbientPed &ped) {
	void *const mem = AmbientReplicaPed(ped);
	if (!mem || !StartCarExit(mem))
		return false;
	if (!g_saidReplicaOutOfDoor) {
		g_saidReplicaOutOfDoor = true;
		Log("population: pedestrian %u climbs out of his seat here as his host's does", ped.netId);
	}
	return true;
}

uint8_t AmbientPedDoorState(const RemoteAmbientPed &ped) {
	void *const mem = AmbientReplicaPed(ped);
	if (!mem || !Field<bool>(mem, offs::PED_IN_VEHICLE))
		return AMBIENT_DOOR_OUT;
	const uint32_t state = Field<uint32_t>(mem, offs::PED_STATE);
	return state == PEDSTATE_EXIT_CAR ? AMBIENT_DOOR_LEAVING : AMBIENT_DOOR_IN;
}

int32_t MoveAmbientPedSeat(RemoteAmbientPed &ped, int32_t carHandle, uint8_t seat) {
	void *const mem     = AmbientReplicaPed(ped);
	void *const vehicle = carHandle >= 0 ? AmbientCarFromRef(carHandle) : nullptr;
	if (!mem || !vehicle || !Field<bool>(mem, offs::PED_IN_VEHICLE) ||
	    Field<void *>(mem, offs::PED_MY_VEHICLE) != vehicle)
		return -1;
	return MovePassengerToSeat(vehicle, mem, seat);
}

void UnseatAmbientPed(RemoteAmbientPed &ped) {
	if (ped.poolHandle < 0)
		return;
	void *const mem = Func<GetPedFn>(CPools__GetPed)(ped.poolHandle);
	if (!mem || Field<uintptr_t>(mem, offs::VTABLE) != CCivilianPed__vtable)
		return;
	UnseatReplicaPed(mem);
	if (g_seatedReplicas > 0)
		--g_seatedReplicas;
	// Whatever was driven into this ped went with the seat: the warp changed
	// its state and its animations. Forgetting makes the next frame re-drive
	// it, the same thing UnseatRemotePed does for a player.
	ped.appliedAnimId = ANIM_NONE;
	ped.appliedWeapon = 0xFF;
}

// Build somebody else's pedestrian here.
//
// The recipe is the one docs/protocol.md §1.13.6 settles for every replicated
// ped, and it is not "turn the NPC logic off" - there is no such switch.
// A ped created the COMMAND_CREATE_CHAR way as a MISSION_CHAR, left in
// PED_IDLE, never given a wander path, and holding its objective at NONE
// decides nothing: CPed::ProcessObjective skips its entire body on a null
// objective, and it is SetWanderPath - not ProcessControl - that makes a
// pedestrian choose where to walk. ProcessControl still runs, which is what
// draws the ped, drops it to the ground and plays what it is told.
//
// So nothing here suppresses anything. It just never asks for any of it.
bool SpawnAmbientReplica(RemoteAmbientPed &ped) {
	if (ped.poolHandle >= 0)
		return true;

	// The model comes off the wire: a loaded car or object id would be read
	// by CPed::SetModelIndex as a CPedModelInfo, and an unloaded one builds
	// a ped with no clump.
	if (!IsPedModel(ped.body.modelId) || !HasModelLoaded(ped.body.modelId))
		return false;

	void *const mem = Func<NewFn>(CPed__operator_new)(offs::SIZEOF_PED);
	if (!mem) {
		Log("population: the ped pool is full; cannot replicate ped %u", ped.netId);
		return false;
	}

	// The type the owner's engine gave it, so this ped lands in the same
	// engine counter its original did (population.md §1.3, and the
	// UpdatePedCount table in addresses.h). CCivilianPed is the right class
	// for every civilian type; anything else falls back to a civilian male
	// rather than constructing the wrong class, which would stamp the wrong
	// vtable and go wrong much later.
	int pedType = static_cast<int>(ped.body.pedType);
	if (pedType != PEDTYPE_CIVMALE && pedType != PEDTYPE_CIVFEMALE)
		pedType = PEDTYPE_CIVMALE;

	Func<CtorFn>(CCivilianPed__ctor)(mem, pedType,
	                                 static_cast<int>(ped.body.modelId));

	// Did the constructor finish? A half-constructed ped and a destroyed one
	// look nearly identical in memory, and this project has already spent a
	// session telling them apart. The vtable and the clump are what actually
	// distinguish the two.
	const uintptr_t vtable = Field<uintptr_t>(mem, offs::VTABLE);
	void *const     clump  = Field<void *>(mem, offs::RW_OBJECT);
	if (vtable != CCivilianPed__vtable || clump == nullptr) {
		Log("population: CCivilianPed ctor did not complete for ped %u "
		    "(vtable %08X, clump %p) - abandoning the slot", ped.netId,
		    static_cast<unsigned>(vtable), clump);
		// Leaked deliberately. An object whose constructor did not finish
		// must never be run through a destructor.
		return false;
	}

	// MISSION_CHAR, for the same reason a remote player is one: every engine
	// sweep that reaps a pedestrian gates on CPed::CanBeDeleted, which comes
	// down to this byte. A replica left as RANDOM_CHAR is ambient population
	// to the local engine and gets deleted within seconds - while the machine
	// that actually hosts it still has it standing there.
	Field<uint8_t>(mem, offs::PED_CHAR_CREATED_BY) = CHAR_CREATED_BY_MISSION;
	Field<uint8_t>(mem, offs::PED_FLAGS_C) &=
	    static_cast<uint8_t>(~offs::PED_RESPONDS_TO_THREATS);
	Field<uint8_t>(mem, offs::PED_FLAGS_G) &=
	    static_cast<uint8_t>(~offs::PED_ALLOW_MEDICS);

	// The off switch, such as it is. CPed::ProcessObjective's first two
	// instructions are `cmp dword [ebx+164h],0 / je` straight to the end, so
	// an objective of NONE means the ped decides nothing whatever state it is
	// in. The constructor should already leave these clear; writing them is
	// four stores and removes the assumption.
	Field<uint32_t>(mem, offs::PED_OBJECTIVE)      = OBJECTIVE_NONE;
	Field<uint32_t>(mem, offs::PED_PREV_OBJECTIVE) = OBJECTIVE_NONE;
	Field<void *>(mem, offs::PED_CAR_IN_OBJECTIVE) = nullptr;

	// Observers do not decide damage. Same five flags a remote player gets,
	// and the fifth is separate because CPed::InflictDamage tests
	// bExplosionProof on its own for every blast cause.
	Field<uint8_t>(mem, offs::ENTITY_FLAGS_C) |= static_cast<uint8_t>(
	    offs::ENTITY_BULLET_PROOF | offs::ENTITY_FIRE_PROOF |
	    offs::ENTITY_COLLISION_PROOF | offs::ENTITY_MELEE_PROOF);
	Field<uint8_t>(mem, offs::ENTITY_FLAGS_B) |= offs::ENTITY_EXPLOSION_PROOF;

	// Place before adding: CWorld::Add files the entity into the sector grid
	// by its position, and nothing re-reads it afterwards, so adding first
	// files the ped at the origin - which in Liberty City is water.
	void *const matrix = reinterpret_cast<uint8_t *>(mem) + offs::MATRIX;
	float       yaw    = 0.0f;
	FiniteOr(ped.body.heading, 0.0f, yaw);
	yaw = WrapAngle(yaw);
	Func<RotateFn>(CMatrix__SetRotate)(matrix, 0.0f, 0.0f, yaw);

	float *const p = &Field<float>(mem, offs::POSITION);
	p[0]           = ClampToWorld(ped.body.pos.x);
	p[1]           = ClampToWorld(ped.body.pos.y);
	FiniteOr(ped.body.pos.z, 0.0f, p[2]);

	Func<ThisFn>(CMatrix__UpdateRW)(matrix);
	Func<ThisFn>(CEntity__UpdateRwFrame)(mem);
	Field<float>(mem, offs::PED_ROT_CUR)  = yaw;
	Field<float>(mem, offs::PED_ROT_DEST) = yaw;

	{
		// Our own Add. Suppressed, or the detour would hand this replica
		// straight back to the client as a pedestrian this machine created
		// and announce somebody else's ped to the session as ours.
		ReplicaScope scope;
		Func<AddFn>(CWorld__Add)(mem);
	}

	// LEVEL_IGNORE, not the level this position falls in. The engine drops
	// entities whose m_nZoneLevel disagrees with CGame::currLevel, and a
	// networked entity has no island - the local player ped uses the same
	// value for the same reason.
	Field<int8_t>(mem, offs::ZONE_LEVEL) = LEVEL_IGNORE;
	++Global<uint32_t>(CPopulation__ms_nTotalMissionPeds);

	ped.poolHandle = Func<RefFn>(CPools__GetPedRef)(mem);
	// A new ped, in sight and in the collision whatever the last one was.
	ped.seatlessHidden = false;
	// And the reverse index, so game/combat.cpp can recognise this object when
	// the engine hands it back as the thing a bullet just hit. One of the three
	// places `poolHandle` changes; the other two forget.
	RememberReplica(mem, ped.poolHandle, ped.netId, ped.body.pedType);
	++g_replicas;
	if (PopTrace())
		Log("population/trace: replica spawn net %u model %u owner %u ped %p "
		    "handle %d at (%.1f %.1f %.1f)", ped.netId, ped.body.modelId,
		    ped.ownerPlayerId, mem, ped.poolHandle,
		    static_cast<double>(p[0]), static_cast<double>(p[1]),
		    static_cast<double>(p[2]));
	return true;
}

void DespawnAmbientReplica(RemoteAmbientPed &ped) {
	if (ped.poolHandle < 0)
		return;

	void *const mem = Func<GetPedFn>(CPools__GetPed)(ped.poolHandle);
	if (PopTrace())
		Log("population/trace: replica gone net %u model %u ped %p handle %d "
		    "resolves %d", ped.netId, ped.body.modelId, mem, ped.poolHandle,
		    mem != nullptr ? 1 : 0);
	ped.poolHandle  = -1;
	// Forgotten here and not after the destructor, so there is no window in
	// which the index names an object this function is in the middle of
	// dismantling. Nothing in the teardown below can reach InflictDamage, but
	// the ordering costs nothing and removes the question.
	ForgetReplica(ped.netId);
	if (!mem)
		return;   // the engine already took it; nothing left to do

	// The slot still resolves, which is not the same as it still being ours.
	// Pool slots are reused immediately, so a stale handle resolves into
	// whatever took the slot - and destroying that would be destroying
	// somebody else's ped. The vtable is the cheap half of the test.
	const uintptr_t vtable = Field<uintptr_t>(mem, offs::VTABLE);
	if (vtable != CCivilianPed__vtable)
		return;

	// game/teardown.h, which unlinks from the moving list by hand first: a
	// replica is put where the wire says and then stands perfectly still,
	// which is what puts a CPhysical to sleep, and CWorld::Remove only
	// unlinks a ped that is not asleep. That asymmetry is the side of it that
	// crashed the game once.
	//
	// ~CPed's first statement is CWorld::Remove(this), so the destructor
	// reaches our own Remove hook. Suppressed for the same reason the Add is:
	// this is not a pedestrian leaving the session, it is the session taking
	// a replica away.
	{
		ReplicaScope scope;
		if (!DestroyPed(mem, /*countedMissionPed=*/true, "a pedestrian replica's despawn"))
			return;
	}

	if (g_replicas > 0)
		--g_replicas;
}

} // namespace

bool InstallPopulationHooks() {
	bool ok = true;
	if (!g_addDetour.IsInstalled()) {
		ok = g_addDetour.Install("CWorld::Add",
		                         reinterpret_cast<void *>(CWorld__Add),
		                         reinterpret_cast<void *>(&AddHook)) && ok;
	}
	if (!g_removeDetour.IsInstalled()) {
		ok = g_removeDetour.Install("CWorld::Remove",
		                            reinterpret_cast<void *>(CWorld__Remove),
		                            reinterpret_cast<void *>(&RemoveHook)) && ok;
	}
	if (!ok)
		Log("population: the CWorld hooks did not install; ambient peds will "
		    "not be shared this session");

	// Not part of `ok`. Without it limbs stay on for observers, which is how
	// every session before protocol 17 looked; the pedestrians themselves
	// are still shared.
	if (!g_bodyPartDetour.IsInstalled() &&
	    !g_bodyPartDetour.Install("CPed::RemoveBodyPart",
	                              reinterpret_cast<void *>(CPed__RemoveBodyPart),
	                              reinterpret_cast<void *>(&BodyPartHook)))
		Log("population: the CPed::RemoveBodyPart hook did not install; limbs "
		    "will only come off on the machine hosting the pedestrian");
	return ok;
}

// The session is gone, and the names it gave our crowd and our traffic went
// with it. Everything still alive here is announced again as though it had
// just been born, and whatever was queued about the old names is dropped:
// kept, our whole crowd was invisible to everybody else under ids the server
// had already let go of, the state batches spent their rows on those, and
// after a server restart the old ids named somebody else's things.
void RestartHostedNames() {
	g_bornCount    = 0;
	g_lostCount    = 0;
	g_bornCarCount = 0;
	g_lostCarCount = 0;
	g_letGoCount   = 0;
	g_deathCount   = 0;
	g_limbCount    = 0;
	UnownedBlast stale[8];
	while (g_ambientWrecks.Drain(stale, 8) != 0) {
	}

	uint32_t peds = 0, cars = 0;
	for (HostedPed &h : g_hosted) {
		if (!h.active)
			continue;
		void *const now =
		    h.poolHandle >= 0 ? Func<GetPedFn>(CPools__GetPed)(h.poolHandle) : nullptr;
		if (now != h.ped || g_bornCount >= MAX_QUEUED) {
			h = HostedPed{};
			continue;
		}
		HostedPed fresh{};
		fresh.active     = true;
		fresh.ped        = h.ped;
		fresh.poolHandle = h.poolHandle;
		fresh.modelId    = h.modelId;
		fresh.tempId     = g_nextTempId++;
		if (g_nextTempId == 0)
			g_nextTempId = 1;
		h = fresh;
		LocalAmbientPed &out = g_born[g_bornCount++];
		out.tempId = h.tempId;
		FillPedBirth(h.ped, out.body);
		++peds;
	}
	for (HostedCar &c : g_hostedCars) {
		if (!c.active)
			continue;
		void *const now = c.poolHandle >= 0 ? AmbientCarFromRef(c.poolHandle) : nullptr;
		AmbientCarBody body{};
		if (now != c.vehicle || g_bornCarCount >= MAX_QUEUED_CARS ||
		    !SampleAmbientCarIdentity(c.vehicle, body)) {
			c = HostedCar{};
			continue;
		}
		HostedCar fresh{};
		fresh.active     = true;
		fresh.vehicle    = c.vehicle;
		fresh.poolHandle = c.poolHandle;
		fresh.tempId     = g_nextCarTempId++;
		if (g_nextCarTempId == 0)
			g_nextCarTempId = 1;
		c = fresh;
		LocalAmbientCar &out = g_bornCars[g_bornCarCount++];
		out.tempId = c.tempId;
		out.body   = body;
		++cars;
	}
	if (peds != 0 || cars != 0)
		Log("population: the session is gone; %u ped(s) and %u car(s) of ours "
		    "will be announced again to the next one", peds, cars);
}

void RemovePopulationHooks() {
	g_addDetour.Remove();
	g_removeDetour.Remove();
	g_bodyPartDetour.Remove();
	g_limbCount  = 0;
	g_deathCount = 0;
	// The replicas themselves are taken away by Client::ClearAmbientPeds, which
	// goes through DespawnAmbientReplica and therefore forgets each one. This is
	// the backstop for a teardown that did not: an index entry naming a freed
	// pool slot would answer yes to the first civilian to inherit it.
	for (ReplicaIdentity &id : g_replicaIndex)
		id = ReplicaIdentity{};
	g_warnedReplicaIndexFull = false;
	for (HostedPed &h : g_hosted)
		h = HostedPed{};
	g_bornCount = 0;
	g_lostCount = 0;
	for (HostedCar &c : g_hostedCars)
		c = HostedCar{};
	g_bornCarCount = 0;
	g_lostCarCount = 0;
	g_letGoCount   = 0;
	g_viewerCount  = 0;
}

// A traffic car has become a session car (protocol.h, S_CarPromoted), and this
// is the half of it that belongs to this file.
//
// The machine that was hosting the car has to stop hosting it, and it must not
// do that through QueueLostCar. That one queues a C_CarDespawn, which is the
// right statement for a car the engine has taken away - "it stopped existing,
// drop your replicas". A promoted car is the opposite statement: the netId
// lives on and every machine keeps the object under it. Sending a despawn here
// would have every observer destroy the replica the promotion just told them
// to keep, and the driver would be left in a car nobody else can see.
//
// So the row is simply forgotten. Nothing goes on the wire, this machine stops
// streaming it in SampleHostedCars, and the wreck poll in SweepHostedCars stops
// watching it - which is correct, because the car has a driver now and a driven
// car's destruction travels on the vehicle blast path instead.
//
// Written against the pool handle rather than the pointer, the same discipline
// the rest of this file keeps: a stale pointer starts matching an unrelated
// taxi the moment the slot is reused.
void AdoptPromotedCarHere(RemoteVehicle &vehicle, bool weHostedIt) {
	if (weHostedIt) {
		for (HostedCar &c : g_hostedCars) {
			if (!c.active || c.netId != vehicle.netId)
				continue;
			Log("population: the session has given traffic car %u a driver, so "
			    "this machine stops hosting it - the car stays exactly where "
			    "it is and only the bookkeeping moves", c.netId);
			ForgetMissionEntity(c.vehicle);
			c = HostedCar{};
			break;
		}
	}
	AdoptPromotedCar(vehicle, weHostedIt);
}

// ---- a leaver's crowd (protocol.h, S_AmbientAdopt) --------------------------
//
// The session has made this machine the host of a pedestrian or a car it only
// had a replica of, because the player whose engine made it has gone. The
// replica is already a real CCivilianPed or CAutomobile standing exactly where
// everybody else has it, so nothing is built: it is taken off the replica
// books, the replica-only flags go back to what a generated one has
// (game/adopt.h), it is given the engine's own reason to move, and it is filed
// here as hosted and named under the netId it already has. From the next
// stream tick it goes out in our batches like any ped or car of ours, and our
// own engine reaps it like any other.
//
// False, with nothing changed, when it cannot be done here; Client then lets
// go of it as its new owner.

namespace {

bool g_saidPedAdopted = false;
bool g_saidAdoptFull  = false;

bool AdoptAmbientPed(RemoteAmbientPed &ped) {
	void *const mem = AmbientReplicaPed(ped);
	if (!mem)
		return false;
	const uint32_t state = Field<uint32_t>(mem, offs::PED_STATE);
	if (state == PEDSTATE_DIE || state == PEDSTATE_DEAD)
		return false;

	HostedPed *slot = nullptr;
	for (HostedPed &h : g_hosted)
		if (!h.active) {
			slot = &h;
			break;
		}
	if (!slot) {
		if (!g_saidAdoptFull) {
			g_saidAdoptFull = true;
			Log("population: hosting %zu peds already; one we were handed is let go "
			    "instead (and this will not be said again)", MAX_HOSTED);
		}
		return false;
	}

	// Our copy of his host's fire goes out. It was lit for the look of him on a
	// fireproof ped; on one who can burn it would be this machine setting him
	// alight.
	if (ped.fireSlot >= 0) {
		void *const fire = Field<void *>(mem, PED_FIRE);
		if (fire && WatchedPedFireIsOurs(ped.fireSlot, mem, fire))
			Func<void(__thiscall *)(void *)>(CFire__Extinguish)(fire);
		ped.fireSlot = -1;
	}

	AdoptPedBytes b;
	b.createdBy = Field<uint8_t>(mem, offs::PED_CHAR_CREATED_BY);
	b.entityB   = Field<uint8_t>(mem, offs::ENTITY_FLAGS_B);
	b.entityC   = Field<uint8_t>(mem, offs::ENTITY_FLAGS_C);
	b.pedC      = Field<uint8_t>(mem, offs::PED_FLAGS_C);
	b.pedG      = Field<uint8_t>(mem, offs::PED_FLAGS_G);
	b.zone      = Field<int8_t>(mem, offs::ZONE_LEVEL);
	b = PedBytesAfterAdoption(b);
	Field<uint8_t>(mem, offs::PED_CHAR_CREATED_BY) = b.createdBy;
	Field<uint8_t>(mem, offs::ENTITY_FLAGS_B)      = b.entityB;
	Field<uint8_t>(mem, offs::ENTITY_FLAGS_C)      = b.entityC;
	Field<uint8_t>(mem, offs::PED_FLAGS_C)         = b.pedC;
	Field<uint8_t>(mem, offs::PED_FLAGS_G)         = b.pedG;
	Field<int8_t>(mem, offs::ZONE_LEVEL)           = b.zone;

	// SpawnAmbientReplica's ++ given back, as DespawnAmbientReplica does: he is
	// not a mission ped any more, and nothing in the engine takes it off for us.
	if (Global<uint32_t>(CPopulation__ms_nTotalMissionPeds) > 0)
		--Global<uint32_t>(CPopulation__ms_nTotalMissionPeds);
	ForgetReplica(ped.netId);
	if (g_replicas > 0)
		--g_replicas;
	if (ped.Seated() && g_seatedReplicas > 0)
		--g_seatedReplicas;

	// Somewhere to walk, the way COMMAND_CHAR_WANDER_DIR gives it: ClearAll,
	// then SetWanderPath. Only on foot - a driver is his car's business.
	const bool inCar = Field<bool>(mem, offs::PED_IN_VEHICLE);
	if (AdoptedPedWanders(inCar, state)) {
		Func<void(__thiscall *)(void *)>(CPed__ClearAll)(mem);
		Func<bool(__thiscall *)(void *, int)>(CPed__SetWanderPath)(
		    mem, WanderDirForHeading(Field<float>(mem, offs::PED_ROT_CUR)));
	}

	*slot            = HostedPed{};
	slot->active     = true;
	slot->ped        = mem;
	slot->poolHandle = ped.poolHandle;
	slot->tempId     = g_nextTempId++;
	if (g_nextTempId == 0)
		g_nextTempId = 1;
	slot->netId      = ped.netId;
	slot->named      = true;
	slot->modelId    = ped.body.modelId;
	slot->heldSinceMs = HeldSinceNow();
	// The row is about to go; nothing may despawn what is ours now.
	ped.poolHandle   = -1;

	if (PopTrace())
		Log("population/trace: adopted ped %p net %u model %u %s", mem, slot->netId,
		    slot->modelId, inCar ? "in a car" : "on foot");
	if (!g_saidPedAdopted) {
		g_saidPedAdopted = true;
		Log("population: took over pedestrian %u from the replica we had - %s",
		    slot->netId,
		    inCar ? "he stays in his seat" : "he walks on under our own engine");
	}
	return true;
}

bool AdoptAmbientCar(RemoteAmbientCar &car) {
	if (car.poolHandle < 0)
		return false;
	void *const v = AmbientCarFromRef(car.poolHandle);
	if (!v || IsWreckedCar(v))
		return false;

	HostedCar *slot = nullptr;
	for (HostedCar &c : g_hostedCars)
		if (!c.active) {
			slot = &c;
			break;
		}
	if (!slot) {
		if (!g_saidAdoptFull) {
			g_saidAdoptFull = true;
			Log("population: hosting %zu cars already; one we were handed is let go "
			    "instead (and this will not be said again)", MAX_HOSTED_CARS);
		}
		return false;
	}
	if (!AdoptAmbientCarReplica(car))
		return false;

	*slot            = HostedCar{};
	slot->active     = true;
	slot->vehicle    = v;
	slot->poolHandle = car.poolHandle;
	slot->tempId     = g_nextCarTempId++;
	if (g_nextCarTempId == 0)
		g_nextCarTempId = 1;
	slot->netId      = car.netId;
	slot->named      = true;
	// The dents everybody already has, so only new ones go out.
	slot->sentPanels = car.damagePanels;
	slot->sentDoors  = car.damageDoors;
	slot->modelId    = car.body.modelId;
	slot->heldSinceMs = HeldSinceNow();

	// A police car handed to us with a cop at the wheel is police here too.
	// CCarCtrl::GenerateOneRandomCar marks one it makes through
	// CVehicle::ChangeLawEnforcerState(true), which a replica never went
	// through, and CCarAI::UpdateCarAI only sets a car on our wanted player
	// when that bit is up. The same call, for the same reason CPed::SetEnterCar
	// makes it for a cop getting into a police vehicle; it counts the car in
	// NumLawEnforcerCars, which is what it now is.
	void *const driver = Field<void *>(v, offs::VEH_DRIVER);
	if (driver && Field<uintptr_t>(driver, offs::VTABLE) == CCopPed__vtable &&
	    CopTypeForCarModel(car.body.modelId) != COP_TYPE_NONE &&
	    (Field<uint8_t>(v, offs::VEH_FLAGS_A) & offs::VEH_IS_LAW_ENFORCER) == 0)
		Func<void(__thiscall *)(void *, int)>(CVehicle__ChangeLawEnforcerState)(v, 1);

	if (g_carReplicas > 0)
		--g_carReplicas;
	car.poolHandle = -1;
	return true;
}

} // namespace

// ---- police for a wanted player on another machine (protocol.h, C_CopHandover)
//
// A CCopPed chases FindPlayerPed() and nobody else, so one of ours beside a
// wanted player whose stars live on another machine walks straight past him.
// game/wanted.h, ShouldHandCopOver, decides which of ours go to him; here they
// are let go of - out of our books, then out of our world - and the wanted
// player's machine builds a real CCopPed in place of its replica.

namespace {

using CopCtorFn   = void(__thiscall *)(void *, int);
using SetUpFn     = void *(__thiscall *)(void *);
using SetUpPassFn = void *(__thiscall *)(void *, int);
using SayFn       = void(__thiscall *)(void *, uint16_t);

bool     g_saidCopBuilt   = false;

bool IsCopPed(void *ped) {
	return ped != nullptr && Field<uintptr_t>(ped, offs::VTABLE) == CCopPed__vtable;
}

// One of ours that may go at all: a CCopPed our own population code made,
// named, alive. A mission's cop is the mission's and never goes.
HostedPed *GiveableCop(void *ped) {
	if (!IsCopPed(ped))
		return nullptr;
	HostedPed *h = FindHostedByPed(ped);
	if (!h || !h->named || h->mission || h->netId == INVALID_NETID)
		return nullptr;
	if (Field<uint8_t>(ped, offs::PED_CHAR_CREATED_BY) != CHAR_CREATED_BY_RANDOM)
		return nullptr;
	const uint32_t state = Field<uint32_t>(ped, offs::PED_STATE);
	if (state == PEDSTATE_DIE || state == PEDSTATE_DEAD)
		return nullptr;
	return h;
}

uint32_t HeldFor(uint32_t heldSinceMs, uint32_t nowMs) {
	return heldSinceMs == 0 ? COP_HANDOVER_HOLD_MS : nowMs - heldSinceMs;
}

uint32_t DrainCopHandovers(const WantedViewer *wanted, uint32_t count, uint32_t nowMs,
                           LocalCopHandover *out, uint32_t max) {
	if (max == 0 || count == 0 || !wanted)
		return 0;
	void *const player = PlayerPed();
	if (!player)
		return 0;

	// Our own player: his stars, and where the population centres on him.
	bool  localWanted = false;
	float lx = 0.0f, ly = 0.0f;
	if (void *const w = Field<void *>(player, offs::PLAYER_PED_WANTED))
		localWanted = Field<int32_t>(w, offs::WANTED_LEVEL) > 0;
	const float *c = Func<const float *(__cdecl *)(int32_t)>(FindPlayerCentreOfWorld)(
	    static_cast<int32_t>(Global<uint8_t>(CWorld__PlayerInFocus)));
	const bool haveLocal = c != nullptr;
	if (haveLocal) {
		lx = c[0];
		ly = c[1];
	}
	auto localD = [&](const Vec3 &at) {
		if (!haveLocal)
			return -1.0f;
		const float dx = at.x - lx, dy = at.y - ly;
		return std::sqrt(dx * dx + dy * dy);
	};

	uint32_t n = 0;

	// Police cars first, so their cops go with them: a car and the people in
	// it always change hands together (server/core/adopt.h). Only a car whose
	// every occupant is a cop of ours we may give; a stolen police car, or one
	// with somebody else's ped in it, stays.
	for (HostedCar &car : g_hostedCars) {
		if (n >= max)
			break;
		if (!car.active || !car.named || car.mission || car.netId == INVALID_NETID)
			continue;
		void *const v = car.poolHandle >= 0 ? AmbientCarFromRef(car.poolHandle) : nullptr;
		if (!v || v != car.vehicle || IsWreckedCar(v))
			continue;
		const uint16_t model = static_cast<uint16_t>(Field<int16_t>(v, offs::MODEL_INDEX));
		if (CopTypeForCarModel(model) == COP_TYPE_NONE)
			continue;
		void *const driver = Field<void *>(v, offs::VEH_DRIVER);
		if (!driver)
			continue;   // parked: nobody to chase anybody with

		HostedPed *occ[1 + offs::VEH_MAX_PASSENGERS] = {};
		uint8_t    k     = 0;
		bool       whole = true;
		auto take = [&](void *p) {
			if (!p)
				return;
			HostedPed *h = GiveableCop(p);
			if (!h) {
				whole = false;
				return;
			}
			occ[k++] = h;
		};
		take(driver);
		uint8_t seats = Field<uint8_t>(v, offs::VEH_NUM_MAX_PASSENGERS);
		if (seats > offs::VEH_MAX_PASSENGERS)
			seats = static_cast<uint8_t>(offs::VEH_MAX_PASSENGERS);
		for (uint8_t i = 0; i < seats; ++i)
			take(Field<void *>(v, offs::VEH_PASSENGERS + 4u * i));
		if (!whole || k == 0 || k > MAX_LET_GO_PEDS)
			continue;

		const Vec3 at = ReadVec3(v, offs::POSITION);
		float          targetD = 0.0f;
		const uint8_t  target  = NearestWantedViewer(at.x, at.y, wanted, count, targetD);
		if (!ShouldHandCopOver(localWanted, localD(at), target, targetD,
		                       HeldFor(car.heldSinceMs, nowMs)))
			continue;

		// Out of our world first, and only then out of our books: a car the
		// teardown refuses stays ours in every sense. The destructor flags
		// the cops in it, whom our engine deletes on its next pass; by then
		// nothing here knows them, so nothing reports them gone.
		bool gone = false;
		{
			ReplicaScope scope;
			gone = DestroyVehicle(v, "a police car handed to a wanted player");
		}
		if (!gone)
			continue;
		LocalCopHandover &o = out[n++];
		o            = LocalCopHandover{};
		o.toPlayerId = target;
		o.carNetId   = car.netId;
		for (uint8_t i = 0; i < k; ++i) {
			o.peds[o.pedCount++] = occ[i]->netId;
			*occ[i]              = HostedPed{};
		}
		car = HostedCar{};
	}

	// Then cops on foot. One in a car is his car's business, above.
	for (HostedPed &h : g_hosted) {
		if (n >= max)
			break;
		if (!h.active || h.poolHandle < 0)
			continue;
		void *const ped = Func<GetPedFn>(CPools__GetPed)(h.poolHandle);
		if (!ped || ped != h.ped || GiveableCop(ped) != &h)
			continue;
		if (Field<bool>(ped, offs::PED_IN_VEHICLE))
			continue;

		const Vec3    at      = ReadVec3(ped, offs::POSITION);
		float         targetD = 0.0f;
		const uint8_t target  = NearestWantedViewer(at.x, at.y, wanted, count, targetD);
		if (!ShouldHandCopOver(localWanted, localD(at), target, targetD,
		                       HeldFor(h.heldSinceMs, nowMs)))
			continue;

		bool gone = false;
		{
			ReplicaScope scope;
			gone = DestroyPed(ped, /*countedMissionPed=*/false, "a cop handed to a wanted player");
		}
		if (!gone)
			continue;
		LocalCopHandover &o = out[n++];
		o            = LocalCopHandover{};
		o.toPlayerId = target;
		o.pedCount   = 1;
		o.peds[0]    = h.netId;
		h            = HostedPed{};
	}
	return n;
}

// A police replica handed to us for our stars. The replica is a CCivilianPed
// and stays one whatever we write into it, so it is taken away and a CCopPed
// built where it was, filed as ours under the same netId. Everybody else
// keeps the replica they have.
//
// On foot, through CCopPed's own constructor, placed and added the way
// SpawnAmbientReplica places and adds, and given somewhere to walk the way an
// adopted civilian is: with our player wanted, its CopAI sets it on him on its
// next ProcessControl. In a car, through CVehicle::SetUpDriver or
// SetupPassenger on the car he sat in - the engine's own way of putting the
// right cop in a police car it made - after his replica is out of the seat.
// The car itself is the car pass's (AdoptAmbientCar), which runs after the
// peds and so finds a CCopPed at the wheel.
bool AdoptAmbientCop(RemoteAmbientPed &ped) {
	void *const mem = AmbientReplicaPed(ped);
	if (!mem)
		return false;
	const uint32_t state = Field<uint32_t>(mem, offs::PED_STATE);
	if (state == PEDSTATE_DIE || state == PEDSTATE_DEAD)
		return false;

	HostedPed *slot = nullptr;
	for (HostedPed &h : g_hosted)
		if (!h.active) {
			slot = &h;
			break;
		}
	if (!slot)
		return false;

	void *cop = nullptr;
	if (Field<bool>(mem, offs::PED_IN_VEHICLE)) {
		void *const car = Field<void *>(mem, offs::PED_MY_VEHICLE);
		if (!car)
			return false;
		int seat = -2;
		if (Field<void *>(car, offs::VEH_DRIVER) == mem)
			seat = -1;
		for (int i = 0; seat == -2 && i < static_cast<int>(offs::VEH_MAX_PASSENGERS); ++i)
			if (Field<void *>(car, offs::VEH_PASSENGERS + 4u * static_cast<size_t>(i)) == mem)
				seat = i;
		const int copType =
		    CopTypeForCarModel(static_cast<uint16_t>(Field<int16_t>(car, offs::MODEL_INDEX)));
		if (seat == -2 || copType == COP_TYPE_NONE ||
		    !HasModelLoaded(PedModelForCopType(copType)) ||
		    Field<uint8_t>(car, offs::VEH_CREATED_BY) != VEHICLE_CREATED_BY_RANDOM)
			return false;

		UnseatAmbientPed(ped);
		DespawnAmbientReplica(ped);
		const size_t at = seat < 0 ? offs::VEH_DRIVER
		                           : offs::VEH_PASSENGERS + 4u * static_cast<size_t>(seat);
		if (Field<void *>(car, at) != nullptr)
			return false;   // somebody else is in the seat now; the row is let go of
		ReplicaScope scope;
		cop = seat < 0 ? Func<SetUpFn>(CVehicle__SetUpDriver)(car)
		               : Func<SetUpPassFn>(CVehicle__SetupPassenger)(car, seat);
	} else {
		const int copType = CopTypeForPedModel(ped.body.modelId);
		if (copType == COP_TYPE_NONE || !HasModelLoaded(PedModelForCopType(copType)))
			return false;
		const Vec3 pos = ReadVec3(mem, offs::POSITION);
		float      yaw = 0.0f;
		FiniteOr(Field<float>(mem, offs::PED_ROT_CUR), 0.0f, yaw);
		yaw = WrapAngle(yaw);

		DespawnAmbientReplica(ped);
		void *const fresh = Func<NewFn>(CPed__operator_new)(SIZEOF_COP_PED);
		if (!fresh)
			return false;
		Func<CopCtorFn>(CCopPed__ctor)(fresh, copType);
		if (Field<uintptr_t>(fresh, offs::VTABLE) != CCopPed__vtable ||
		    Field<void *>(fresh, offs::RW_OBJECT) == nullptr) {
			// Leaked, as SpawnAmbientReplica leaks one: never destroy an object
			// whose constructor did not finish.
			Log("population: CCopPed ctor did not complete for ped %u - abandoning the slot",
			    ped.netId);
			return false;
		}
		void *const matrix = reinterpret_cast<uint8_t *>(fresh) + offs::MATRIX;
		Func<RotateFn>(CMatrix__SetRotate)(matrix, 0.0f, 0.0f, yaw);
		float *const p = &Field<float>(fresh, offs::POSITION);
		p[0]           = pos.x;
		p[1]           = pos.y;
		p[2]           = pos.z;
		Func<ThisFn>(CMatrix__UpdateRW)(matrix);
		Func<ThisFn>(CEntity__UpdateRwFrame)(fresh);
		Field<float>(fresh, offs::PED_ROT_CUR)  = yaw;
		Field<float>(fresh, offs::PED_ROT_DEST) = yaw;
		{
			ReplicaScope scope;
			Func<AddFn>(CWorld__Add)(fresh);
		}
		Func<bool(__thiscall *)(void *, int)>(CPed__SetWanderPath)(fresh,
		                                                            WanderDirForHeading(yaw));
		cop = fresh;
	}
	if (!cop)
		return false;

	*slot             = HostedPed{};
	slot->active      = true;
	slot->ped         = cop;
	slot->poolHandle  = Func<RefFn>(CPools__GetPedRef)(cop);
	slot->tempId      = g_nextTempId++;
	if (g_nextTempId == 0)
		g_nextTempId = 1;
	slot->netId       = ped.netId;
	slot->named       = true;
	slot->modelId     = static_cast<uint16_t>(Field<int16_t>(cop, offs::MODEL_INDEX));
	slot->heldSinceMs = HeldSinceNow();
	ped.poolHandle    = -1;

	if (!g_saidCopBuilt) {
		g_saidCopBuilt = true;
		Log("population: a cop was handed to us for our stars; built CCopPed %u %s "
		    "(and this will not be said again)",
		    slot->netId, IsCopPed(cop) ? "in his place" : "- the car gave us somebody else");
	}
	return true;
}

// ---- a pedestrian speaking (protocol.h, C_PedSpeech) --------------------------

constexpr uint32_t MAX_SPEECH = 16;
LocalPedSpeech g_speech[MAX_SPEECH];
uint32_t       g_speechCount = 0;
SpeechBudget   g_speechBudget;

bool SayOn(void *ped, uint16_t sound) {
	if (!ped || !PedSpeechSoundValid(sound))
		return false;
	Func<SayFn>(CPed__Say)(ped, sound);
	return true;
}

bool SayAmbient(RemoteAmbientPed &ped, uint16_t sound) {
	return SayOn(AmbientReplicaPed(ped), sound);
}

bool SayRemotePlayer(RemotePlayer &player, uint16_t sound) {
	if (player.poolHandle < 0)
		return false;
	void *const ped = Func<GetPedFn>(CPools__GetPed)(player.poolHandle);
	if (!ped || Field<uintptr_t>(ped, offs::VTABLE) != CCivilianPed__vtable)
		return false;
	return SayOn(ped, sound);
}

uint32_t DrainPedSpeech(LocalPedSpeech *out, uint32_t max) {
	const uint32_t n = g_speechCount < max ? g_speechCount : max;
	for (uint32_t i = 0; i < n; ++i)
		out[i] = g_speech[i];
	for (uint32_t i = n; i < g_speechCount; ++i)
		g_speech[i - n] = g_speech[i];
	g_speechCount -= n;
	return n;
}

// ---- a traffic replica's controls -------------------------------------------

void ApplyAmbientCarControls(RemoteAmbientCar &car) {
	if (car.poolHandle < 0 || car.destroyed)
		return;
	void *const v = AmbientCarFromRef(car.poolHandle);
	if (!v || IsWreckedCar(v) || Field<void *>(v, offs::VEH_DRIVER) == PlayerPed())
		return;
	Field<float>(v, offs::VEH_STEER_ANGLE) = car.steer;
	Field<float>(v, offs::VEH_GAS_PEDAL)   = car.gas;
	Field<float>(v, offs::VEH_BRAKE_PEDAL) = car.brake;
}

} // namespace

// Heard by game/pedspeech.cpp's ServiceTalking hook. Our own player, or a
// pedestrian of ours the session has named, near enough another player to be
// heard, within the budget. A replica is neither, which is what keeps a line
// we were told about from going back out.
void NotePedSpoke(void *ped, uint16_t sound) {
	if (!ped || g_viewerCount == 0 || g_speechCount >= MAX_SPEECH)
		return;
	LocalPedSpeech line;
	line.sound = sound;
	if (ped == PlayerPed()) {
		line.who = SPEECH_PLAYER;
	} else {
		HostedPed *h = FindHostedByPed(ped);
		if (!h || !h->named || h->netId == INVALID_NETID)
			return;
		line.who   = SPEECH_AMBIENT;
		line.netId = h->netId;
	}
	const Vec3 at = ReadVec3(ped, offs::POSITION);
	Vec3       pts[MAX_PLAYERS];
	for (uint32_t i = 0; i < g_viewerCount; ++i)
		pts[i] = g_viewers[i].pos;
	if (!PedSpeechWorthSending(sound, NearestFlatD2(at.x, at.y, pts, g_viewerCount)))
		return;
	if (!g_speechBudget.Take(WallClock::NowMs()))
		return;
	g_speech[g_speechCount++] = line;
}

// Somebody else's cops built here, for the cops-on-foot gate (docs/wanted.md
// 6.1): a replica whose host made it a PEDTYPE_COP, still ours and standing.
uint32_t CopReplicasBuiltHere() {
	uint32_t n = 0;
	for (const ReplicaIdentity &id : g_replicaIndex) {
		if (!id.ped || id.poolHandle < 0 || id.hostPedType != PEDTYPE_COP)
			continue;
		void *const now = Func<GetPedFn>(CPools__GetPed)(id.poolHandle);
		if (now != id.ped)
			continue;
		const uint32_t state = Field<uint32_t>(now, offs::PED_STATE);
		if (state == PEDSTATE_DIE || state == PEDSTATE_DEAD)
			continue;
		++n;
	}
	return n;
}

void AddPopulationToBridge(WorldBridge &bridge) {
	// Only if the door is actually hooked. Half of this seam - replicating
	// other people's peds while never announcing our own - is worse than
	// none of it: every machine would hold everybody else's crowd and
	// contribute nothing, so the streets would fill up differently on every
	// screen and nobody would be able to say why.
	if (!g_addDetour.IsInstalled())
		return;

	bridge.DrainLocalAmbientPeds = &DrainLocalAmbientPeds;
	bridge.DrainLostAmbientPeds  = &DrainLostAmbientPeds;
	bridge.NameLocalAmbientPed   = &NameLocalAmbientPed;
	bridge.SpawnAmbientReplica   = &SpawnAmbientReplica;
	bridge.DespawnAmbientReplica = &DespawnAmbientReplica;
	bridge.AmbientReplicaIsAlive = &AmbientReplicaIsAlive;

	// The stream, and the driver link that rides it (§3 step 6). All four or
	// none, for the same all-or-nothing reason the handshake is: a machine
	// that applied everybody else's ped states while never sending its own
	// would watch a moving city and contribute a static one.
	bridge.SampleHostedPeds     = &SampleHostedPeds;
	bridge.ArmAmbientReplica    = &ArmAmbientReplica;
	bridge.ApplyAmbientPedState = &ApplyAmbientPedState;
	bridge.SeatAmbientPed       = &SeatAmbientPed;
	bridge.UnseatAmbientPed     = &UnseatAmbientPed;
	bridge.BeginAmbientPedEntry   = &BeginAmbientPedEntry;
	bridge.PollAmbientPedEntry    = &PollAmbientPedEntry;
	bridge.AbandonAmbientPedEntry = &AbandonAmbientPedEntry;
	bridge.BeginAmbientPedExit    = &BeginAmbientPedExit;
	bridge.AmbientPedDoorState    = &AmbientPedDoorState;
	bridge.MoveAmbientPedSeat     = &MoveAmbientPedSeat;
	bridge.SampleReplicaPosition = &SampleReplicaPosition;

	// Limbs (protocol version 17). Both or neither, and only with the hook
	// in: a machine that applied other people's limbs while never reporting
	// its own would show a headshot on somebody else's pedestrian and hide
	// one on its own.
	if (g_bodyPartDetour.IsInstalled()) {
		bridge.DrainAmbientBodyParts = &DrainAmbientBodyParts;
		bridge.RemoveAmbientBodyPart = &RemoveAmbientBodyPart;
		bridge.RemovePlayerBodyPart  = &RemovePlayerBodyPart;
	}

	// Deaths. Both or neither, and for the same all-or-nothing reason: a
	// machine that killed other people's pedestrians while never reporting
	// its own would watch a city of corpses and contribute a city of people
	// who cannot be shot.
	//
	// Not gated on a detour of this file's own, unlike the limbs: the witness
	// is game/combat.cpp's CPed::SetDie hook, and this file cannot see
	// whether it installed. It does not need to - NoteHostedPedDeath is
	// simply never called in a build where it failed, the queue stays empty,
	// and the cost is that our pedestrians stay standing elsewhere, which is
	// how every session before this change looked.
	bridge.DrainAmbientPedDeaths = &DrainAmbientPedDeaths;
	bridge.KillAmbientReplica    = &KillAmbientReplica;

	// A burning pedestrian. Only the receiving half needs an entry here; the
	// host's half is the bit SampleHostedPeds already writes.
	bridge.ApplyAmbientPedFire = &ApplyAmbientPedFire;
	bridge.KeepAmbientSeatedPose = &KeepAmbientSeatedPose;

	// And the traffic half. Same door, same handshake, same all-or-nothing
	// rule about installing it: a machine that replicated other people's
	// traffic without announcing its own would hold everybody's cars and
	// contribute none.
	bridge.DrainLocalAmbientCars    = &DrainLocalAmbientCars;
	bridge.DrainLostAmbientCars     = &DrainLostAmbientCars;
	// A car our engine drops beside another player goes out as a let-go
	// (game/carletgo.h). Only the Remove detour ever queues one, so with the
	// reaper's calls not taken this simply stays empty.
	bridge.DrainLetGoAmbientCars    = &DrainLetGoAmbientCars;
	bridge.DrainLetGoAmbientPeds    = &DrainLetGoAmbientPeds;
	bridge.NoteRemoteViewers        = &NoteRemoteViewers;
	bridge.NameLocalAmbientCar      = &NameLocalAmbientCar;
	bridge.SpawnAmbientCarReplica   = &SpawnAmbientCarReplicaCounted;
	bridge.DespawnAmbientCarReplica = &DespawnAmbientCarReplicaCounted;
	bridge.SampleHostedCars         = &SampleHostedCars;
	bridge.CorrectAmbientCarReplica = &CorrectAmbientCarReplica;
	// Getting into somebody else's traffic is an ownership change now, not
	// just a reason to stop correcting it (protocol.h, S_CarPromoted). Wired
	// here with the rest of the ambient seam although both live in
	// game/vehicle.cpp, because the roster half of the promotion is the
	// ambient roster and this is where that roster's seam is installed.
	bridge.LocalDrivesAmbientCar    = &LocalDrivesAmbientCar;
	bridge.AdoptPromotedCar         = &AdoptPromotedCarHere;
	bridge.HostedCarHandle          = &HostedCarHandle;
	bridge.HostedCarNetId           = &HostedCarNetId;
	bridge.AmbientBeingPulledOut    = &AmbientBeingPulledOut;
	bridge.RestartHostedNames       = &RestartHostedNames;

	// A leaver's crowd (protocol.h, S_AmbientAdopt). Both or neither: a car
	// taken over without its driver is one nobody here can drive away.
	bridge.AdoptAmbientPed = &AdoptAmbientPed;
	bridge.AdoptAmbientCar = &AdoptAmbientCar;

	// Police for a wanted player (protocol.h, C_CopHandover). Both or neither:
	// a machine that gave its cops away and could not take anybody else's
	// would only ever empty the street of them.
	bridge.DrainCopHandovers = &DrainCopHandovers;
	bridge.AdoptAmbientCop   = &AdoptAmbientCop;

	// A replica's steering, gas and brake, from the car rows.
	bridge.ApplyAmbientCarControls = &ApplyAmbientCarControls;

	// Somebody else's crowd speaking here. The sending half is
	// game/pedspeech.cpp's, and only with its hook in.
	bridge.SayAmbient      = &SayAmbient;
	bridge.SayRemotePlayer = &SayRemotePlayer;
	bridge.DrainPedSpeech  = &DrainPedSpeech;

	// And the wreck pair (docs/roadmap.md 5.8, the ambient half). Both or
	// neither, like everything else here: a machine that applied other
	// people's ambient wrecks without reporting its own would burn out cars
	// on demand and never say when its own city did.
	bridge.DrainAmbientWrecks     = &DrainAmbientWrecks;
	bridge.WreckAmbientCarReplica = &WreckAmbientCarReplica;

	// And its dents, on the same terms: reported by the host, worn by the
	// replicas.
	bridge.DrainHostedCarDamage  = &DrainHostedCarDamage;
	bridge.ApplyAmbientCarDamage = &ApplyAmbientCarDamage;
}

uint32_t HostedAmbientPedCount() {
	uint32_t n = 0;
	for (const HostedPed &h : g_hosted)
		if (h.active)
			++n;
	return n;
}

uint32_t HostedAmbientCarCount() {
	uint32_t n = 0;
	for (const HostedCar &c : g_hostedCars)
		if (c.active)
			++n;
	return n;
}

uint32_t AmbientCarReplicaCount() { return g_carReplicas; }

// Not nested and not counted: every call is on the game thread inside the
// frame pump, and the one place two of these could overlap - a replica spawn
// that itself triggered an Add - is exactly the case the flag is for.
ReplicaScope::ReplicaScope() { g_creatingReplica = true; }
ReplicaScope::~ReplicaScope() { g_creatingReplica = false; }

} // namespace coopiii::game
