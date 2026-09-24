#include "population.h"

#include "addresses.h"
#include "ped.h"
#include "pedanim.h"
#include "streampick.h"
#include "vehicle.h"
#include "wreckqueue.h"
#include "../hook/hook.h"
#include "../clock.h"
#include "../log.h"

#include <cstdlib>

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
	void    *ped        = nullptr;
	int32_t  poolHandle = -1;
	uint16_t netId      = INVALID_NETID;
};

ReplicaIdentity g_replicaIndex[MAX_REPLICA_INDEX];

// Said once. Past the table a replica still exists and still walks around; it
// simply cannot be shot, which is a visible thing and has to be said rather
// than discovered.
bool g_warnedReplicaIndexFull = false;

void RememberReplica(void *ped, int32_t poolHandle, uint16_t netId) {
	for (ReplicaIdentity &id : g_replicaIndex) {
		if (id.ped != nullptr)
			continue;
		id = ReplicaIdentity{ped, poolHandle, netId};
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
bool    g_saidCarsTakeTurns = false;

void ReportStreamTurns(float seconds);

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
bool IsAmbientPedWeShouldHost(void *entity) {
	if (!entity)
		return false;
	if (EntityType(entity) != offs::ENTITY_TYPE_PED)
		return false;
	if (Field<uint8_t>(entity, offs::PED_CHAR_CREATED_BY) != CHAR_CREATED_BY_RANDOM)
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
};

// GTA III's whole vehicle pool is 110 slots and CCarCtrl keeps a dozen-odd
// random cars alive around the player. 64 is that with a lot of room over;
// past it a car stays purely local to this machine, which is what every
// traffic car was before this file existed.
constexpr size_t MAX_HOSTED_CARS = 64;
HostedCar g_hostedCars[MAX_HOSTED_CARS];

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
//     crash between two AI cars, a traffic car an NPC shot. combat.cpp
//     deliberately does not relay those ("a car blowing up because the city
//     AI decided so already happens on every machine") - which is true of
//     every machine's *own* traffic and false of a hosted one, because a
//     replica's autopilot is zeroed and its transform is written from the
//     wire. The replica is not simulating the crash that killed the original
//     and never will.
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

void QueueLostCar(HostedCar &c) {
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
bool IsAmbientCarWeShouldHost(void *entity) {
	if (!entity)
		return false;
	if (EntityType(entity) != ENTITY_TYPE_VEHICLE)
		return false;
	if (Field<uint8_t>(entity, offs::VEH_CREATED_BY) != VEHICLE_CREATED_BY_RANDOM)
		return false;
	if ((Field<uint8_t>(entity, offs::VEH_FLAGS_A) & offs::VEH_IS_LOCKED) != 0)
		return false;
	if (Field<void *>(entity, offs::VEH_DRIVER) == PlayerPed())
		return false;
	return true;
}

// The car half of the Add detour. Called from AddHook after the engine's own
// work, on an entity that is already properly in the world.
void NoteCarAdded(void *entity) {
	if (!IsAmbientCarWeShouldHost(entity))
		return;
	if (FindHostedCar(entity))
		return;   // already ours; a second Add on one entity is its own bug

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
		return;
	}

	if (g_bornCarCount >= MAX_QUEUED_CARS) {
		if (!g_warnedBornCarsFull) {
			g_warnedBornCarsFull = true;
			Log("population: the new-car queue is full; dropping claims until "
			    "it drains (and this will not be said again)");
		}
		return;
	}

	AmbientCarBody body{};
	if (!SampleAmbientCarIdentity(entity, body))
		return;   // no model index yet; not a car worth announcing

	*slot            = HostedCar{};
	slot->active     = true;
	slot->vehicle    = entity;
	slot->poolHandle = AmbientCarRef(entity);
	slot->tempId     = g_nextCarTempId++;
	// 0 is what a backfilled S_CarSpawn carries and must never be a real
	// claim.
	if (g_nextCarTempId == 0)
		g_nextCarTempId = 1;

	LocalAmbientCar &out = g_bornCars[g_bornCarCount++];
	out.tempId = slot->tempId;
	out.body   = body;
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

	for (HostedCar &c : g_hostedCars) {
		if (!c.active)
			continue;

		void *const now =
		    c.poolHandle >= 0 ? AmbientCarFromRef(c.poolHandle) : nullptr;
		if (now != c.vehicle) {
			QueueLostCar(c);
			continue;
		}
		if (player && Field<void *>(now, offs::VEH_DRIVER) == player) {
			Log("population: the local player got into ambient car %u; handing "
			    "it to the vehicle claim path", c.netId);
			QueueLostCar(c);
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
	for (uint32_t i = 0; i < n; ++i)
		out[i] = g_bornCars[i];
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
	if (now == c->vehicle)
		return true;

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
	body.pad     = 0;
	const float *const p = &Field<float>(entity, offs::POSITION);
	body.pos     = Vec3{p[0], p[1], p[2]};
	body.heading = Field<float>(entity, offs::PED_ROT_CUR);
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

	if (!IsAmbientPedWeShouldHost(entity))
		return;
	if (FindHostedByPed(entity))
		return;   // already ours; a second Add on one entity is its own bug

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
		return;
	}

	if (g_bornCount >= MAX_QUEUED) {
		if (!g_warnedBornFull) {
			g_warnedBornFull = true;
			Log("population: the new-ped queue is full; dropping claims until "
			    "it drains (and this will not be said again)");
		}
		return;
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
	const float *const p = &Field<float>(entity, offs::POSITION);
	slot->modelId    = out.body.modelId;
	if (PopTrace())
		Log("population/trace: host add ped %p temp %u model %u type %u at "
		    "(%.1f %.1f %.1f)", entity, slot->tempId, out.body.modelId,
		    out.body.pedType, static_cast<double>(p[0]),
		    static_cast<double>(p[1]), static_cast<double>(p[2]));
}

void __cdecl RemoveHook(void *entity) {
	// Noticed before the engine's own teardown, because ~CPed calls
	// CWorld::Remove as its first statement and by the time it returns the
	// object is on its way to being freed. Reading the pointer's identity is
	// all this needs and it does that here.
	if (entity && !g_creatingReplica) {
		if (HostedPed *h = FindHostedByPed(entity))
			QueueLost(*h, "CWorld::Remove");
		if (HostedCar *c = FindHostedCar(entity))
			QueueLostCar(*c);
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

int32_t HostedCarHandle(uint16_t netId) {
	if (netId == INVALID_NETID)
		return -1;
	for (const HostedCar &c : g_hostedCars)
		if (c.active && c.named && c.netId == netId && c.poolHandle >= 0 &&
		    AmbientCarFromRef(c.poolHandle) == c.vehicle)
			return c.poolHandle;
	return -1;
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

void *AmbientReplicaPed(const RemoteAmbientPed &ped) {
	if (ped.poolHandle < 0)
		return nullptr;
	void *const mem = Func<GetPedFn>(CPools__GetPed)(ped.poolHandle);
	if (!mem || Field<uintptr_t>(mem, offs::VTABLE) != CCivilianPed__vtable)
		return nullptr;
	return mem;
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
	const uint16_t anim = PlanDeathAnim(animId, StdAnimGroupCount());
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

// Is this hosted ped sitting in a car this machine also hosts, and if so
// which one and which seat?
//
// Only an ambient car we host. A pedestrian the local player has picked up as
// a passenger is in a car the M2 claim path owns, and the seat of a ped in
// *that* is not this seam's to state - it would be two parts of CoopIII
// speaking for the same pair. A car that is not ours at all cannot be named
// to anybody, because only its host knows its netId.
bool HostedSeatOf(void *ped, uint16_t &vehicleNetId, uint8_t &seat) {
	vehicleNetId = INVALID_NETID;
	seat         = 0;
	if (!Field<bool>(ped, offs::PED_IN_VEHICLE))
		return false;
	void *const car = Field<void *>(ped, offs::PED_MY_VEHICLE);
	if (!car)
		return false;   // the car went away and left bInVehicle standing

	const HostedCar *held = FindHostedCar(car);
	if (!held || !held->named || held->netId == INVALID_NETID)
		return false;

	const int slot = SeatOfPedInCar(car, ped);
	if (slot < 0)
		return false;   // bInVehicle set and no seat: the engine's own race

	vehicleNetId = held->netId;
	seat         = static_cast<uint8_t>(slot);
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
                          uint32_t viewerCount) {
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
	uint32_t        n = 0;

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
		const bool inCar = HostedSeatOf(ped, r.vehicleNetId, r.seat);
		// Alight, as the host's own engine has it. m_pFire is set by
		// StartFire and nilled by Extinguish, so there is no second copy of
		// the answer to go stale (the same read PF_ON_FIRE makes). And what
		// is in his hand, so the replica is holding the gun his rounds come
		// out of (C_NpcShot).
		r.flags = Field<void *>(ped, PED_FIRE) ? AMBIENT_PED_ON_FIRE : 0;
		r.flags = AmbientPedFlagsWithWeapon(r.flags, HeldWeaponType(ped));

		const uint32_t state  = Field<uint32_t>(ped, offs::PED_STATE);
		const bool     corpse = state == PEDSTATE_DIE || state == PEDSTATE_DEAD;

		StreamCandidate &k = cand[n];
		k          = StreamCandidate{};
		k.row      = &h.stream;
		k.index    = n;
		k.says     = PedRowSays(r.vehicleNetId, r.seat, r.flags);
		k.deadline = (r.flags & AMBIENT_PED_ON_FIRE) ? STREAM_FIRE_DEADLINE : uint8_t{0};
		const bool seen =
		    NearestViewerDist2(ReadVec3(ped, offs::POSITION), viewers, viewerCount, k.dist2);
		k.weight = (inCar || corpse) ? uint16_t{1} : StreamWeight(seen, k.dist2);
		++n;
	}

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

	const float heading = WrapAngle(at.heading);
	PlaceReplicaPed(mem, at.pos, heading, /*inWorld=*/true);
	Field<float>(mem, offs::PED_ROT_CUR)  = heading;
	Field<float>(mem, offs::PED_ROT_DEST) = heading;

	// On change only, like a remote player's base animation. Re-blending the
	// same id sixty times a second would keep restarting its crossfade and
	// leave the ped permanently half-way into its own walk.
	if (ped.animId != ped.appliedAnimId && BlendReplicaAnim(mem, ped.animId))
		ped.appliedAnimId = ped.animId;
}

// A pedestrian on fire, on a screen that isn't his host's.
//
// The host's engine burns him - CFire, flee, PED_ON_FIRE, the damage and the
// death - and none of it reaches a replica on its own: the replica is
// bFireProof, so nothing here can light it, and entity fires never travel
// (addresses.h, the fire section). So the host says one bit
// (AMBIENT_PED_ON_FIRE) and this puts the same visual-only fire a burning
// player gets on the replica. It decides nothing: no AI, the ped's health is
// untouched because the cause-9 arm of InflictDamage stops at bFireProof and
// combat.cpp refuses anything aimed at a replica anyway.
//
// The decision is ped.cpp's PlanRemoteFire, unchanged.
bool g_saidReplicaFireLit    = false;
bool g_saidReplicaFireNoSlot = false;

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
bool SeatAmbientPed(RemoteAmbientPed &ped, RemoteAmbientCar &car, uint8_t seat) {
	if (ped.poolHandle < 0 || car.poolHandle < 0)
		return false;
	void *const mem = Func<GetPedFn>(CPools__GetPed)(ped.poolHandle);
	if (!mem || Field<uintptr_t>(mem, offs::VTABLE) != CCivilianPed__vtable)
		return false;
	void *const vehicle = AmbientCarFromRef(car.poolHandle);
	if (!vehicle)
		return false;

	if (SeatReplicaPed(mem, vehicle, seat)) {
		++g_seatedReplicas;
		// Once, and it is the only positive witness this half of the change
		// has. Everything else about a driver shows up as an absence: a ped
		// that was never seated is a man standing in the road, which is
		// exactly what it looked like before.
		static bool said = false;
		if (!said) {
			said = true;
			Log("population: a replicated traffic driver is in his car - ped %u "
			    "in seat %u of car %u (and this will not be said again)",
			    ped.netId, seat, car.netId);
		}
		return true;
	}

	// Said once. A driver who will not sit down is the visible half of this
	// whole change failing, and a per-frame line at sixty frames a second
	// would bury it.
	static bool said = false;
	if (!said) {
		said = true;
		Log("population: ambient ped %u would not take seat %u of car %u; it "
		    "stays on foot (and this will not be said again)", ped.netId, seat,
		    car.netId);
	}
	return false;
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
// The recipe is the one docs/protocol.md §1.13.6 settles for every
// replicated ped, and it is not "turn the AI off" - there is no such switch.
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
	// And the reverse index, so game/combat.cpp can recognise this object when
	// the engine hands it back as the thing a bullet just hit. One of the three
	// places `poolHandle` changes; the other two forget.
	RememberReplica(mem, ped.poolHandle, ped.netId);
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

	// The asymmetry, again, and this is the side of it that crashed the game.
	// CWorld::Remove only unlinks from ms_listMovingEntityPtrs when bIsStatic
	// is clear, so a ped that went to sleep keeps its node - and destroying it
	// leaves that node pointing at a pool slot that has just been freed.
	// A replica is exactly the entity this happens to: it is put where the
	// wire says and then stands perfectly still, which is what puts a
	// CPhysical to sleep.
	if (NeedsMovingListUnlink(Field<uint8_t>(mem, offs::ENTITY_FLAGS_A),
	                          Field<void *>(mem, offs::MOVING_LIST_NODE) != nullptr))
		Log("population: a ped replica went static while still in the moving "
		    "list; unlinking by hand, because CWorld::Remove walks past it");
	Func<ThisFn>(CPhysical__RemoveFromMovingList)(mem);

	Func<RemoveRefsFn>(CWorld__RemoveReferencesToDeletedObject)(mem);

	{
		// ~CPed's first statement is CWorld::Remove(this), so the destructor
		// reaches our own Remove hook. Suppressed for the same reason the Add
		// is: this is not a pedestrian leaving the session, it is the session
		// taking a replica away.
		ReplicaScope scope;
		void *const *vt = *reinterpret_cast<void *const *const *>(mem);
		auto deleter    = reinterpret_cast<DtorFn>(vt[VTABLE_DELETING_DTOR]);
		deleter(mem, 1);   // 1 = free the memory too
	}

	--Global<uint32_t>(CPopulation__ms_nTotalMissionPeds);
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
			c = HostedCar{};
			break;
		}
	}
	AdoptPromotedCar(vehicle, weHostedIt);
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

	// And the traffic half. Same door, same handshake, same all-or-nothing
	// rule about installing it: a machine that replicated other people's
	// traffic without announcing its own would hold everybody's cars and
	// contribute none.
	bridge.DrainLocalAmbientCars    = &DrainLocalAmbientCars;
	bridge.DrainLostAmbientCars     = &DrainLostAmbientCars;
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
	bridge.RestartHostedNames       = &RestartHostedNames;

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
