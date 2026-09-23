// The client: everything CoopIII does inside gta3.exe, above the transport.
//
// Owns the socket thread, the roster of remote players, and the two
// callbacks the frame hook drives (client/src/game/frame.h):
//
//   PreFrame()   apply inbound state, before the world updates
//   PostFrame()  sample and send the local player, after it has settled
//
// Both run on the game thread. Nothing here blocks - the socket thread has
// already done the waiting for us.
//
// No engine dependencies here on purpose. Anything that reads or writes game
// memory goes through WorldBridge below, which is the seam Area B fills once
// the ped field offsets are verified (docs/addresses-unverified.md). That
// keeps the roster, the interpolation and the send cadence testable without
// GTA III running - same reason interp.h is shaped the way it is.
#pragma once

#include "clock.h"
#include "interp.h"
#include "netthread.h"

#include <coopiii/protocol.h>

#include <cstdint>
#include <string>

namespace coopiii {

struct RemotePlayer {
	bool         active  = false;
	uint8_t      playerId = 0xFF;
	uint16_t     netId   = 0;
	std::string  nick;
	uint16_t     modelId = 0;

	InterpBuffer interp;
	// Last snapshot as received, for fields interpolation doesn't cover
	// (animation, health, weapon).
	PlayerStateBody last{};
	bool            haveState = false;

	// Where the session said this player was when we were told about them,
	// used until their own snapshots take over.
	//
	// It is not pushed into `interp`, and that is not fussiness. The buffer
	// interpolates within one sender's timeline, and this timestamp is the
	// *server's* clock while every snapshot in there carries the sender's -
	// mixing the two would have the buffer interpolating between two numbers
	// that don't measure the same thing. So it sits beside the buffer as a
	// starting pose and is dropped the moment a real snapshot arrives.
	//
	// Why it exists at all: a remote ped is not created until we know where
	// to put it, and the only thing that ever said where was a snapshot. A
	// player in the frontend, on a loading screen or in a cutscene has no ped
	// to sample and sends none, so before this a joiner could sit next to
	// them and see nothing for as long as they stayed there.
	Pose seedPose{};
	bool haveSeedPose = false;

	// Dead, as far as the session is concerned, and whether the engine has
	// been told yet.
	//
	// Two fields rather than one call at the moment the news arrives, because
	// the news can arrive before there is a ped to kill: a death during the
	// model stream, or - the case this was written for - a join packet
	// announcing somebody who has been lying in the road since before we
	// connected. UpdateRemotes drives the second toward the first, the same
	// shape as UpdateRemoteSeats.
	bool     dead         = false;
	uint16_t deathAnimId  = ANIM_NONE;
	bool     deathApplied = false;

	// Engine-side identity. -1 until Area B spawns a ped for this player.
	// docs/protocol.md §1.5: local only, never goes on the wire.
	int32_t poolHandle = -1;
	// Set once the model has been requested but the ped doesn't exist yet
	// (§1.6 - spawning is two-phase).
	bool spawnPending = false;

	// What's already been driven into the engine for this ped, so the
	// expensive stateful calls only happen on *change* instead of every
	// frame: re-blending an animation that's already playing restarts its
	// blend, and re-running CPed::SetCurrentWeapon rebuilds the weapon
	// model. Reset when the ped is created, since a new ped starts with
	// none of this applied.
	uint16_t appliedAnimId  = ANIM_NONE;
	uint16_t appliedAnimId2 = ANIM_NONE;
	uint16_t appliedWeapon  = 0xFFFF;   // not a weapon type: "nothing set yet"

	// What this player says is in each of their thirteen weapon slots, and
	// whether they have said anything about it yet.
	//
	// The wire is the only writer of this table, and that is the point of it
	// existing at all. The observer's own engine also writes ammunition into
	// the ped - CWeapon::Fire decrements both counts on a replayed shot
	// (0x0055C7D1 and 0x0055C7E9) and CCivilianPed::ProcessControl runs
	// CWeapon::Update on it every frame, which reloads on its own timer. Two
	// writers drift; docs/protocol.md 1.9.6 says which one wins, and this is
	// the copy of the winner's number.
	//
	// The slot for the weapon they are holding is kept current from the
	// snapshot rather than from a C_PlayerAmmo, so this table is complete
	// whichever hand the count arrived in.
	uint32_t ammoTotal[INVENTORY_SLOTS] = {};
	uint16_t ammoClip[INVENTORY_SLOTS]  = {};
	bool     ammoKnown[INVENTORY_SLOTS] = {};

	// Where the session says this player is sitting, versus where the
	// engine actually has them. Two fields because they're routinely
	// different: the enter event is reliable and arrives all at once, while
	// the ped and the car each take as long as their model takes to stream.
	// So `seatVehicleNetId` is a standing instruction that Client keeps
	// trying to carry out every frame, not a one-shot command.
	//
	// INVALID_NETID in either means "on foot".
	uint16_t seatVehicleNetId  = INVALID_NETID;
	uint8_t  seatIndex         = 0;           // 0 is the driver
	uint16_t seatedVehicleNetId = INVALID_NETID;

	// And a third, because getting in is not instantaneous either.
	//
	// The engine's own entry walks the ped to a door, opens it and climbs
	// in, over the best part of a second, and it can be refused or
	// interrupted at any point in that second. So while it is running this
	// player is in neither of the two states above: the session says seat n
	// of car m, the engine says "walking towards a door", and the pose
	// stream must not write a position over either of them.
	//
	// `enterDeadlineMs` is what stops that ever becoming forever. When it
	// passes, the entry is taken off the ped and the seat is given by the
	// warp instead - a player who teleports into a seat is a bad frame, and
	// a player stuck half-inside a car is a bad session.
	uint16_t enteringVehicleNetId = INVALID_NETID;
	uint32_t enterDeadlineMs      = 0;
	// One animated attempt per enter event. Cleared when the session says
	// something new about where this player is sitting; without it, an entry
	// that times out is just retried, and the ped spends the rest of the
	// session walking up to the same door.
	bool     seatAnimSpent        = false;

	// Which of gFireManager's 40 slots holds the fire CoopIII lit on this
	// player's ped, or -1 for none. An index rather than a CFire*, for the
	// same reason poolHandle is a pool reference: the slot outlives the
	// fire, so the contents have to be asked whether it is still ours
	// (docs/roadmap.md §5.7 phase three).
	int8_t fireSlot = -1;

	// A seated ped gets positioned by the engine, from the car, every frame.
	// Nothing else should write its transform - see ApplyRemotePose.
	bool Seated() const { return seatedVehicleNetId != INVALID_NETID; }

	// And so does one that is climbing in: CWorld::Process calls EnterCar()
	// on it out of the same walk over the moving list, which is what lines
	// the ped up with the door. Same rule, same reason.
	bool Entering() const { return enteringVehicleNetId != INVALID_NETID; }

	// Does this player have anything at all to do with that car right now -
	// sitting in it, or halfway through getting into it? Asked wherever a
	// car is about to be taken away, because a ped left mid-entry into a
	// destroyed car is the half-state this whole feature has to not create.
	bool InvolvedWith(uint16_t netId) const {
		return netId != INVALID_NETID &&
		       (seatedVehicleNetId == netId || enteringVehicleNetId == netId);
	}
};

// What PollSeatRemotePed says about an entry that is already in flight.
enum SeatProgress : uint8_t {
	SEAT_RUNNING = 0,   // still walking to the door, or climbing in
	SEAT_DONE    = 1,   // the engine has them in the seat
	SEAT_LOST    = 2,   // it stopped without seating them, however it stopped
};

// What SeatLocalPlayerIn and PollLocalSeatEntry say when they are not
// handing back a seat number. The entry is animated, so the seat key no
// longer has an answer on the frame it was pressed.
constexpr int32_t SEAT_LOCAL_REFUSED = -1;   // no, and the client log says why
constexpr int32_t SEAT_LOCAL_WALKING = -2;   // on the way, ask again next frame

// How long an entry animation is given before the seat is taken by force.
//
// A get-in is a four-animation chain - align, open, get in, shut - and the
// slowest of them on the slowest door comes to a little over a second. This
// is that with room to spare, and it is deliberately a wall-clock timeout
// rather than a count of frames: the thing being waited on is an animation,
// and animations advance on CTimer, which is a different clock from ours but
// the same order of magnitude. Being generous costs a late warp; being mean
// costs a warp that interrupts an animation that was about to finish.
constexpr uint32_t SEAT_ANIM_TIMEOUT_MS = 2500;

// How long to leave a refused vehicle claim alone before asking again.
//
// The server says no for two reasons and neither of them changes in a frame:
// its vehicle table is full, or the netId means nothing to it. Long enough
// that a full table is not being asked sixty times a second, short enough
// that a seat freeing up is noticed while the player is still in the car.
constexpr uint32_t CLAIM_RETRY_MS = 2000;

// The engine's PED_EXIT_CAR, as it turns up in PlayerStateBody::pedState.
//
// Repeated here because this layer is deliberately engine-free and cannot
// include game/addresses.h, where the number is verified. ped.cpp
// static_asserts the two against each other, which is the only thing that
// makes a second copy of a constant worth anything.
//
// It is the one field on the wire that says an exit has *started*. The
// reliable S_ExitVehicle is sent when the owner's own bInVehicle goes false,
// which is the end of their get-out animation - so waiting for it means
// starting the animation a whole animation late. The snapshot says it a
// second earlier, and it is already being sent.
constexpr uint8_t WIRE_PEDSTATE_EXIT_CAR = 54;

// Something the local player did with a weapon, or that a weapon did to
// them, on its way to the wire.
//
// Sampled by the detours in game/combat.h instead of read off the ped once a
// frame, because these are events and the ped only carries state - an Uzi
// empties a clip between two snapshots, and an explosion leaves nothing
// behind on the thrower to sample at all. docs/protocol.md §1.9.
//
// DAMAGE is the odd one out: it's a hit the local engine was about to apply
// to somebody else's ped and was stopped from applying. The event is what
// happens instead of the damage, not a record of it (§1.10).
struct CombatEvent {
	enum Kind : uint8_t { SHOT, EXPLOSION, DAMAGE, DEATH };
	uint8_t       kind = SHOT;
	ShotBody      shot{};
	ExplosionBody explosion{};
	DamageBody    damage{};
	// The animation the engine chose for this death, straight out of its own
	// CPed::SetDie call. ANIM_NONE if the detour that captures it isn't
	// installed, in which case the observer picks its own.
	uint16_t      deathAnimId = ANIM_NONE;
};

// The local player's own car blew up, and where it was when it did.
//
// Same reasoning as CombatEvent and the same seam shape: it comes off a
// detour on CAutomobile::BlowUpCar rather than out of the 25 Hz sample,
// because m_fHealth reaching zero is not what destroys a car - the engine
// never looks at it that way (game/vehicle.h, docs/protocol.md §1.11).
//
// No netId: the detour only knows "the car the local player is driving".
// Client is the half that knows what the session calls it.
struct LocalVehicleBlast {
	Vec3 pos{};
	Quat rot{};
};

// A Pay'n'Spray this machine's own engine just finished.
//
// No vehicle identity on it, on purpose. The car in a spray shop is by
// construction the one the local player is driving - the engine's own
// IsStaticPlayerCarEntirelyInside is what let the visit start - so the netId
// is whatever Client already knows the local player's car is called, and
// asking the engine a second time would be two answers to one question.
//
// The colours are read back *off that car* rather than chosen here. See
// game/garage.h: the engine's ChooseVehicleColour is a per-machine round
// robin, so a colour is a fact about the owner's machine and not something
// an observer may reproduce.
struct LocalRespray {
	uint8_t garage  = 0;   // index into CGarages::aGarages, 0..31
	uint8_t colour1 = 0;
	uint8_t colour2 = 0;
};

// What happened when this machine was asked to make an unowned car a wreck.
//
// docs/roadmap.md 5.8. Four outcomes rather than a bool because three of them
// mean "stop asking" and one means "ask again next frame", and a backfilled
// wreck is very often the last one: a joiner is told about a car three
// streets away that their streamer has not reached yet.
enum class UnownedWreckOutcome : uint8_t {
	// We destroyed it, through the engine's own BlowUpCar.
	Wrecked,
	// It is already a wreck here - which is the ordinary case, because the
	// explosion that did it was replayed on this machine too. Not a failure
	// and not worth a line.
	Already,
	// The car is not in this machine's world right now. Not streamed in yet,
	// or already reaped. Worth retrying for as long as the record lives.
	NotHere,
	// Nothing this machine can resolve, ever: a key kind it does not speak,
	// or a generator index outside the map's own range. Dropped.
	BadKey,
};

// One queued report that a car nobody is driving has been destroyed here.
//
// The key names the car and the transform says where it ended up, which only
// the ambient kind reads - see BlastTransform in protocol.h for why a parked
// car deliberately ignores it and what the sender puts there instead.
//
// The two together rather than the key alone, because the transform has to be
// read at the moment of detonation: by the time the queue is drained the next
// frame the car is a wreck the engine may already have started reaping, and a
// position fetched then is a position that has been settling under gravity
// since the blast.
struct UnownedBlast {
	UnownedVehicleKey key;
	BlastTransform    where;
};

// A wreck the session has told us about that we have not managed to apply.
//
// A standing instruction rather than an event, which is the shape this
// codebase keeps arriving at (UpdateRemoteSeats, the pickup loop): the car
// and the packet turn up in either order, and reconciling every frame makes
// every one of those races fall out of one loop instead of needing a handler
// each.
struct PendingUnownedWreck {
	bool              active  = false;
	UnownedVehicleKey key     = {};
	uint8_t           byPlayer = INVALID_PLAYER;
	uint32_t          sinceMs = 0;
	// Held with the instruction rather than re-read when it is carried out.
	// A wreck report is routinely retried for many frames while the car
	// streams in, and the place it blew up does not change while we wait.
	BlastTransform    where   = {};
};

// Everything the session needs to build the same car somebody else is sitting
// in: what it is, what colour it is, and which extra components are bolted to
// it. EnterVehicleBody's identity half, read off the local player's car.
//
// `extra1`/`extra2` are CVehicle::m_aExtras, -1 for an empty slot. They are
// here rather than being left to each machine because the engine rolls them at
// spawn - see game/vehicle.h, "Extras", for why they are the one field on this
// struct that cannot be applied after the car exists.
struct VehicleIdentity {
	uint16_t modelId = 0;
	uint8_t  colour1 = 0;
	uint8_t  colour2 = 0;
	int8_t   extra1  = -1;
	int8_t   extra2  = -1;
	Vec3     pos{};
	Quat     rot{0.0f, 0.0f, 0.0f, 1.0f};
};

// ---- our own life ----------------------------------------------------------
//
// Two decisions, pulled out of Client so tools/clienttest can reach them
// without a socket. They're the whole of the local death/respawn state
// machine, and both are the kind of thing that looks obviously right and
// then fires twice.

enum class LifeEvent : uint8_t { NOTHING, DIED, RESPAWNED };

// What a freshly sampled health means, given whether a death is already out
// on the wire.
//
// Health is the entire input. CGameLogic writes 0 the moment the player dies
// and 100 again at the hospital, and nothing in between ever leaves a living
// player at zero - the in-vehicle arm of CPed::InflictDamage writes 1.0f
// rather than 0.0f precisely so that stays true. Written as `!(health > 0)`
// so a NaN off a corrupt read counts as dead rather than as alive.
inline LifeEvent LifeEventFor(float health, bool deathAnnounced) {
	if (!(health > 0.0f))
		return deathAnnounced ? LifeEvent::NOTHING : LifeEvent::DIED;
	return deathAnnounced ? LifeEvent::RESPAWNED : LifeEvent::NOTHING;
}

// Who gets the kill, if anyone.
//
// Recency, not proof. Whoever last damaged us, if it was recent enough to
// plausibly be the reason. A player who shot us five seconds ago and then
// watched us drown doesn't get it, and neither does anyone when we walked
// into the water on our own.
inline uint16_t KillCreditFor(uint16_t lastAttackerNetId, uint32_t lastAttackerMs,
                              uint32_t nowMs, uint32_t windowMs) {
	if (lastAttackerNetId == INVALID_NETID)
		return INVALID_NETID;
	// Unsigned subtraction, so a clock that wrapped past 2^32 reads as a
	// small elapsed time rather than an enormous one.
	return (nowMs - lastAttackerMs) <= windowMs ? lastAttackerNetId : INVALID_NETID;
}
// The time of day and the sky, as one machine's engine has them.
//
// Same four fields as WorldStateBody on the wire, kept separate because the
// engine seam below shouldn't have to speak in packets - that's the rule the
// rest of WorldBridge follows too.
struct WorldState {
	uint8_t hour       = 0;
	uint8_t minute     = 0;
	uint8_t weather    = 0;   // CWeather::NewWeatherType
	uint8_t weatherOld = 0;   // CWeather::OldWeatherType
};

// How far this machine's clock may sit from the host's before we move it.
//
// Not zero, and that's the whole design. A world packet is a round trip old
// before it arrives, so a client that matched the host exactly would be told
// to step its clock every single second, and GTA III hangs a lot on the
// minute: the HUD clock, where the sun is, whether the streetlights are on.
// Three game minutes is three seconds of real time and nothing on screen can
// show it, while anything that actually matters - a mission setting the
// time, a client that joined at a different hour - is hours out and gets
// corrected at once.
constexpr int kClockToleranceMinutes = 3;

// Signed distance from one time of day to another, in minutes, the short way
// round a 24 hour dial. 23:59 to 00:01 is +2, not +1438.
int ClockDriftMinutes(uint8_t fromHour, uint8_t fromMinute, uint8_t toHour,
                      uint8_t toMinute);

// A vehicle this machine is observing rather than simulating.
//
// Vehicles aren't per-player: a car outlives whoever was driving it, several
// players can be in one at once, and a parked one has nobody in it at all.
// So the roster is keyed by the server's netId, not by a player slot.
//
// Ownership belongs to the driver (vehicle.h). `driverPlayerId` is who the
// server says that is; 0xFF means nobody, i.e. a parked car - still synced,
// because a car pushed out of the way on one screen has to move on the other
// too.
struct RemoteVehicle {
	bool     active  = false;
	uint16_t netId   = 0;
	uint16_t modelId = 0;
	uint8_t  driverPlayerId = 0xFF;
	uint8_t  colour1 = 0, colour2 = 0;

	// CVehicle::m_aExtras as the claimer's machine rolled them, -1 for an
	// empty slot. Consumed by SpawnRemoteVehicle and only by it: they can
	// only be applied while the car is being constructed (game/vehicle.h,
	// "Extras"), so unlike the colours there is nothing useful to do with
	// them afterwards.
	int8_t   extra1 = -1, extra2 = -1;

	// Transform history, sampled every frame rather than at the snapshot
	// rate: the correction below has to undo a frame of local physics, so
	// it needs to run on every frame physics ran, not just snapshot frames.
	VehicleInterpBuffer interp;

	// Where the server last said it is, and whether it ever said anything at
	// all. Same rule as a ped: nothing gets created until we know where to
	// put it. See Client::UpdateRemotes.
	VehicleStateBody last{};
	bool             haveState = false;

	// CPools::GetVehicleRef, not a raw pointer, for the same reason as
	// RemotePlayer::poolHandle: it stops resolving the moment the engine
	// deletes the vehicle, reused slot or not.
	int32_t poolHandle   = -1;
	bool    spawnPending = false;

	// This car has been destroyed and is a wreck, here and everywhere else.
	//
	// It is not the same thing as "gone": the wreck stays in the street for a
	// minute like any other. What it stops is the respawn. The engine clears
	// a wreck away by itself about a minute after it died, through the one
	// reaping path a locked mission car does not survive (addresses.h, "the
	// reaping site that deletes a car BECAUSE it is locked"), and without
	// this the roster would notice the empty pool slot and build a brand new,
	// undamaged car in its place.
	bool destroyed = false;

	// This CVehicle is one our own engine made and we claimed, not a replica
	// CoopIII built off the wire.
	//
	// The row exists so that stepping out of our own car and back into it is
	// recognised as the same car (Client::AdoptOurClaimedVehicle). What it
	// must never do is let the roster destroy it: DespawnRemoteVehicle runs
	// the engine's deleting destructor, and on our own car that is one of
	// the player's own traffic cars - very possibly the one they are sitting
	// in - being deleted underneath them on a disconnect.
	bool ours = false;

	// Driven into the engine on change, not every frame.
	uint8_t appliedFlags = 0xFF;   // not a flag set: "nothing applied yet"

	// What shape the session says this car is in: CDamageManager's panel word
	// and six two-bit door levels (docs/cardamage.md §3).
	//
	// Held in the row rather than applied and forgotten, because a car is
	// two-phase like everything else here - the damage can arrive before the
	// model has streamed in, and the car can be reaped and respawned under us
	// - and because the merge is a maximum, so the row is the union of
	// everything anyone has reported rather than the last thing that arrived.
	uint32_t damagePanels = 0;
	uint16_t damageDoors  = 0;

	// Set when the row has damage the *car* has not been given yet: a packet
	// that arrived before the spawn, or a spawn that has just happened. Drives
	// exactly one apply pass, with the flying components turned off, because a
	// joiner handed eight damaged cars must not get a shower of doors out of
	// the object pool (docs/cardamage.md §5.2).
	bool damagePending = false;
};

// ---- ambient population (docs/population.md §3 step 2) ----------------------

// A pedestrian the local engine just created, on its way to the session.
//
// It already exists in CWorld and is already running its own AI here; this is
// only the announcement. `tempId` is this machine's private name for it and
// is meaningless anywhere else - see C_PedSpawn.
struct LocalAmbientPed {
	uint32_t       tempId = 0;
	AmbientPedBody body{};
};

// Somebody else's ambient ped, as this machine holds it.
//
// Not a RemotePlayer and not a RemoteVehicle: it has an owner that never
// changes and nothing here ever decides anything about it. Step 2 created it
// where the session said and left it there - "does it appear on the other
// screen and stay where it is put" - and step 6 is what stops it standing
// there afterwards.
struct RemoteAmbientPed {
	bool     active        = false;
	uint16_t netId         = INVALID_NETID;
	uint8_t  ownerPlayerId = INVALID_PLAYER;
	AmbientPedBody body{};

	// CPools::GetPedRef, not a pointer, for the same reason as
	// RemotePlayer::poolHandle: it stops resolving the moment the engine
	// deletes the ped, reused slot or not.
	int32_t  poolHandle   = -1;
	// The model has been asked for and the ped doesn't exist yet. Same
	// two-phase spawn as a player (protocol.md §1.6).
	bool     spawnPending = false;

	// ---- the stream (docs/population.md §3 step 6) -------------------------

	// Where the owner says it is. Fed at whatever rate the owner happens to
	// be streaming this particular ped at, which is not a fixed rate: only
	// the twelve nearest its own player are sent (protocol.h,
	// MAX_PED_STATES). The buffer copes - it is keyed on the sender's own
	// timestamps - and a ped that falls out of the twelve is held at `last`
	// rather than guessed at.
	InterpBuffer interp;
	// The last pose the session gave us, seeded from the spawn packet so it
	// is true from the first frame the replica exists rather than from the
	// first state batch.
	Pose         last{};
	// What the owner's ped is playing. ANIM_NONE means "nothing said", which
	// is not the same as "standing still" - a sender whose animation could
	// not be read leaves the replica playing whatever it already had.
	uint16_t     animId        = ANIM_NONE;
	// What CoopIII actually blended onto the clump, so the blend happens on
	// a change rather than sixty times a second. Same field and same reason
	// as RemotePlayer::appliedAnimId.
	uint16_t     appliedAnimId = ANIM_NONE;

	// ---- dead ---------------------------------------------------------------
	//
	// Recorded and then carried out, never carried out on the packet, for
	// exactly the reason RemotePlayer::dead/deathApplied exist: the death
	// arrives on CH_EVENT the moment the host's engine decides it, and the
	// replica may not exist yet - a spawn waiting on a model is several
	// frames long, and a backfilled corpse has not even been asked for when
	// its S_PedDeath lands. "If there is a ped, kill it" loses the death in
	// both cases and leaves a pedestrian walking around a session that knows
	// he is dead.
	bool     dead         = false;
	// The animation the host's engine chose, straight off its CPed::SetDie.
	// ANIM_NONE means it could not say, and the observer uses the engine's
	// own front-knockdown default.
	uint16_t deathAnimId  = ANIM_NONE;
	bool     deathApplied = false;

	// ---- the traffic driver ------------------------------------------------
	//
	// Two fields rather than one, and the pair is the same reconciliation
	// shape as RemotePlayer's `seatVehicleNetId` / `seatedVehicleNetId`:
	// what the session asks for, and what has actually been carried out.
	// Driving one toward the other on every frame is what makes every race -
	// ped first, car first, ped lost and respawned, car despawned underneath
	// - fall out of one loop instead of needing its own handler.
	uint16_t seatVehicleNetId   = INVALID_NETID;
	uint16_t seatedVehicleNetId = INVALID_NETID;
	uint8_t  seatIndex          = 0;

	// A seated ped is positioned by CWorld::Process from the car's own
	// matrix, every frame (docs/protocol.md §1.13.2). So nothing else may
	// write its transform - exactly the rule ApplyRemotePose follows for a
	// seated player.
	bool Seated() const { return seatedVehicleNetId != INVALID_NETID; }
};

// ---- ambient traffic (docs/population.md §3 step 4) ------------------------

// A traffic car the local engine just created, on its way to the session.
struct LocalAmbientCar {
	uint32_t       tempId = 0;
	AmbientCarBody body{};
};

// Somebody else's traffic car, as this machine holds it.
//
// Shaped like RemoteAmbientPed with one thing added, and that thing is the
// whole difference between step 2 and step 4: a car is *going somewhere*. A
// pedestrian replica is created where the session says and left there, which
// looks like a person standing still. A traffic car replica left where it was
// created is a locked, undeletable car parked across a junction on every
// screen but its owner's - so it gets a transform stream and an interpolation
// buffer, the same pair a driven car has had since M2.
struct RemoteAmbientCar {
	bool     active        = false;
	uint16_t netId         = INVALID_NETID;
	uint8_t  ownerPlayerId = INVALID_PLAYER;
	AmbientCarBody body{};

	// CPools::GetVehicleRef, not a pointer. Same reason as everywhere else:
	// it stops resolving when the engine deletes the car, reused slot or not.
	int32_t  poolHandle   = -1;
	bool     spawnPending = false;

	// The session says this car is a wreck (roadmap.md 5.8). Kept on the row
	// rather than read back off the engine because it has to outlive the
	// object: CorrectAmbientCarReplica re-arms the spawn when a replica goes
	// missing from the pool, and without this a wreck the local engine has
	// just reaped would come straight back as a pristine car nobody destroyed.
	bool     destroyed    = false;

	// Where the owner says it is. `interp` is fed at whatever rate the owner
	// is streaming this particular car at - which is not a fixed rate, since
	// only the eight nearest its owner are sent (protocol.h, MAX_CAR_STATES).
	// The buffer already copes: it is keyed on the sender's own timestamps and
	// extrapolates along the velocity for a bounded window, then holds.
	VehicleInterpBuffer interp;
	// The last transform we were told, so a car nobody is streaming any more
	// is held where it was last seen instead of being left to the local
	// engine's gravity and suspension. Seeded from the spawn packet, so this
	// is true from the first frame the replica exists.
	VehicleTransform    last{};
};

// The engine seam. Every function here is optional - with none of them set,
// CoopIII runs headless. It connects, keeps an accurate roster, interpolates
// poses, but draws nothing. That's exactly the state Area B starts from, and
// it's genuinely useful to be able to test that state on its own.
struct WorldBridge {
	// Fill `out` from the local player. Returns false if there's no player
	// yet (menus, loading) - CoopIII then sends nothing this frame.
	bool (*SampleLocalPlayer)(PlayerStateBody &out) = nullptr;

	// The local player's model index right now. False when there's no
	// player ped, same "menus, loading" case as SampleLocalPlayer.
	//
	// Sampled rather than assumed, because the model a client announced at
	// join is only true until something changes it, and a remote player
	// wearing somebody else's body is obvious on screen and invisible in a
	// log.
	bool (*SampleLocalPlayerModel)(uint16_t &modelId) = nullptr;

	// Read every one of the local player's thirteen weapon slots. Fills
	// `out` with INVENTORY_SLOTS entries, one per slot, and sets `held` to
	// the eWeaponType in their hands. Returns false when there is no player
	// ped, the same "menus, loading" case as the two above.
	//
	// A slot the player does not own reads back as weapon 0 with no ammo,
	// which is a fact worth sending once: it is how a player who has just
	// been stripped of a weapon by the script stops looking armed to
	// everyone else.
	bool (*SampleLocalAmmo)(AmmoSlotBody *out, uint8_t &held) = nullptr;

	// Write a slot the player is not holding onto their remote ped. Only
	// called with ammo sync on. Separate from ApplyRemotePose because it
	// happens on change and not every frame, and because it may have to give
	// the ped a weapon it does not have - which, for a slot that is not in
	// the hand, needs no model and no streaming.
	void (*ApplyRemoteAmmo)(RemotePlayer &player, const AmmoSlotBody &slot) = nullptr;

	// Ask the streamer for a model. Called once per remote player.
	void (*RequestModel)(uint16_t modelId) = nullptr;
	bool (*IsModelReady)(uint16_t modelId) = nullptr;

	// Create/destroy the ped. Sets/clears RemotePlayer::poolHandle.
	bool (*SpawnRemote)(RemotePlayer &player) = nullptr;
	void (*DespawnRemote)(RemotePlayer &player) = nullptr;

	// Write an interpolated pose onto an already-spawned ped.
	void (*ApplyRemotePose)(RemotePlayer &player, const Pose &pose) = nullptr;

	// ---- vehicles (M2) ----------------------------------------------------
	//
	// Same shape as the ped functions above, optional in the same way: a
	// build with these left null syncs players on foot and just ignores
	// cars, which happens to be exactly where M1 left off.

	// Fill `out` from the vehicle the local player is *driving*. False when
	// on foot, or riding as a passenger - either way this machine doesn't
	// own it.
	bool (*SampleLocalVehicle)(VehicleStateBody &out) = nullptr;

	// Create/destroy an observed vehicle. Sets/clears RemoteVehicle::poolHandle.
	bool (*SpawnRemoteVehicle)(RemoteVehicle &vehicle) = nullptr;
	void (*DespawnRemoteVehicle)(RemoteVehicle &vehicle) = nullptr;

	// Write an observed state onto an already-spawned vehicle: controls,
	// health, flags. Called at the snapshot rate, before the world updates.
	void (*ApplyRemoteVehicle)(RemoteVehicle &vehicle,
	                           const VehicleStateBody &body) = nullptr;

	// The same slot in the frame, for a car nobody is sitting in the driver's
	// seat of. Puts it at rest instead of replaying the last driver.
	//
	// A car changes hands through the reliable, ordered enter/exit pair, and
	// between the two it belongs to nobody. There is no "last driver's
	// opinion" to carry on applying: the snapshot that would be replayed was
	// sampled while somebody was still driving, and it says the car is doing
	// forty. Held there, the engine's own CVehicle::CanPedEnterCar refuses
	// every attempt to get in, forever. game/vehicle.cpp has the disassembly.
	void (*RestRemoteVehicle)(RemoteVehicle &vehicle) = nullptr;

	// ---- the damage model (docs/cardamage.md) -----------------------------
	//
	// Optional in the same way as everything above it: a build with these
	// null drives cars around with converged health and per-machine dents,
	// which is exactly where the vehicle work stood before this.

	// What shape the car the local player is *driving* is in. False on foot,
	// as a passenger, or for a car that is already a wreck - a wreck's damage
	// came from FuckCarCompletely and is the same on every machine already.
	bool (*SampleLocalVehicleDamage)(VehicleDamageBody &out) = nullptr;

	// Make an observed car wear it. Never lowers anything; `flying` decides
	// whether a part that has just gone actually flies off (a live change) or
	// is simply absent (a spawn or a backfill).
	void (*ApplyRemoteVehicleDamage)(RemoteVehicle &vehicle,
	                                 const VehicleDamageBody &body,
	                                 bool flying) = nullptr;

	// Put the vehicle back where the session says it is. Called *after* the
	// world has updated, every frame.
	//
	// These two are separate because they happen at different times for
	// different reasons. A remote car is still simulated locally - that's
	// what turns its wheels, works its suspension, plays its engine note -
	// and pulling it off the physics list to stop it drifting would just
	// leave a brick sliding down the street. So it's simulated, then
	// corrected: physics runs, and the last thing before the frame draws is
	// us putting the car back where it belongs. An observer can animate
	// what it's watching; it doesn't get to decide where it ends up.
	void (*CorrectRemoteVehicle)(RemoteVehicle &vehicle,
	                             const VehicleTransform &at) = nullptr;

	// Is the local player sitting in this row's driver's seat, as far as the
	// *engine* is concerned? Asked of CVehicle::m_pDriver, not of the roster.
	//
	// The roster's answer is Client::DrivenLocally, and it is a different
	// question: it says "the session has told us this netId is ours". Between
	// pressing the enter key and that reply landing there is a window where
	// the engine says we are driving and the session has not said anything
	// yet, and in that window the old code went on writing the *previous*
	// driver's snapshot - velocity, steering, throttle, gear - onto the car
	// under us, every frame.
	//
	// One round trip is nothing. A claim that never comes back is not: the
	// claim is sent once and Client::m_vehicleClaimPending stops it being
	// sent again, so a refused or dropped claim leaves that window open for
	// the rest of the session. The car is then held at whatever the last
	// driver was doing when they stepped out - which for a car somebody
	// parked is a standstill - and the symptom is a car you can get into and
	// cannot drive. "No se mueve, o se mueve super lentísimo."
	//
	// CorrectRemoteVehicle has always asked the engine this question before
	// writing a transform (game/vehicle.cpp). Only the other half of the pair
	// never did, and this is that half.
	bool (*LocalDrivesVehicle)(const RemoteVehicle &vehicle) = nullptr;

	// Identity of the vehicle the local player is driving, for the claim
	// that introduces it to the session.
	bool (*SampleLocalVehicleIdentity)(VehicleIdentity &out) = nullptr;

	// The engine's own reference (CPools::GetVehicleRef) for the car the
	// local player is driving, or -1 on foot or as a passenger. The same
	// number RemoteVehicle::poolHandle holds, so the two can be compared.
	//
	// This exists because of a question only a late joiner ever asks: is the
	// car I have just got into one of *ours*? Everybody who was in the
	// session when a car was claimed has it as a car from their own world;
	// only somebody who joined afterwards has it as a CVehicle CoopIII
	// created, and until this callback there was no way to notice. The claim
	// therefore went out as a brand new car, the session handed back a second
	// netId for a car it already knew, and the joiner ended up both driving
	// that car and observing it - so CorrectRemoteVehicles pinned its
	// transform every frame while its own driver pressed the accelerator.
	// You could climb in and it would not move. See Client::SendLocalVehicle.
	//
	// A bare sample rather than a "is this that car?" predicate on purpose:
	// the comparison is the decision, and the decision belongs in client.cpp
	// where tools/clienttest can reach it with a stub instead of an engine.
	int32_t (*SampleLocalVehicleHandle)() = nullptr;

	// ---- riding in somebody else's car -----------------------------------
	//
	// GTA III has no passenger seat for the player: the enter key jacks the
	// driver, because in single player nobody is driving a car you would want
	// to ride in. This is the one control CoopIII adds that the original game
	// does not have. game/seat.h has the whole of it.

	// True once per press of the seat key, never while it is held.
	bool (*LocalWantsSeatToggle)() = nullptr;

	// Start getting the local player into the first free passenger seat of
	// the car this pool ref names. The engine walks them to the door and
	// opens it, so the answer is usually SEAT_LOCAL_WALKING and the
	// seat number arrives from PollLocalSeatEntry a second or two later.
	// SEAT_LOCAL_REFUSED for no, and the client log says why.
	int32_t (*SeatLocalPlayerIn)(int32_t vehicleHandle) = nullptr;

	// Drive an entry SeatLocalPlayerIn started. A seat number means the ped
	// is seated as of this frame and the session can be told; WALKING means
	// ask again; REFUSED means it is over, one way or another. Safe to call
	// every frame with nothing in flight.
	int32_t (*PollLocalSeatEntry)() = nullptr;

	// In a car, and not driving it. How the exit is noticed: there is no
	// CoopIII way out, the player uses the game's own exit key and this goes
	// false.
	bool (*LocalIsPassenger)() = nullptr;

	// Get out. The same key that got us in.
	bool (*UnseatLocalPlayer)() = nullptr;

	// Put a remote ped in a seat, and take them out again.
	//
	// Seat returns false when it couldn't be done *yet* - the ped or the
	// car is still streaming in - and Client just tries again next frame.
	// It must never report a seating it didn't actually perform: the return
	// value is what stops the pose stream from writing over a ped the
	// engine now owns.
	bool (*SeatRemotePed)(RemotePlayer &player, RemoteVehicle &vehicle,
	                      uint8_t seat) = nullptr;
	// Takes whatever car the *ped* says it's in, not one pulled from the
	// roster, so it still does the right thing for a car the session has
	// already forgotten about.
	void (*UnseatRemotePed)(RemotePlayer &player) = nullptr;

	// ---- and the same two with the door open ------------------------------
	//
	// The four above put a ped in a seat, or take it out, in one call. These
	// are the engine's own entry and exit, which play the animation and
	// therefore take about a second - so they are an *attempt*, polled, with
	// the pair above standing behind them as the answer that always works.
	//
	// Nothing here is allowed to be the only way a player reaches a seat.
	// Every one of them can refuse, and the engine can abandon what it
	// started without telling anybody, so Client drives them on a deadline
	// and falls back to the warp. See UpdateRemoteSeats.

	// Ask the engine to walk this ped to a door and get in. False means it
	// would not start - the ped is dying, the car is driving off, the door
	// is already being used, a model is still streaming - and the caller
	// should just seat them.
	bool (*BeginSeatRemotePed)(RemotePlayer &player, RemoteVehicle &vehicle,
	                           uint8_t seat) = nullptr;

	// How an entry begun by the above is getting on.
	uint8_t (*PollSeatRemotePed)(RemotePlayer &player, RemoteVehicle &vehicle,
	                             uint8_t seat) = nullptr;

	// Take an unfinished entry off the ped, leaving nothing behind on the
	// car. Safe on a ped that is not entering anything, which is what lets
	// it double as "make sure this ped is not halfway into something".
	//
	// This one is not optional and it is not tidiness: an entry dropped
	// without it leaves the car's m_nGettingInFlags holding that door for
	// the rest of its life, and the door is then refused to everybody.
	void (*AbandonSeatRemotePed)(RemotePlayer &player) = nullptr;

	// Ask the engine to open the door and climb out. False means it refused,
	// and the caller should take them out the plain way.
	bool (*BeginUnseatRemotePed)(RemotePlayer &player) = nullptr;

	// Hands over the blasts the local player's own car suffered since the
	// last call, oldest first, and returns how many got written.
	//
	// By detour, not by sampling, for the same reason a shot is: a car
	// exploding is an event, and m_fHealth - which *is* sampled - is only a
	// number. Nothing in the engine watches health for zero, so an observer
	// handed a zero gets an undamaged-looking car with no health rather than
	// a wreck. docs/protocol.md §1.11.
	uint8_t (*DrainLocalVehicleBlasts)(LocalVehicleBlast *out,
	                                   uint8_t max) = nullptr;

	// Replay somebody else's car blowing up, through the engine's own
	// CAutomobile::BlowUpCar, at the transform they say it ended up at. One
	// call because one call is what the engine does: the blast and the burnt
	// shell are decided in the same function, so replaying it gets both, in
	// the same place, without CoopIII inventing either.
	bool (*BlowUpRemoteVehicle)(RemoteVehicle &vehicle, const Vec3 &pos,
	                            const Quat &rot) = nullptr;

	// ---- cars nobody owns (docs/roadmap.md 5.8) ---------------------------

	// Hands over the unowned cars this machine's own engine destroyed since
	// the last call - the parked ones, which C_VehicleBlowUp cannot carry
	// because it is sent by a driver and a parked car has none.
	//
	// The transform half of each UnownedBlast is left at zero here and is
	// never read on the other side: a parked car is where the map put it, on
	// every machine, so sending one would be sending a machine its own copy
	// of shared map data. The traffic half of 5.8 does fill it, for the
	// reason BlastTransform gives, and this is why the field is on the shared
	// struct rather than on that queue alone - one shape, two queues, and the
	// kind byte already says which of them a report came from.
	//
	// No health either: writing health destroys nothing
	// and a low one arms the engine's five-second fire timer on the receiver,
	// which is an observer deciding to destroy somebody else's car later.
	uint8_t (*DrainUnownedBlasts)(UnownedBlast *out, uint8_t max) = nullptr;

	// Make the unowned car this key names a wreck on this machine, through
	// the same CAutomobile::BlowUpCar the reporter's engine went through.
	// See UnownedWreckOutcome for why the answer is not a bool.
	UnownedWreckOutcome (*WreckUnownedVehicle)(const UnownedVehicleKey &key) =
	    nullptr;

	// ---- combat (M3) ------------------------------------------------------

	// Hands over whatever the local player fired or blew up since the last
	// call, oldest first, and returns how many got written into `out`.
	//
	// A drain, not a sample - these are events, and the engine is the only
	// thing that knows when one happened. Called every frame, not at the
	// snapshot rate; a shot that waited for the next 25 Hz tick would arrive
	// after the one that followed it.
	uint8_t (*DrainLocalCombat)(CombatEvent *out, uint8_t max) = nullptr;

	// Replay somebody else's shot on their ped, and play somebody else's
	// explosion. Both get dropped rather than queued when the ped isn't
	// there yet - a muzzle flash only matters at the instant it happened,
	// and an explosion has its own position and doesn't need a ped anyway.
	void (*ReplayRemoteShot)(RemotePlayer &player, const ShotBody &shot) = nullptr;
	void (*PlayRemoteExplosion)(RemotePlayer &player,
	                            const ExplosionBody &body) = nullptr;

	// Hurt the *local* player, with somebody else's hit, through the engine's
	// own CPed::InflictDamage. `attacker` is who gets the blame and may be
	// null when their ped hasn't streamed in; the damage lands either way.
	//
	// This is the only place in CoopIII where a packet reduces anyone's
	// health, and it is deliberately the one player this machine owns.
	void (*ApplyRemoteDamage)(RemotePlayer *attacker, const DamageBody &body) = nullptr;

	// Kill a remote player's ped, with the animation their own engine chose.
	// One way: the ped is a corpse afterwards and the way back is a new ped,
	// which is what RespawnRemote is for.
	void (*KillRemotePed)(RemotePlayer &player, uint16_t animId) = nullptr;

	// Whether this session allows players to hurt each other
	// (docs/roadmap.md §5.2). The server enforces it by refusing to relay a
	// C_Damage, so this only covers the one kind of damage that never goes
	// near the server: an explosion every machine replays for itself.
	void (*SetFriendlyFire)(bool enabled) = nullptr;

	// Whether this session reports ammunition honestly (protocol.h,
	// SESSION_AMMO_SYNC). The engine seam needs it in two places the
	// packets never reach: what CPed::GiveWeapon hands a freshly spawned
	// remote ped, and what CWeapon::Fire is allowed to leave behind in a
	// replayed shot.
	void (*SetAmmoSync)(bool enabled) = nullptr;

	// ---- the wanted level (docs/wanted.md) --------------------------------
	//
	// Two calls, and between them they are the whole engine surface of the
	// feature. Nothing here spawns a policeman, suppresses one, or tells one
	// who to chase: a cop ped is a RANDOM_CHAR and a police car a
	// RANDOM_VEHICLE, so game/population.cpp already replicates both, and
	// CCopPed can only ever pursue FindPlayerPed(). The wanted player's own
	// engine does all of it from this one number.

	// The local player's stars right now, 0..6. False when there is no player
	// ped - menus, loading, between a death and a respawn - and Client then
	// leaves the whole thing alone for that tick rather than assuming zero,
	// because assuming zero is indistinguishable from a player who has just
	// been busted.
	bool (*SampleLocalWantedLevel)(uint8_t &level) = nullptr;

	// Set them, through the engine's own CPlayerPed::SetWantedLevel.
	//
	// Called only when Client disagrees with what it just sampled, which in
	// the default rule with nobody in your car is never. That is not an
	// optimisation: the engine's setter resets m_nChaos to the bottom of the
	// bracket, so calling it every tick with the level the player already has
	// would throw their accumulated chaos away and stop them ever climbing.
	void (*WriteLocalWantedLevel)(uint8_t level) = nullptr;

	// ---- time of day and weather ------------------------------------------
	//
	// Read on the host, to tell the session what time it is. Read on everyone
	// else too, to work out how far off they are before deciding whether
	// moving the clock is worth the jump it costs.
	//
	// False when there's no world to read: the menu, a loading screen.
	bool (*SampleWorld)(WorldState &out) = nullptr;

	// Put the session's time of day on this machine. Only called once the
	// drift is past kClockToleranceMinutes, so it's allowed to be a jump.
	void (*ApplyWorldTime)(uint8_t hour, uint8_t minute) = nullptr;

	// Pin this machine's sky to the pair the host is blending between, and
	// stop the local weather rotation picking its own next one.
	void (*ApplyWorldWeather)(uint8_t weather, uint8_t weatherOld) = nullptr;

	// Give the sky back to the engine. Called when this machine becomes the
	// host, because a pinned sky never changes again and the session would
	// otherwise inherit whatever the last host was looking at, forever.
	void (*ReleaseWorldWeather)() = nullptr;

	// ---- ambient population (docs/population.md §3 step 2) ----------------
	//
	// Filled by game/population.cpp, which hooks CWorld::Add - the one door
	// every entity walks through on its way into the world, so the ped
	// generator, a script spawn and anything else register the same way with
	// no list of creation paths to keep up to date (§1.1).

	// Hands over the ambient peds the local engine created since the last
	// call, oldest first, and returns how many got written.
	//
	// A drain rather than a sample for the same reason a shot is: creation
	// is an event, and only the engine knows when one happened.
	uint32_t (*DrainLocalAmbientPeds)(LocalAmbientPed *out, uint32_t max) = nullptr;

	// Hands over the netIds of locally hosted peds the engine has taken
	// away. Only peds that had already been named - one that dies before its
	// name comes back is dealt with by NameLocalAmbientPed returning false.
	uint32_t (*DrainLostAmbientPeds)(uint16_t *out, uint32_t max) = nullptr;

	// Tell the engine side what the session decided to call a ped it
	// announced under `tempId`.
	//
	// Returns false when that ped is already gone, which is not an error and
	// is the normal race: CPopulation reaps pedestrians constantly, and the
	// round trip to the server is several frames long. The caller answers a
	// false by telling the session the ped is gone.
	bool (*NameLocalAmbientPed)(uint32_t tempId, uint16_t netId) = nullptr;

	// Create/destroy a replica of somebody else's ambient ped. Sets and
	// clears RemoteAmbientPed::poolHandle.
	//
	// A replica is written to and never decides anything (§1.1). Making that
	// true is the remote-ped AI suppression work, which is a different seam
	// and a prerequisite for this one looking right - without it, every
	// machine's copy of a pedestrian wanders off in its own direction.
	bool (*SpawnAmbientReplica)(RemoteAmbientPed &ped) = nullptr;
	void (*DespawnAmbientReplica)(RemoteAmbientPed &ped) = nullptr;

	// Is the replica this row names still the object this row made?
	//
	// The engine owns the ped pool and is free to take a replica away -
	// CWorld::ClearExcitingStuffFromArea on a respawn, a car blowing up under
	// its occupants, anything that flags bRemoveFromWorld. When it does, this
	// clears `poolHandle` and sets `spawnPending` so the spawn pass builds a
	// new one, the same recovery CorrectAmbientCarReplica performs for a car.
	// Without it the row keeps a handle that will never resolve again and the
	// pedestrian is gone from this machine for the rest of the session.
	//
	// Called on every live replica on every frame, before anything else looks
	// at the row. A false is not an error.
	bool (*AmbientReplicaIsAlive)(RemoteAmbientPed &ped) = nullptr;

	// ---- the ped stream (docs/population.md §3 step 6) --------------------

	// Fills `out` with up to `max` hosted pedestrians and returns how many.
	// Seated ones first - they are the traffic drivers and there is at most
	// one per hosted car - then nearest the local player, which is §2.1's
	// rate-by-distance in the same crude form the car stream uses it.
	uint32_t (*SampleHostedPeds)(AmbientPedState *out, uint32_t max) = nullptr;

	// Puts a replica where the session says it is and plays what the session
	// says it is playing.
	//
	// Called from PreFrame, not PostFrame, and that is a real difference
	// from CorrectAmbientCarReplica rather than an oversight. A car replica
	// is simulated by the local engine on purpose - that is what turns its
	// wheels and works its suspension - so its transform has to be written
	// back *after* the physics that just moved it. A ped replica is driven
	// exactly like a remote player, whose pose has been written from
	// PreFrame since Area B and which renders correctly: CPed::ProcessControl
	// moves a ped by its animation's own translation, which is centimetres a
	// frame, not by a suspension that walks a parked car down a hill.
	void (*ApplyAmbientPedState)(RemoteAmbientPed &ped, const Pose &at) = nullptr;

	// Seat a ped replica in a traffic replica, and take it back out. The
	// engine work is the same CPed::SetObjective + CPed::WarpPedIntoCar pair
	// the player seating uses, reached through game/ped.cpp rather than
	// written a third time.
	bool (*SeatAmbientPed)(RemoteAmbientPed &ped, RemoteAmbientCar &car,
	                       uint8_t seat) = nullptr;
	void (*UnseatAmbientPed)(RemoteAmbientPed &ped) = nullptr;

	// ---- limbs (protocol version 17) ---------------------------------------

	// The limbs this machine's engine has taken off its own hosted
	// pedestrians since the last call, oldest first. Returns how many.
	uint32_t (*DrainAmbientBodyParts)(PedBodyPartBody *out, uint32_t max) = nullptr;

	// Runs CPed::RemoveBodyPart on a replica, because its host's engine did.
	// False if the replica is not there to take it.
	bool (*RemoveAmbientBodyPart)(RemoteAmbientPed &ped, uint8_t node,
	                              int8_t direction) = nullptr;

	// ---- deaths ------------------------------------------------------------

	// The hosted pedestrians this machine's engine has killed since the last
	// call, oldest first, with the animation it chose for each. Returns how
	// many. A drain rather than a sample for the same reason a limb is: only
	// the engine knows the moment, and the only witness is the CPed::SetDie
	// detour.
	uint32_t (*DrainAmbientPedDeaths)(PedDeathBody *out, uint32_t max) = nullptr;

	// Runs CPed::SetDie on a replica, because its host's engine did - the
	// engine deciding the death rather than CoopIII inventing one, the same
	// rule BlowUpRemoteVehicle follows for a car.
	//
	// False when the replica is not there to take it, which the caller reads
	// as "not yet" and retries: the death is a standing fact about the ped
	// and it outlives any particular replica of him.
	bool (*KillAmbientReplica)(RemoteAmbientPed &ped, uint16_t animId) = nullptr;

	// ---- ambient traffic (docs/population.md §3 step 4) -------------------
	//
	// The same five, for cars, plus a sixth a ped does not need: somebody has
	// to read the hosted cars back out of the engine every tick, because they
	// are driving away from where they were announced.

	uint32_t (*DrainLocalAmbientCars)(LocalAmbientCar *out, uint32_t max) = nullptr;
	uint32_t (*DrainLostAmbientCars)(uint16_t *out, uint32_t max) = nullptr;
	bool (*NameLocalAmbientCar)(uint32_t tempId, uint16_t netId) = nullptr;
	bool (*SpawnAmbientCarReplica)(RemoteAmbientCar &car) = nullptr;
	void (*DespawnAmbientCarReplica)(RemoteAmbientCar &car) = nullptr;

	// Fills `out` with up to `max` hosted cars, nearest the local player
	// first, and returns how many. Nearest-first is the rate-by-distance rule
	// (docs/population.md §2.1) in its simplest possible form: there is one
	// slot budget per tick and the cars somebody is about to drive into get
	// it.
	uint32_t (*SampleHostedCars)(AmbientCarState *out, uint32_t max) = nullptr;

	// Puts a replica back where the session says it is, after the local
	// frame's physics. Exactly CorrectRemoteVehicle's job and for exactly its
	// reason - written before CGame::Process it would only be what the local
	// physics starts from.
	void (*CorrectAmbientCarReplica)(RemoteAmbientCar &car,
	                                 const VehicleTransform &at) = nullptr;

	// ---- a hosted traffic car that was destroyed (roadmap.md 5.8) ---------
	//
	// The ambient half of "a car nobody is driving has nobody to report it",
	// and a separate pair from DrainUnownedBlasts / WreckUnownedVehicle even
	// though both carry an UnownedVehicleKey. Those two resolve a name the
	// *map* hands out, in game/vehicle.cpp; these two resolve a netId the
	// session hands out, against the ambient roster, in game/population.cpp.
	// One pointer for both would tie the two files' installs together - and
	// this codebase has already been bitten by a half-installed bridge
	// silently dropping one arm of a feature.
	//
	// Only the machine hosting a car ever fills this queue. A replica's own
	// wreck is a local opinion about somebody else's car, and
	// game/population.cpp refuses it by never letting a replica into the
	// hosted roster in the first place.
	uint8_t (*DrainAmbientWrecks)(UnownedBlast *out, uint8_t max) = nullptr;

	// Make a replica a wreck, through the same BlowUpCar its host went
	// through. See UnownedWreckOutcome for why the answer is not a bool: the
	// replica is routinely still streaming in when the report arrives.
	UnownedWreckOutcome (*WreckAmbientCarReplica)(RemoteAmbientCar &car,
	                                              const BlastTransform &where) =
	    nullptr;
	// ---- pickups (M4) ------------------------------------------------------
	//
	// The outbound half does not live here. game/pickup.cpp calls straight
	// into Client through its own PickupCallbacks, because a claim is made
	// from inside the CPickups::Update detour rather than once a frame - the
	// whole point is that it goes out *before* the engine gets a chance to
	// award anything.
	//
	// docs/pickups.md is the design.

	// The server has reserved this one for us. Unblocks it so the engine's
	// own award path *may* run on it - whether it does is the engine's
	// decision, read back afterwards. False when there is nothing there to
	// take any more, in which case the seam has already released it.
	bool (*PickupGrantedToUs)(const PickupIdent &ident) = nullptr;

	// Somebody else got it. Removes our copy the way the engine would have,
	// and tells this machine's own script it happened.
	void (*PickupTakenByOther)(const PickupIdent &ident) = nullptr;

	// Our claim lost, or the pickup is not back yet.
	void (*PickupDenied)(const PickupIdent &ident) = nullptr;

	// No session any more: every pickup goes back to being the local engine's
	// own business, which is single player behaving exactly as it always did.
	void (*PickupsReset)() = nullptr;

	// Somebody else's pedestrian dropped money or a gun. Builds the same
	// pickup here through CPickups::GenerateNewOne with the numbers their
	// engine actually used, after which it is an ordinary pickup and the four
	// entries above own it. docs/pickups.md 10.
	void (*PickupDropped)(const PickupDropBody &drop) = nullptr;

	// ---- garages, doors and the Pay'n'Spray (M4) --------------------------
	//
	// game/garage.h is the design; the short version is that one bit per
	// garage travels - "my own state machine has this garage away from where
	// this type of garage rests" - and the union of everybody's bits is what
	// each machine holds its own doors to. The door's height never travels:
	// every machine derives it from the state, at its own frame rate,
	// through the engine's own ramp.

	// This machine's own mask, one bit per garage, bit i = aGarages[i].
	// False in the frontend and on a loading screen, where CGarages::Update
	// is not called and the globals hold whatever the last session left.
	bool (*SampleLocalGarages)(uint32_t &mask) = nullptr;

	// The union of what everybody else reports. Written every frame rather
	// than on change: it costs one store, and a bridge that is only written
	// on change has to be got right on reconnect as well.
	void (*ApplyRemoteGarages)(uint32_t mask) = nullptr;

	// Hands over the Pay'n'Spray visits this machine's own engine completed
	// since the last call. A drain, not a sample, for the same reason a shot
	// is one: it is an event, and only the engine knows when one happened.
	uint8_t (*DrainLocalResprays)(LocalRespray *out, uint8_t max) = nullptr;

	// Repair and repaint somebody else's car, with the colours their engine
	// chose. `vehicle` is null when the session has no row for the car that
	// was sprayed - a player can drive an unclaimed traffic car into a spray
	// shop - and then there is nothing here to do but the doors, which the
	// mask has already done.
	void (*ApplyRemoteRespray)(RemoteVehicle *vehicle,
	                           const ResprayBody &body) = nullptr;
	// ---- breakable street objects -----------------------------------------
	//
	// docs/objects.md. Somebody drove into a lamp post, a meter or a crate
	// somewhere else. Replays the engine's own CObject::ObjectDamage on our
	// copy of the same map object, and quietly does nothing when our copy is
	// still a dummy because nobody here is near it.
	void (*ObjectBroken)(const ObjectBreakBody &body) = nullptr;
};

class Client {
public:
	bool Start(const std::string &host, uint16_t port, const std::string &nick,
	           const WorldBridge &bridge);
	void Stop();

	// Game thread, once per frame, around CGame::Process.
	void PreFrame();
	void PostFrame();

	bool     IsConnected() const { return m_net.IsConnected(); }
	// Taken from S_Welcome on the game thread, so the roster logic has one
	// source of truth and doesn't depend on the socket thread having run.
	uint8_t  LocalPlayerId() const { return m_localPlayerId; }
	uint32_t RoundTripMs() const { return m_net.RoundTripMs(); }

	// Whether this machine's own game is the one everyone else's clock and
	// sky follow. INVALID_PLAYER on either side means no, so this is false
	// before the first welcome and after a disconnect.
	uint8_t HostPlayerId() const { return m_hostPlayerId; }
	bool    IsHost() const {
		return m_localPlayerId != INVALID_PLAYER && m_localPlayerId == m_hostPlayerId;
	}

	const RemotePlayer &PlayerSlot(uint8_t id) const { return m_players[id]; }
	uint8_t             RemoteCount() const;

	// How long a door-opening entry is given before the seat is taken by
	// force. Settable so the timeout can be reached in a test without a test
	// that takes two and a half seconds to run - the branch it guards is the
	// one that decides whether a remote player can be left standing half
	// inside a car, so it is worth being able to reach.
	void SetSeatAnimTimeoutMs(uint32_t ms) { m_seatAnimTimeoutMs = ms; }

	// Vehicles are keyed by the server's netId rather than by slot, so this
	// looks one up. Returns null if we've never been told about it.
	const RemoteVehicle *VehicleByNetId(uint16_t netId) const;

	// The car the local player is driving, as the session names it, or
	// INVALID_NETID on foot.
	uint16_t LocalVehicleNetId() const { return m_localVehicleNetId; }

	// The union of what every *other* player says their garages are doing,
	// one bit per garage. This is the whole authority decision for doors and
	// it is one OR, so tools/clienttest reads it directly rather than
	// inferring it from what the bridge was handed.
	uint32_t RemoteGarageMask() const;
	uint8_t              VehicleCount() const;

	// Test seam: drive the roster and the frame pump without a socket.
	void SetBridge(const WorldBridge &bridge) { m_bridge = bridge; }
	void HandleMessage(const Message &msg);
	// One frame of the roster: what PreFrame does, then what PostFrame does
	// minus the sending. The correction is part of a frame, not part of a
	// send - that's the distinction the vehicle sync hinges on.
	void Tick() {
		UpdateRemotes();
		UpdateRemoteVehicles();
		UpdateUnownedWrecks();
		UpdateRemoteAmbientPeds();
		UpdateRemoteAmbientCars();
		UpdateRemoteSeats();
		UpdateAmbientPedSeats();
		ApplyAmbientPedPoses();
		UpdateGarages();
		TickWanted();
		CorrectRemoteVehicles();
		CorrectAmbientCars();
	}

	// Test seam: the ambient rosters, so the stream and the driver link can
	// be checked without a game. Null when the session has never mentioned
	// that netId.
	const RemoteAmbientPed *AmbientPed(uint16_t netId) const {
		return const_cast<Client *>(this)->AmbientPedByNetId(netId);
	}
	const RemoteAmbientCar *AmbientCar(uint16_t netId) const {
		return const_cast<Client *>(this)->AmbientCarByNetId(netId);
	}
	// Test seam: the outgoing half of a frame, which PostFrame keeps behind
	// a live socket. Send is a no-op while disconnected, so what this
	// exercises is the decision - claim a new car, take over one the session
	// already has, or report the one we are in - and the decision is where
	// the bug was.
	void TickLocalVehicle() { SendLocalVehicle(); }

	// Test seam: what a dropped connection does to the roster. PreFrame
	// reaches it only through a live socket that has just died.
	void ClearRosterForTest() { ClearRoster(); }

	// Called from game/pickup.cpp, on the game thread, from inside the
	// CPickups::Update detour. Not queued: a claim is only useful if it
	// leaves before the engine has a chance to award the pickup, and the
	// detour runs on the same thread everything else here does.
	void ClaimPickup(const PickupIdent &ident);
	void ReleasePickup(const PickupIdent &ident);
	void CollectedPickup(const PickupIdent &ident);

	// A pedestrian this machine hosts just left this behind. Same path and
	// same reason as the three above: it is sent from inside CPed::SetDead,
	// on the game thread, not queued for the next tick.
	//
	// Returns whether it went out, so the seam can tell "shared" from "single
	// player" in its own log rather than claiming the first and meaning the
	// second.
	bool DroppedPickup(const PickupDropBody &drop);

	// Is this CPed one CoopIII built - a remote player's, or an ambient
	// replica? Asked by game/pickup.cpp from inside the drop detours, which
	// have nothing but a pointer and no way to tell a replica from a script
	// ped without the roster.
	//
	// Walks both rosters rather than keeping a set: MAX_PLAYERS is 8 and
	// MAX_REMOTE_PEDS is a few hundred, this runs once per ped death, and a
	// second index of pool refs is a second thing to keep in step with the
	// spawn and despawn paths.
	bool IsReplicatedPed(int32_t pedRef) const;

	// The same question for a car, and it is asked in one place:
	// game/object.cpp, from inside the CObject::ObjectDamage detour, about
	// whatever CPhysical recorded as having hit the object. A replica's
	// collision here is a copy of a collision that really happened somewhere
	// else, so the machine that owns the car is the one that reports what it
	// knocked down.
	bool IsReplicatedVehicle(int32_t vehRef) const;

	// A map object broke on this machine and this machine is the one entitled
	// to say so. Same path and same reason as the pickup senders: called on
	// the game thread from inside a detour, not queued.
	//
	// Returns whether it went out, so the seam can count "shared" separately
	// from "single player" instead of claiming one and meaning the other.
	bool ReportObjectBroken(const ObjectBreakBody &body);

private:
	void OnWelcome(const S_Welcome &pkt);
	void OnJoin(const S_PlayerJoin &pkt);
	void OnLeave(const S_PlayerLeave &pkt);
	void OnPlayerModel(const S_PlayerModel &pkt);
	void OnPlayerAmmo(const S_PlayerAmmo &pkt);
	void OnPlayerState(const S_PlayerState &pkt);
	void OnVehicleSpawn(const S_VehicleSpawn &pkt);
	void OnVehicleDespawn(const S_VehicleDespawn &pkt);
	void OnVehicleState(const S_VehicleState &pkt);
	void OnVehicleBlowUp(const S_VehicleBlowUp &pkt);
	void OnVehicleDamage(const S_VehicleDamage &pkt);
	// A car nobody owns was destroyed somewhere. Records a standing
	// instruction; UpdateUnownedWrecks is what carries it out, because the
	// car may be three streets away and not streamed in yet.
	void OnUnownedBlowUp(const S_UnownedBlowUp &pkt);
	void OnEnterVehicle(const S_EnterVehicle &pkt);
	void OnExitVehicle(const S_ExitVehicle &pkt);
	void OnShot(const S_Shot &pkt);
	void OnExplosion(const S_Explosion &pkt);
	void OnDamage(const S_Damage &pkt);
	void OnDeath(const S_Death &pkt);
	void OnRespawn(const S_Respawn &pkt);
	void OnWorldState(const S_WorldState &pkt);
	void OnPickupTaken(const S_PickupTaken &pkt);
	void OnPickupDenied(const S_PickupDenied &pkt);
	void OnPickupGrant(const S_PickupGrant &pkt);
	void OnPickupDrop(const S_PickupDrop &pkt);
	// Somebody else's garages. A level, not an event: the last mask received
	// from a player is what that player currently says, and it replaces
	// whatever they said before.
	void OnGarageState(const S_GarageState &pkt);
	void OnRespray(const S_Respray &pkt);
	void OnObjectBroken(const S_ObjectBroken &pkt);

	// The one place m_hostPlayerId changes, so that becoming or stopping
	// being the host is a single event with one handler instead of something
	// two packet paths each have to remember.
	void SetHost(uint8_t hostPlayerId);
	// Move the clock if it's drifted far enough to be worth the jump, and
	// mirror the sky. Does nothing on the host: the host is what this is a
	// copy of.
	void ApplyWorldState(const WorldStateBody &body);

	// Finds an existing slot, or claims a free one. Null when the table's
	// full, which isn't fatal - the vehicle just isn't shown, and the next
	// despawn frees a slot.
	RemoteVehicle *VehicleSlot(uint16_t netId, bool createIfMissing);

	// Is this a car the local player is driving rather than watching?
	//
	// A row in m_vehicles is normally somebody else's car, but it does not
	// have to stay that way: a car CoopIII spawned from the session backfill
	// is a real car in the street and anyone can get into it. The moment the
	// local player does, this machine owns it - it samples it, it sends its
	// snapshots, and it must stop writing anybody else's idea of where it is
	// on top of its own physics.
	bool DrivenLocally(const RemoteVehicle &vehicle) const;

	// Give the car we just claimed a row in the roster, so that getting out
	// of it does not make this machine forget it exists.
	//
	// The server broadcasts S_VehicleSpawn for a freshly claimed car to
	// everyone *except* the claimer (server.h, OnEnterVehicle) - correctly,
	// since the claimer already has the car; it is one of its own engine's.
	// The consequence was that the claimer was the one machine in the session
	// with no record of that car at all. Step out, walk round, get back in,
	// and ObservedVehicleWeAreDriving finds nothing, so the claim path
	// registers the same physical CVehicle under a second netId - and every
	// other machine then holds two cars for it: one that follows the driver
	// and one, frozen, that nobody drives. That is the 2026-09-22 session's
	// 364 and 475, same model 104, same extras 3/-1, no despawn in between.
	//
	// The row is marked `ours`, which is the whole difference between it and
	// an observed car: it is never spawned (the car is already there) and
	// never despawned (it is not ours to delete).
	void AdoptOurClaimedVehicle(const S_EnterVehicle &pkt);

	// The observed car the local player has just got into, if it is one of
	// ours at all. Null when they are on foot, when they are in a car from
	// their own world, or when the engine seam cannot tell us.
	RemoteVehicle *ObservedVehicleWeAreDriving();

	// Takes a player out of whatever seat we have them in, if any. Used
	// wherever either half of the pair is about to stop existing.
	void UnseatPlayer(RemotePlayer &player);

	void ClearRoster();
	void UpdateRemotes();
	void UpdateRemoteVehicles();
	// Reconciles seatVehicleNetId against seatedVehicleNetId. Runs after
	// both spawn passes, so a ped and a car that appear on the same frame
	// get seated that frame instead of the next one.
	void UpdateRemoteSeats();
	void CorrectRemoteVehicles();
	void SendLocalState();
	void SendLocalVehicle();
	// One packet per new dent, and nothing at all in between. Runs at the
	// snapshot rate because that is when the car is sampled anyway, not
	// because the field belongs in a snapshot - docs/cardamage.md §3.4.
	void SendLocalVehicleDamage();
	// Gives a car the damage its row is holding, once, after it spawns.
	void UpdateRemoteVehicleDamage();
	void TickPassengerSeat();

	// Tell the session we are sitting in somebody else's car. One place, so
	// the animated entry and the warp fallback cannot drift apart.
	void AnnounceLocalSeat(uint16_t netId, uint8_t seat);

	// ---- the wanted level (docs/wanted.md) --------------------------------

	// The netId of the car the local player is in, driving or riding, or
	// INVALID_NETID on foot. The two are tracked separately on purpose
	// (m_localVehicleNetId means "its physics are ours to report") and the
	// vehicle rule does not care which it is, because §1 found no source that
	// distinguishes a driver from a passenger and the engine offers nothing
	// to hang a distinction on.
	uint16_t LocalCarNetId() const;

	// The highest level any other player is entitled to lend us under the
	// session's rule. game/wanted.h, WantedPeerRaisesFloor, is the decision;
	// this is the fold over the roster.
	uint8_t WantedFloor() const;

	// Reconcile the engine's stars against the session's.
	//
	// Runs at the end of PreFrame, and both halves of that are deliberate:
	// at the end, because the floor is a question about the roster and the
	// roster has just been drained; in PreFrame, because CGame::Process is
	// where the two police generators read the level, so writing it here
	// takes effect this frame rather than next.
	void TickWanted();
	// One packet per car per life, off the BlowUpCar detour's queue.
	void SendLocalVehicleBlasts();
	// The same queue's other half: cars nobody owns, which the driver path
	// cannot carry because they have no driver. docs/roadmap.md 5.8.
	void SendUnownedBlasts();
	// Drives the pending wrecks toward being actual wrecks, one attempt per
	// entry per frame, and retires an entry that has landed or gone stale.
	void UpdateUnownedWrecks();
	// The UNOWNED_SESSION arm of the loop above: a car the session has a row
	// for, parked, with nobody driving it. Here rather than behind the bridge
	// because the key is a netId, and translating a netId into an engine
	// object is the roster's job.
	UnownedWreckOutcome WreckSessionVehicle(uint16_t netId);
	// The same, for a traffic car somebody else's engine made and destroyed.
	// Its netId is the session's name for a replica on the ambient roster,
	// which is why this is here and not behind the bridge's key resolver.
	UnownedWreckOutcome WreckAmbientCar(uint16_t netId, const BlastTransform &where);
	// Every frame, not at the snapshot rate, and not rate-limited - these
	// are discrete events on the reliable channel and there are only ever a
	// handful per second.
	void SendLocalCombat();
	// Announce our own death or our own respawn, from the health we just
	// sampled. Called with the body SendLocalState already has, so the ped
	// only gets read once per tick.
	//
	// Health is the whole state machine. CGameLogic sets it to 0 the moment
	// the player dies and back to 100 at the hospital, and nothing in
	// between ever leaves it at zero on a living player - the in-vehicle arm
	// of CPed::InflictDamage writes 1.0f rather than 0.0f precisely so that
	// stays true. Reading the ped state instead would drag addresses.h into
	// this file for no gain.
	void UpdateLocalLife(const PlayerStateBody &body);
	// Puts one C_Death on the wire, whichever of the two noticed first.
	// `animId` is ANIM_NONE when it was the health poll rather than the
	// engine's own SetDie.
	void AnnounceDeath(uint16_t animId);
	// One reliable packet on change, not two bytes riding every snapshot: a
	// model index only changes a handful of times in a playthrough at most.
	void SendLocalModel();
	// The same shape, for the twelve weapon slots that are not in the
	// player's hands. The one that is rides the snapshot, because it changes
	// every time a trigger is pulled and a reliable-ordered packet per
	// bullet would sit in front of the shots and the damage on channel 1.
	void SendLocalAmmo();
	// Only the host sends this, and only once a second. Below the snapshot
	// rate because a game minute is a real second, so 25 Hz would be 25
	// identical packets for every one that said anything new.
	void SendLocalWorld();

	// ---- garages (game/garage.h) ------------------------------------------

	// Push the union of everybody else's masks into the engine seam, and send
	// our own if it has changed since the last time we said anything.
	//
	// One function for both halves because they are one loop over one array
	// and because the outbound half has to run after the inbound one: the
	// engine seam samples this machine's own opinion from inside
	// CGarage::Update, so the mask read here is the one the frame just
	// produced.
	void UpdateGarages();

	// Announce the Pay'n'Spray visits our own engine completed. Above the
	// send-rate limiter with the rest of the reliable events: a respray is a
	// thing that happened once, not a stream.
	void SendLocalResprays();

	// ---- ambient population (docs/population.md §3 step 2) -----------------

	// Announce the peds the local engine made this frame, and the ones it
	// took away. Every frame rather than at the snapshot rate: these are
	// events on the reliable channel, and a ped born and reaped between two
	// 25 Hz ticks would otherwise be announced after it no longer existed.
	void SendLocalAmbientPeds();
	// Drive replicas toward what the session says, the same shape as
	// UpdateRemotes: ask for the model, then create the ped once it is there.
	void UpdateRemoteAmbientPeds();
	void OnPedSpawn(const S_PedSpawn &pkt);
	void OnPedDespawn(const S_PedDespawn &pkt);
	void OnPedBodyPart(const S_PedBodyPart &pkt);
	void OnPedDeath(const S_PedDeath &pkt);
	// The stream (docs/population.md §3 step 6). Three halves of the same
	// thing: say where our own pedestrians are, put everybody else's where
	// they said, and reconcile the traffic drivers into their seats.
	void SendHostedPedStates();
	void OnPedStates(const S_PedStates &pkt);
	// Every frame, from PreFrame, beside UpdateRemotes - not from PostFrame
	// like the car correction. WorldBridge::ApplyAmbientPedState says why.
	void ApplyAmbientPedPoses();
	// After both ambient spawn passes, so a ped and its car appearing on the
	// same frame are joined on that frame. The same loop as
	// UpdateRemoteSeats, over a different roster.
	void UpdateAmbientPedSeats();
	// Null when the session has never mentioned that netId.
	RemoteAmbientPed *AmbientPedByNetId(uint16_t netId);
	// Drops every replica, for a disconnect. Locally hosted peds are *not*
	// touched: they are this engine's own pedestrians and go on being
	// pedestrians when the session ends.
	void ClearAmbientPeds();

	// ---- ambient traffic (docs/population.md §3 step 4) --------------------
	//
	// Three of these four are the ped pass with the names changed. The fourth,
	// SendHostedCarStates, is the one that has no ped equivalent.
	void SendLocalAmbientCars();
	void SendHostedCarStates();
	void UpdateRemoteAmbientCars();
	// Every frame, after CGame::Process, beside CorrectRemoteVehicles.
	void CorrectAmbientCars();
	void OnCarSpawn(const S_CarSpawn &pkt);
	void OnCarDespawn(const S_CarDespawn &pkt);
	void OnCarStates(const S_CarStates &pkt);
	RemoteAmbientCar *AmbientCarByNetId(uint16_t netId);
	void ClearAmbientCars();

	NetThread   m_net;
	WorldBridge m_bridge;

	uint8_t      m_localPlayerId = 0xFF;
	// Our own netId, which is how everyone else's C_Damage names us. Kept so
	// an S_Damage that somehow arrives for somebody else can be thrown away
	// instead of applied to us.
	uint16_t     m_localNetId    = INVALID_NETID;
	RemotePlayer m_players[MAX_PLAYERS];
	RateLimiter  m_sendRate{SNAPSHOT_HZ};

	// ---- our own life ------------------------------------------------------
	//
	// One flag, set when a death has been announced and cleared when the
	// respawn has. Two things can notice a death - the CPed::SetDie detour,
	// which knows which animation played, and the health poll, which works
	// even when that detour failed to install - and this is what keeps them
	// from announcing it twice.
	bool     m_deathAnnounced = false;
	// Who hurt us last, and when, so a death can be credited to them. Plain
	// recency, the way every game does it: a player who shot you five
	// seconds ago and then watched you drown doesn't get the kill.
	uint16_t m_lastAttackerNetId = INVALID_NETID;
	uint32_t m_lastAttackerMs    = 0;
	static constexpr uint32_t KILL_CREDIT_MS = 5000;

	// What S_Welcome said about friendly fire. Only used to pass it on to
	// the bridge; the server is what actually enforces it.
	bool m_friendlyFire = false;
	// And what it said about ammunition (protocol.h, SESSION_AMMO_SYNC).
	// With it clear this client never samples its own counts, never sends a
	// C_PlayerAmmo, and ignores the two ammo fields in every snapshot it
	// receives - which is exactly the behaviour CoopIII had before any of
	// this existed.
	bool m_ammoSync = false;

	// ---- the wanted level (docs/wanted.md §4.6) ---------------------------
	//
	// Three bytes, and between them they are the entire feature's state.
	//
	// m_wantedRule is what S_Welcome said. It defaults to the per-player
	// rule rather than to "off" so that a build talking to a server too old
	// to set the bits behaves the way docs/roadmap.md §5.1 says the session
	// should by default.
	uint8_t m_wantedRule = WANTED_RULE_PERPLAYER;

	// The level this player reached without CoopIII's help, and the level
	// CoopIII last left the engine at. `own` is only ever re-read from the
	// engine when the engine has moved by itself - the comparison against
	// `applied` is how that is noticed, and it is the whole of the
	// earned-versus-granted bookkeeping. game/wanted.h, PlanWanted.
	uint8_t m_ownWanted     = 0;
	uint8_t m_appliedWanted = 0;

	// What went out on the last snapshot, so the flags a remote machine reads
	// and the flags this machine believes it sent cannot drift apart. Also
	// what the log line below reports.
	uint8_t m_sentWanted         = 0;
	bool    m_sentWantedBorrowed = false;

	// Said once each, the first time this machine raises its own player's
	// stars because of the session, and the first time it lowers them.
	//
	// A feature that does nothing has to say which step it stopped at
	// (AGENTS.md, "the damage that never landed"). Between them these two
	// lines answer the only question worth asking when somebody reports that
	// the wanted level "does not work": did this machine ever decide to
	// change its own player's stars, and in which direction.
	bool m_saidWantedRaised  = false;
	bool m_saidWantedLowered = false;

	// Whose game the session's time of day comes from. INVALID_PLAYER until
	// a welcome or a world packet says.
	uint8_t     m_hostPlayerId = INVALID_PLAYER;
	RateLimiter m_worldRate{1};

	// Whether it was us that pinned this machine's sky. Only set while we're
	// following somebody else's, and it's what stops a machine that has been
	// the host all along from releasing a weather its own mission script
	// forced - FORCE_WEATHER is a campaign opcode and undoing it mid-mission
	// would be us changing the weather, not following it.
	bool m_weatherPinned = false;

	// Sized for a co-op session, not for traffic - these are the cars
	// players are actually in or have touched, not Liberty City's whole
	// population. The server decides what's worth telling us about.
	static constexpr size_t MAX_REMOTE_VEHICLES = 64;
	RemoteVehicle m_vehicles[MAX_REMOTE_VEHICLES];

	// The car the local player is driving, as the session knows it.
	//
	// INVALID_NETID while on foot. Between getting in and the server's
	// reply it stays INVALID_NETID and `m_vehicleClaimPending` is set,
	// which is what stops the claim from being resent 25 times a second
	// while the round trip is in flight.
	uint16_t m_localVehicleNetId   = INVALID_NETID;
	uint32_t m_seatAnimTimeoutMs   = SEAT_ANIM_TIMEOUT_MS;

	// What we have already told the session about our own car's shape.
	// Compared against a fresh sample once per tick; a car that has not taken
	// a new dent sends nothing, which for most of a session is every tick.
	// Reset on every claim, because the next car is a different car.
	uint32_t m_sentDamagePanels    = 0;
	uint16_t m_sentDamageDoors     = 0;
	bool     m_haveSentDamage      = false;
	bool     m_saidDamageSent      = false;
	bool     m_saidDamageReceived  = false;

	// Wrecks the session has told us about that this machine has not applied
	// yet. Thirty-two is generous: the common case is that the explosion was
	// replayed here too and the car is already a wreck when the packet lands,
	// so an entry usually survives one frame. The backfill is what fills this
	// - a joiner can be handed a minute's worth at once.
	static constexpr size_t  MAX_PENDING_UNOWNED_WRECKS = 32;
	PendingUnownedWreck      m_unownedWrecks[MAX_PENDING_UNOWNED_WRECKS];
	// Retry for as long as the server would have kept the record. Past that
	// every machine's engine has had time to clear the shell and let the
	// generator park a fresh car, so a retry would start wrecking new cars.
	// Server-side twin: WRECK_BACKFILL_MS in server/core/session.h.
	static constexpr uint32_t UNOWNED_WRECK_RETRY_MS = 60000;
	bool m_saidUnownedWreckApplied = false;
	bool m_saidUnownedWreckSent    = false;
	bool m_saidUnownedWrecksFull   = false;

	// ---- garages -----------------------------------------------------------
	//
	// One mask per player slot, bit i = CGarages::aGarages[i]. A level rather
	// than a log of events, so a lost packet costs a door position until the
	// next change rather than a fact that never arrives.
	//
	// The local slot is not used: this machine's own opinion is read from the
	// engine seam every frame and lives in m_localGarageMask, because it is
	// the engine that decides it and Client that reports it.
	uint32_t m_garageMasks[MAX_PLAYERS] = {};
	uint32_t m_localGarageMask          = 0;
	// What we last told the session. 0 is also the initial value, which is
	// correct: a machine that has never deviated from rest has nothing to
	// say, and the first packet goes out on the first bit that sets.
	uint32_t m_sentGarageMask           = 0;
	bool     m_saidGarageHeld           = false;

	// The car we are riding in as a passenger, if any. Separate from
	// m_localVehicleNetId on purpose: that one means "we are driving this and
	// its physics are ours to report", and a passenger reports nothing.
	uint16_t m_localSeatNetId      = INVALID_NETID;

	// The car an animated passenger entry is walking towards, or INVALID_NETID.
	// Held here and not in game/seat.cpp because it is the session that has to
	// be told, and only once the engine has actually given the seat.
	uint16_t m_pendingSeatNetId    = INVALID_NETID;
	bool     m_saidSeatRefused     = false;
	bool     m_vehicleClaimPending = false;

	// When it is worth asking again after the session refused to name our
	// car, and whether the refusal has already been written down once.
	uint32_t m_claimRetryAtMs    = 0;
	bool     m_saidClaimRefused  = false;

	// Said once, the first time the local player gets into a car while this
	// machine is observing at least one of the session's, and the engine seam
	// cannot tell the two apart.
	//
	// It is a log line rather than a refusal because refusing would mean not
	// syncing the car at all, and because the only build that can reach it is
	// one where WorldBridge::SampleLocalVehicleHandle was left null. But it
	// must not be silent: what happens instead is a car registered twice,
	// which looks like a duplicate on every other screen and like a car that
	// will not move on this one - and that is exactly the bug this whole
	// area was reported as.
	bool     m_warnedNoVehicleHandle = false;

	// The model index we last told the session we're wearing. 0xFFFF isn't
	// a model - it means "nothing announced yet", which is what makes the
	// first sample always send. The hello carries a guess made before
	// there's even a player ped to ask; this is the correction.
	uint16_t m_sentModelId = 0xFFFF;

	// The last ammunition this client put on the wire for each of its own
	// thirteen slots, so a slot only costs a packet when it actually
	// changes.
	//
	// Seeded to "unowned, nothing in it", which is not a placeholder - it is
	// what the far side assumes about a player it has heard nothing about,
	// so seeding it that way is what stops a join announcing twelve empty
	// slots nobody needed to be told about.
	//
	// The held slot is written here from the sample without being sent - the
	// snapshot is carrying it - which is what stops a weapon switch firing
	// off a redundant C_PlayerAmmo for the gun that was just put away.
	AmmoSlotBody m_sentAmmo[INVENTORY_SLOTS] = {};

	// ---- ambient population -------------------------------------------------
	//
	// Sized for one machine's share of Liberty City, times the player cap.
	// CPopulation keeps roughly 25 pedestrians alive around one player, and
	// the server's own MAX_AMBIENT_PEDS is 256, so this matches it: a roster
	// smaller than what the server will send would silently drop peds.
	static constexpr size_t MAX_REMOTE_PEDS = 256;
	RemoteAmbientPed m_peds[MAX_REMOTE_PEDS];

	// Peds this machine hosts, waiting for the server to name them.
	//
	// Only the ids, because the ped itself is the engine's and population.cpp
	// is what holds it. This exists so the claim goes out exactly once per
	// ped, which matters more here than anywhere else in CoopIII: the thing
	// producing these is a generator that never stops.
	static constexpr size_t MAX_PENDING_PED_CLAIMS = 64;
	uint32_t m_pedClaims[MAX_PENDING_PED_CLAIMS]{};
	uint32_t m_pedClaimCount = 0;
	// The next temporary id. Starts at 1 because 0 is what a backfilled
	// S_PedSpawn carries, and the two must never collide.
	uint32_t m_nextPedTempId = 1;
	// Said once each rather than once per ped, because the stream behind
	// these is a pedestrian generator.
	bool     m_warnedPedRosterFull  = false;
	bool     m_warnedPedClaimsFull  = false;

	// ---- ambient traffic ----------------------------------------------------
	//
	// Matched to the server's MAX_AMBIENT_CARS for the same reason the ped
	// roster is matched to MAX_AMBIENT_PEDS: a roster smaller than what the
	// session will send drops cars silently.
	static constexpr size_t MAX_REMOTE_CARS = 128;
	RemoteAmbientCar m_cars[MAX_REMOTE_CARS];

	static constexpr size_t MAX_PENDING_CAR_CLAIMS = 32;
	uint32_t m_carClaims[MAX_PENDING_CAR_CLAIMS]{};
	uint32_t m_carClaimCount = 0;
	bool     m_warnedCarRosterFull = false;
	bool     m_warnedCarClaimsFull = false;

	// The hosted-car stream's own rate limiter, separate from m_sendRate.
	// 10 Hz rather than 25: protocol.h, MAX_CAR_STATES.
	uint32_t m_lastCarStateMs = 0;
	// And the hosted-ped stream's, kept separate from the car one rather
	// than shared. They run at the same rate today and there is no reason
	// they have to: peds are slower than cars and there are more of them, so
	// the ped rate is the first one step 5 will want to move on its own.
	uint32_t m_lastPedStateMs = 0;

	std::vector<Message> m_scratch;
	bool                 m_wasConnected = false;
};

} // namespace coopiii
