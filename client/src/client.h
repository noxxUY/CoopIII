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

	// A seated ped gets positioned by the engine, from the car, every frame.
	// Nothing else should write its transform - see ApplyRemotePose.
	bool Seated() const { return seatedVehicleNetId != INVALID_NETID; }
};

// Something the local player did with a weapon, on its way to the wire.
//
// Sampled by the detours in game/combat.h instead of read off the ped once a
// frame, because these are events and the ped only carries state - an Uzi
// empties a clip between two snapshots, and an explosion leaves nothing
// behind on the thrower to sample at all. docs/protocol.md §1.9.
struct CombatEvent {
	enum Kind : uint8_t { SHOT, EXPLOSION };
	uint8_t       kind = SHOT;
	ShotBody      shot{};
	ExplosionBody explosion{};
};

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
	bool (*SampleLocalVehicleIdentity)(uint16_t &modelId, uint8_t &colour1,
	                                   uint8_t &colour2, Vec3 &pos,
	                                   Quat &rot) = nullptr;

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

private:
	void OnWelcome(const S_Welcome &pkt);
	void OnJoin(const S_PlayerJoin &pkt);
	void OnLeave(const S_PlayerLeave &pkt);
	void OnPlayerModel(const S_PlayerModel &pkt);
	void OnPlayerState(const S_PlayerState &pkt);
	void OnVehicleSpawn(const S_VehicleSpawn &pkt);
	void OnVehicleDespawn(const S_VehicleDespawn &pkt);
	void OnVehicleState(const S_VehicleState &pkt);
	void OnEnterVehicle(const S_EnterVehicle &pkt);
	void OnExitVehicle(const S_ExitVehicle &pkt);
	void OnShot(const S_Shot &pkt);
	void OnExplosion(const S_Explosion &pkt);

	// Finds an existing slot, or claims a free one. Null when the table's
	// full, which isn't fatal - the vehicle just isn't shown, and the next
	// despawn frees a slot.
	RemoteVehicle *VehicleSlot(uint16_t netId, bool createIfMissing);

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
	// Every frame, not at the snapshot rate, and not rate-limited - these
	// are discrete events on the reliable channel and there are only ever a
	// handful per second.
	void SendLocalCombat();
	// One reliable packet on change, not two bytes riding every snapshot: a
	// model index only changes a handful of times in a playthrough at most.
	void SendLocalModel();

	NetThread   m_net;
	WorldBridge m_bridge;

	uint8_t      m_localPlayerId = 0xFF;
	RemotePlayer m_players[MAX_PLAYERS];
	RateLimiter  m_sendRate{SNAPSHOT_HZ};

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
	bool     m_vehicleClaimPending = false;

	// The model index we last told the session we're wearing. 0xFFFF isn't
	// a model - it means "nothing announced yet", which is what makes the
	// first sample always send. The hello carries a guess made before
	// there's even a player ped to ask; this is the correction.
	uint16_t m_sentModelId = 0xFFFF;

	std::vector<Message> m_scratch;
	bool                 m_wasConnected = false;
};

} // namespace coopiii
