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

	// Which of gFireManager's 40 slots holds the fire CoopIII lit on this
	// player's ped, or -1 for none. An index rather than a CFire*, for the
	// same reason poolHandle is a pool reference: the slot outlives the
	// fire, so the contents have to be asked whether it is still ours
	// (docs/roadmap.md §5.7 phase three).
	int8_t fireSlot = -1;

	// A seated ped gets positioned by the engine, from the car, every frame.
	// Nothing else should write its transform - see ApplyRemotePose.
	bool Seated() const { return seatedVehicleNetId != INVALID_NETID; }
};

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

	// Driven into the engine on change, not every frame.
	uint8_t appliedFlags = 0xFF;   // not a flag set: "nothing applied yet"
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

	// Put the local player in the first free passenger seat of the car this
	// pool ref names, and report which seat the engine gave them. -1 for no.
	// The engine picks the slot; CoopIII only reads back the number, because
	// that number is what the session has to be told.
	int32_t (*SeatLocalPlayerIn)(int32_t vehicleHandle) = nullptr;

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

	// Vehicles are keyed by the server's netId rather than by slot, so this
	// looks one up. Returns null if we've never been told about it.
	const RemoteVehicle *VehicleByNetId(uint16_t netId) const;

	// The car the local player is driving, as the session names it, or
	// INVALID_NETID on foot.
	uint16_t LocalVehicleNetId() const { return m_localVehicleNetId; }
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
		UpdateRemoteSeats();
		CorrectRemoteVehicles();
	}
	// Test seam: the outgoing half of a frame, which PostFrame keeps behind
	// a live socket. Send is a no-op while disconnected, so what this
	// exercises is the decision - claim a new car, take over one the session
	// already has, or report the one we are in - and the decision is where
	// the bug was.
	void TickLocalVehicle() { SendLocalVehicle(); }

private:
	void OnWelcome(const S_Welcome &pkt);
	void OnJoin(const S_PlayerJoin &pkt);
	void OnLeave(const S_PlayerLeave &pkt);
	void OnPlayerModel(const S_PlayerModel &pkt);
	void OnPlayerState(const S_PlayerState &pkt);
	void OnVehicleSpawn(const S_VehicleSpawn &pkt);
	void OnVehicleDespawn(const S_VehicleDespawn &pkt);
	void OnVehicleState(const S_VehicleState &pkt);
	void OnVehicleBlowUp(const S_VehicleBlowUp &pkt);
	void OnEnterVehicle(const S_EnterVehicle &pkt);
	void OnExitVehicle(const S_ExitVehicle &pkt);
	void OnShot(const S_Shot &pkt);
	void OnExplosion(const S_Explosion &pkt);
	void OnDamage(const S_Damage &pkt);
	void OnDeath(const S_Death &pkt);
	void OnRespawn(const S_Respawn &pkt);
	void OnWorldState(const S_WorldState &pkt);

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
	void TickPassengerSeat();
	// One packet per car per life, off the BlowUpCar detour's queue.
	void SendLocalVehicleBlasts();
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
	// Only the host sends this, and only once a second. Below the snapshot
	// rate because a game minute is a real second, so 25 Hz would be 25
	// identical packets for every one that said anything new.
	void SendLocalWorld();

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

	// The car we are riding in as a passenger, if any. Separate from
	// m_localVehicleNetId on purpose: that one means "we are driving this and
	// its physics are ours to report", and a passenger reports nothing.
	uint16_t m_localSeatNetId      = INVALID_NETID;
	bool     m_saidSeatRefused     = false;
	bool     m_vehicleClaimPending = false;

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

	std::vector<Message> m_scratch;
	bool                 m_wasConnected = false;
};

} // namespace coopiii
