// Reading and writing CVehicles - the vehicle half of WorldBridge.
//
// Same rule as ped.h: nothing here uses an offset that hasn't been confirmed
// against the disassembly. All 22 CVehicle offsets and the whole CREATE_CAR
// path got verified on 2026-09-21 and carry their proof in addresses.h.
//
// ---------------------------------------------------------------------------
// Who owns a vehicle
// ---------------------------------------------------------------------------
//
// The driver does. A car with a driver gets simulated by that driver's
// machine, and every other machine is an observer that just writes down
// what it's told - transform, velocities, control positions - and lets its
// own physics do nothing. Same rule as a remote ped, same reason: two
// machines running physics on one object disagree within a second, and the
// disagreement looks like rubber-banding, not like an obvious bug.
//
// `STATUS_PLAYER_REMOTE` is NOT how this is done. Looks like the obvious
// answer, and it isn't - see docs/protocol.md §1.4.
#pragma once

#include "client.h"
#include "combat.h"

#include <coopiii/protocol.h>

namespace coopiii::game {

// True when the local player is sitting in a vehicle (driving or riding).
bool LocalPlayerInVehicle();

// True when the local player is in this row's driver's seat, off the engine's
// own CVehicle::m_pDriver. See WorldBridge::LocalDrivesVehicle for why the
// roster's answer to the same question is not good enough.
bool LocalDrivesVehicle(const RemoteVehicle &vehicle);

// The live CVehicle behind a roster entry, or null when the engine no
// longer has it - in which case the entry's spawn gets re-armed on the way
// out, so the caller only ever has to handle "not right now".
//
// Public because seating a remote ped needs both a ped and a car, and the
// call that does it (CPed::WarpPedIntoCar) is a CPed method, so that code
// lives in ped.cpp. Nothing else outside this file should need it.
void *ResolveRemoteVehicle(RemoteVehicle &vehicle);

// Identity of the vehicle the local player is driving, for the claim packet
// that introduces it to the session (EnterVehicleBody in protocol.h). Returns
// false for the same reasons SampleLocalVehicle does.
//
// One struct rather than five out-params because the list grew: the extras
// joined the colours at protocol 9, and "add another reference argument" was
// already the wrong shape at four.
bool SampleLocalVehicleIdentity(VehicleIdentity &out);
int32_t SampleLocalVehicleHandle();

// Fills `out` from the vehicle the local player is driving. False when the
// player is on foot, is a passenger rather than the driver, or there's no
// player at all - in every one of those cases this machine isn't the owner
// and has nothing authoritative to say.
bool SampleLocalVehicle(VehicleStateBody &out);

// Creates a vehicle the engine's own way, following COMMAND_CREATE_CAR's
// sequence exactly (addresses.h, "vehicle lifecycle"). Sets
// RemoteVehicle::poolHandle. False if the model is not loaded or the
// constructor did not complete.
//
// Also applies the car's extras, and that has to happen here rather than
// anywhere else: they are RwAtomics cloned into the clump during construction,
// so the engine's CVehicleModelInfo::ms_compsToUse override is set immediately
// before the constructor call and put back immediately after. See the
// "Extras" note below and addresses.h.
bool SpawnRemoteVehicle(RemoteVehicle &vehicle);

// Removes it the engine's own way: CWorld::Remove,
// RemoveReferencesToDeletedObject, then the deleting destructor through the
// vehicle's own vtable - never the global operator delete, which is exactly
// what turned the ped despawn into a heap corruption.
//
// Unless the local player is sitting in it, in which case the car is handed
// to the engine instead of deleted (game/carlife.h, CopyEnd::HandToEngine).
void DespawnRemoteVehicle(RemoteVehicle &vehicle);

// ---------------------------------------------------------------------------
// A copy's life (game/carlife.h has the reasoning, carlife.cpp the code)
// ---------------------------------------------------------------------------

// The engine seam's own list of copies, by pool handle. In when
// SpawnRemoteVehicle builds one or AdoptPromotedCar keeps a replica; out when
// DespawnRemoteVehicle destroys one or it goes to the engine.
void NoteSessionCopy(int32_t handle);
void ForgetSessionCopy(int32_t handle);

// Before every frame's CGame::Process: MaxNumberOfCarsInUse becomes the
// engine's own value plus what the copies add to the generator's sum, so they
// cost this machine's traffic nothing. RestoreTrafficCap puts it back.
void UpdateTrafficAllowance();
void RestoreTrafficCap();

// VehicleCreatedBy to MISSION_VEHICLE with the counters moved to match. For a
// traffic replica that has just become a session car, so every copy is the
// same kind of car.
void MakeCopyAMissionCar(void *vehicle);

// A copy stops being CoopIII's: RANDOM_VEHICLE, unlocked, not collision-proof,
// off the copy list. Counters moved through UpdateCarCount.
void HandCopyToEngine(void *vehicle);

// The detour on CPools::SaveVehiclePool that keeps copies out of the
// single-player save. Installed from InstallVehicleHooks; a failure is logged
// on its own line and left out of that function's result.
bool InstallSaveGuard();
void RemoveSaveGuard();

// Writes an observed state onto an already-spawned vehicle.
void ApplyRemoteVehicle(RemoteVehicle &vehicle, const VehicleStateBody &body);
void RestRemoteVehicle(RemoteVehicle &vehicle);

// ---------------------------------------------------------------------------
// Custody of a car nobody is driving (protocol.h, S_VehicleCustody)
// ---------------------------------------------------------------------------
//
// Resting and pinning a driverless car is right for one parked on the street
// and wrong for one that was still moving when its driver got out. The pin is
// applied after physics every frame, so whatever pose the car was in at that
// instant is permanent - and CVehicle::CanPedEnterCar refuses a car whose
// up.z is *inside* +-0.1, which is a car on its side, which is exactly where a
// car that was rolling ends up. So the session hands one machine a bounded
// window in which it stops correcting the car and lets its own engine finish.
//
// Read a car by its roster row rather than by the local player's seat, for the
// C_VehicleState the custodian sends on the session's behalf.
bool SampleObservedVehicle(RemoteVehicle &vehicle, VehicleStateBody &out);

// Has it stopped? bIsStatic if the engine has already decided, and
// CanPedExitCar's own numbers before that - the tightest gates in the game,
// because the point of settling a car is that somebody can then use it.
bool VehicleAtRest(RemoteVehicle &vehicle);

// Is it on fire? A custodian doesn't hand a burning car back (client.h,
// CustodyMayEnd). False for a wreck or a car the pool no longer has.
bool VehicleBurning(RemoteVehicle &vehicle);

// A traffic car the session has just made a session car, under the same netId
// and with nothing created or destroyed anywhere. protocol.h, S_CarPromoted.
void AdoptPromotedCar(RemoteVehicle &vehicle, bool weHostedIt);

// The car the local player claimed, now that it has a netId: registered in
// the table the two vehicle detours read, the way AdoptPromotedCar registers
// a promoted car. Nothing changes while we drive it; it is what makes our
// engine refuse damage and destruction once somebody else does.
void AdoptClaimedVehicle(RemoteVehicle &vehicle);

// The session dropping a car our own engine made (RemoteVehicle::ours): its
// row leaves that table and the collision-proof bit comes off. The car itself
// is left alone.
void ReleaseOwnVehicle(RemoteVehicle &vehicle);

// Hand the driver's seat of a car over, because the session says somebody
// else has it now. The losing end of a carjack, and the one thing in this file
// allowed to take the wheel out from under the local player - see
// WorldBridge::SurrenderVehicleSeat for what entitles it to and
// game/seat.cpp's UnseatLocalPlayer for the rule it is the exception to.
//
// False when there is nothing to hand over: no car in the pool, or the engine
// does not have us at its wheel after all.
bool SurrenderVehicleSeat(RemoteVehicle &vehicle);

// Puts the vehicle back where the session says it is, and pushes the matrix
// to the RenderWare frame. Called every frame after the world has updated -
// a remote car is still simulated locally, since that's what turns its
// wheels and works its suspension, so the correction has to be the last
// thing that touches it before the frame draws.
void CorrectRemoteVehicle(RemoteVehicle &vehicle, const VehicleTransform &at);

// ---------------------------------------------------------------------------
// The damage model (docs/cardamage.md)
// ---------------------------------------------------------------------------
//
// Panels and doors, and nothing else. Wheels, lights and the engine status all
// converge on their own and the design says why with the disassembly; the
// short version is in protocol.h beside VehicleDamageBody.
//
// The shape of this is the one M4 settled for a ped and §1.11 settled for a
// wreck: read what this machine's own engine decided, put it on the wire as an
// event, and let every observer replay it through the engine's own function.
// What is new is only that damage accumulates, so the packet carries absolute
// state and the merge is a maximum.

// What shape the car the local player is driving is in. False for the same
// reasons SampleLocalVehicle is false, plus one more: a car that is already a
// wreck has nothing to report, because FuckCarCompletely gave it the same
// damage on every machine.
//
// `netId` is left at zero - netIds are the session's, and Client fills it.
bool SampleLocalVehicleDamage(VehicleDamageBody &out);

// The same reading, of the car a roster row names, for the machine the session
// has asked to settle it (S_VehicleCustody). Same refusals: not a car, or
// already a wreck.
bool SampleObservedVehicleDamage(RemoteVehicle &vehicle, VehicleDamageBody &out);

// Make an observed car wear the damage the session says it has.
//
// Writing the status bytes is NOT enough, which is the same lesson as the
// explosion: a panel you can see is an RpAtomic that was swapped or hidden
// when the status changed, so every component that moves needs its status
// written *and* CAutomobile::SetPanelDamage / SetBumperDamage / SetDoorDamage
// called for it - which is exactly the pair CReplay::ProcessCarUpdate does six
// times in a row, for exactly this problem.
//
// `flying` picks the appliers' `noFlyingComponents` argument, inverted:
//
//   true  - a change that arrived while the car is in front of you. The part
//           flies off. Each machine makes its own CObject for it, the same way
//           each machine breaks its own lamp posts.
//   false - a spawn or a backfill. A car that has been missing its boot for
//           ten minutes simply arrives without one, rather than greeting a
//           late joiner with a shower of doors out of the object pool. This is
//           the case CReplay passes `true` for, and the reason CoopIII does
//           not use CAutomobile::SetupDamageAfterLoad, which is the one-call
//           version and passes `false` unconditionally.
//
// Never lowers anything: the merge is a componentwise maximum, here as on the
// server, because no applier in the engine has an arm that restores an atomic.
void ApplyRemoteVehicleDamage(RemoteVehicle &vehicle,
                              const VehicleDamageBody &body, bool flying);

// Stop, or resume, this machine's own engine deciding a car's damage.
//
// One bit: CEntity::bCollisionProof. CAutomobile::VehicleDamage tests it once,
// unconditionally, and returns - no switch in front of it, unlike the ped case
// docs/protocol.md §1.10.2 is about - so it is a complete switch and not a
// backstop. Set on a car this machine is only watching, cleared the moment the
// local player takes the driver's seat, by the same test that decides whether
// to correct the transform.
//
// Without it an observer's own collisions keep denting a car it does not own,
// and because damage only climbs those invented dents are permanent.
void SetVehicleObserved(void *vehicle, bool observed);

// ---------------------------------------------------------------------------
// Destruction
// ---------------------------------------------------------------------------
//
// A car exploding is an event, and m_fHealth is only a number. Writing zero
// into an observer's copy of a remote car produces a car with no health that
// is not destroyed, because nothing in the engine watches health for zero -
// destruction goes through CAutomobile::BlowUpCar, and its callers are a
// damage transition, a bomb timer and a five-second fire timer. addresses.h
// has all three transcribed under "a car's destruction".
//
// So it travels as its own event, and the rule is CoopIII's usual one: the
// machine that owns the entity decides. A car's owner is its driver, so the
// driver's machine announces the blast and every observer replays it through
// the same engine function, at the transform the owner sends. One call gives
// both halves of what has to match - the explosion and the burnt shell it
// leaves - because BlowUpCar is where the engine decides both.
//
// A synced car with nobody driving it has no owner and therefore nobody
// entitled to destroy it. Those are left to each machine's own engine, the
// same way untouched traffic already is. docs/protocol.md §1.11.3.
//
// Except while somebody is settling it (S_VehicleCustody). For those two
// seconds the custodian's engine is the only one simulating the car, so it
// owns the car's condition the way a driver does: everybody else refuses
// damage and blow-up on it and sends their hits to the custodian, and the
// custodian's wreck goes out as UNOWNED_SESSION like any driverless car's.
//
// Which is also who owns the burn. A shot at a car nobody holds makes the
// shooter its custodian (Session::CustodyForHit), and a custodian keeps a
// burning car until it goes up (client.h, CustodyMayEnd): its health streams
// out so everybody draws the flames, everybody else holds the fire timer, and
// its own timer's BlowUpCar is the one wreck the session hears about.

// ---------------------------------------------------------------------------
// Ambient traffic (docs/population.md §3 step 4)
// ---------------------------------------------------------------------------
//
// The same four operations as a claimed car, on a car nobody is driving.
// game/population.cpp owns the seam that decides which cars these are and
// keeps the books; these are the engine calls, because everything they need -
// the CREATE_CAR sequence, the extras override, the matrix push - already
// lives in this file.
//
// The one difference that matters is in SpawnAmbientCarReplica's comment:
// VehicleCreatedBy is RANDOM_VEHICLE, not MISSION_VEHICLE, and it decides
// which engine counter the replica is counted in. Read it before changing it.

bool SpawnAmbientCarReplica(RemoteAmbientCar &car);
void DespawnAmbientCarReplica(RemoteAmbientCar &car);
void CorrectAmbientCarReplica(RemoteAmbientCar &car, const VehicleTransform &at);

// Is the local player at the wheel of this replica? The question
// CorrectAmbientCarReplica has always asked the engine, with an answer the
// roster can act on - getting into somebody else's traffic is an ownership
// change now, not just a reason to stop correcting. protocol.h, S_CarPromoted.
bool LocalDrivesAmbientCar(const RemoteAmbientCar &car);

// Reads one hosted car's transform and velocity out of the engine. False when
// the pool no longer has it, which is the caller's cue to drop the row.
bool SampleHostedCar(int32_t poolHandle, AmbientCarState &out);

// Is one of our own traffic cars honking, in a way our own audio would play?
// The bit SampleHostedCars puts in the batch's horn mask (game/horn.h,
// TrafficHornOnWire). False when the pool no longer has it.
bool HostedCarHonking(int32_t poolHandle);

// Is one of our own traffic cars' m_bSirenOrAlarm set? The bit for the
// batch's siren mask (game/siren.h). False when the pool no longer has it.
bool HostedCarSirenOn(int32_t poolHandle);

// Where a car is and which way up it is, in the shape the unowned blow-up
// packet carries. Read at the moment a host notices one of its own traffic
// cars has become a wreck, which is the only moment that answer is worth
// anything - see BlastTransform in protocol.h.
//
// Split out rather than reusing SampleHostedCar because that one also reads
// velocity and needs a pool handle, and the sweep that calls this already has
// the pointer in hand: a wreck has no velocity worth sending and re-resolving
// a handle it just walked would be work for nothing.
bool ReadCarBlastTransform(void *vehicle, BlastTransform &out);

// Put a car exactly where a blow-up report says it blew up, immediately
// before running BlowUpCar on it. The order matters and is the one
// BlowUpRemoteVehicle has always used: BlowUpCar reads GetPosition() for the
// explosion, the camera shake and the fire it lights, so a car corrected
// afterwards moves its shell and leaves all three in the wrong street.
void PlaceCarForBlast(void *vehicle, const BlastTransform &where);

// Everything that describes what a car *is*, for the spawn announcement:
// model, both colours, both extras, and where it is right now.
bool SampleAmbientCarIdentity(void *vehicle, AmbientCarBody &out);

// CPools::GetVehicleRef / GetVehicle, so population.cpp can hold engine
// references rather than raw pointers - a handle stops resolving the moment
// the engine deletes the car, reused slot or not.
void   *AmbientCarFromRef(int32_t poolHandle);
int32_t AmbientCarRef(void *vehicle);

// `LocalVehicleBlast` is declared in client.h beside CombatEvent, because
// WorldBridge names it and client.h may not include this file.

// Detours CAutomobile::BlowUpCar. Not fatal if it fails, and the log says so:
// without it a car's destruction neither travels nor gets held back, which is
// exactly the behaviour this replaces.
bool InstallVehicleHooks();
void RemoveVehicleHooks();
bool VehicleHooksInstalled();

// Hands over the blasts the local player's own car suffered since the last
// call. Bounded, oldest first, same shape as DrainLocalCombat and for the
// same reason: a car blowing up is an event the engine knows about and a
// 25 Hz sample does not.
//
// The netId isn't in here. The detour only knows "the car the local player is
// driving"; Client is the half that knows what the session calls it.
uint8_t DrainLocalVehicleBlasts(LocalVehicleBlast *out, uint8_t max);

// Replay somebody else's car blowing up: put it where they say it ended up,
// then run the engine's own CAutomobile::BlowUpCar on it.
//
// Returns false when the car is no longer in the pool, which is a "not now"
// rather than an error - Client marks the entry destroyed either way, because
// a car whose owner says it is wrecked must never be respawned as a new one.
bool BlowUpRemoteVehicle(RemoteVehicle &vehicle, const Vec3 &pos, const Quat &rot);

// ---------------------------------------------------------------------------
// Shooting a car somebody else is driving (protocol.h, VehicleHitBody)
// ---------------------------------------------------------------------------
//
// The direction the car packets never had. Everything else about a claimed car
// travels from its driver outwards - where it is, what shape it is in, that it
// exploded - and nothing travelled the other way, so a player could empty a
// clip into a replica and the machine that owns the car never heard.
//
// It is not the pedestrian bug with different nouns, and the difference is
// what makes it worse. A replica ped is bullet-, fire-, melee- and
// explosion-proof, so a shot at one was refused and nothing happened at all.
// CoopIII sets one proof flag on a replica car (bCollisionProof, below) and
// cannot set the rest - bExplosionProof would stop a replayed blast reaching a
// car it must reach identically everywhere - and the engine's remaining flags
// sit inside a switch with four arms that check nothing. So a shot at a replica
// car was *accepted*, by the one machine with no right to decide it: health
// came off a copy nobody else could see, and the two copies drifted apart while
// both screens looked correct.
//
// The fix is the pedestrian one's shape with the engine's own arity: the
// shooter reports, the owner applies it through its own
// CVehicle::InflictDamage, and the result comes back on packets that already
// exist - the health on the driver's 25 Hz snapshot, the dents on
// C_VehicleDamage, the wreck on C_VehicleBlowUp.

// The hits the local player landed on other people's cars since the last call.
// Bounded, oldest first, same shape as DrainLocalVehicleBlasts.
//
// Wider than the blast queue because the events are: a blast is one per car per
// life and a burst is one of these per round. Not deduplicated either - two
// rounds into one car are two hits and single player applies both.
uint8_t DrainLocalVehicleHits(VehicleHitBody *out, uint8_t max);

// Take a hit somebody else reported on a car this machine is driving, or is
// settling for the session, through the engine's own CVehicle::InflictDamage.
//
// `attacker` is who fired, or null when their ped has not streamed in here -
// the shot happened either way. It is resolved to their replica and handed to
// the engine as the culprit, so the scorch marks and m_pSetOnFireEntity point
// at the player who did it.
//
// `settling` is true when the roster has us as the car's custodian rather
// than its driver. The engine check is then an empty driver's seat instead of
// us in it (MayTakeReportedHit).
//
// Does nothing, quietly, when the engine no longer has the car, when the engine
// disagrees that it's ours, when the car is already a wreck, or when the
// cause or the amount is not one this wire is allowed to carry. Each of those
// is an ordinary race rather than an error; the one that is a decision is the
// first, and vehicle.cpp says why it is not retried.
void ApplyRemoteVehicleHit(RemoteVehicle &vehicle, RemotePlayer *attacker,
                           const VehicleHitBody &body, bool settling);

// Who else holds the session car `netId`, into the table the two detours
// read. Client works both names out from the roster before every frame's
// physics (Client::NoteVehicleHolders); 0xFF means nobody, or us. `weSettle`
// is the custody that is ours (client.h, VehicleHolders).
void NoteVehicleHolders(uint16_t netId, uint8_t driverPlayerId,
                        uint8_t custodianPlayerId, bool weSettle);

// ---------------------------------------------------------------------------
// Shooting somebody else's traffic (protocol.h, C_CarHit)
// ---------------------------------------------------------------------------
//
// docs/protocol.md §1.23. A replica of a traffic car another machine hosts
// takes no damage and never blows up here. A hit the local player lands on
// one is queued for that host instead, and the host's C_UnownedBlowUp is the
// only thing that wrecks the replica.

// Hits the local player landed on traffic replicas since the last call.
uint8_t DrainLocalCarHits(VehicleHitBody *out, uint8_t max);

// Apply a hit somebody reported on traffic this machine hosts, through the
// engine's own CVehicle::InflictDamage. Quietly does nothing when the car is
// gone, already a wreck, or the cause or amount isn't one the wire may carry.
void ApplyHostedCarHit(uint16_t netId, RemotePlayer *attacker,
                       const VehicleHitBody &body);

// Run a car's own BlowUpCar through its vtable with CoopIII's say-so, so the
// detour lets it through. For the replays that the owner of a car asked for.
// False when the object has no vtable or no slot 29.
bool BlowUpCarAsOwnerSaid(void *vehicle);

// For game/darkel.cpp. CDarkel::RegisterCarBlownUpByPlayer is called from
// inside BlowUpCar (0x0053BF04), and the rampage needs to know two things
// about the car going up at that moment.
//
// `replay` is true while CoopIII is running BlowUpCar for a wreck another
// machine decided: BlowUpRemoteVehicle, BlowUpCarAsOwnerSaid and
// WreckUnownedVehicle. `key` names the car when more than one machine can
// decide its wreck, with the names C_UnownedBlowUp already uses:
// UNOWNED_SESSION for a session car nobody is driving, UNOWNED_PARKED for a
// car generator's car. Anything else is RAMPAGE_CAR_UNKEYED.
struct WreckForRampage {
	bool              replay = false;
	UnownedVehicleKey key    = {RAMPAGE_CAR_UNKEYED, 0, 0};
};
WreckForRampage DescribeWreckForRampage(void *vehicle);

// Just the first half, for the occupants BlowUpCar hands to
// CDarkel::RegisterKillByPlayer on the way (0x0053BDB6, 0x0053BE17).
bool ReplayingVehicleBlast();

// Holds ReplayingVehicleBlast() true for its lifetime, for a replay run from
// outside this file. game/heli.cpp puts the destruction of somebody else's
// helicopter under it: CDarkel::RegisterCarBlownUpByPlayer has a caller in
// CHeli::UpdateHelis (0x0054A04F), and whatever of that destruction is ever
// replayed here must not count on this machine as well as on the owner's.
class VehicleBlastReplayScope {
public:
	VehicleBlastReplayScope();
	~VehicleBlastReplayScope();
	VehicleBlastReplayScope(const VehicleBlastReplayScope &)            = delete;
	VehicleBlastReplayScope &operator=(const VehicleBlastReplayScope &) = delete;

private:
	bool m_was;
};

// ---- the decision the two detours make, without an engine ------------------
//
// Who gets to take health off a car, and whether it may blow up here. Pure so
// tools/clienttest can walk every case; vehicle.cpp fills the inputs from the
// Observed table, the replica table, CVehicle::m_pDriver and the culprit.
enum class CarOwner : uint8_t {
	// This engine decides. The local player is at the wheel, CoopIII is
	// applying a hit or a wreck somebody else reported, or no other machine
	// has a claim on the car: traffic we host, a parked car, a session car
	// nobody is driving.
	Local,
	// A session car another player is driving (protocol 23).
	RemoteDriver,
	// A replica of traffic another machine hosts (§1.23).
	RemoteHost,
	// A session car nobody is driving that another player is settling
	// (S_VehicleCustody). Their engine is the one simulating it for those two
	// seconds, so it is theirs to damage for the same reason a driven car is
	// its driver's, and a hit goes to them on the same C_VehicleHit.
	RemoteCustodian,
	// A session car nobody drives or settles - parked, walked away from, the
	// settle over. Every machine holds its health at the session's last word,
	// so a hit taken here was undone the next frame and a shot-up parked car
	// never caught fire. A hit from the local player goes out as C_VehicleHit
	// and the server makes the shooter its custodian, so one engine owns the
	// damage and the fire timer. A blast is still every machine's own.
	Nobody,
};

// The driver outranks the custodian, which is Session::MayReportVehicle's
// precedence; the Observed table never holds both anyway. `unheld` is an
// Observed row naming nobody, and us not settling it either.
inline CarOwner ClassifyCar(bool authorised, bool weDrive, bool remoteDriver,
                            bool remoteCustodian, bool trafficReplica,
                            bool unheld = false) {
	if (authorised || weDrive)
		return CarOwner::Local;
	if (remoteDriver)
		return CarOwner::RemoteDriver;
	if (remoteCustodian)
		return CarOwner::RemoteCustodian;
	if (trafficReplica)
		return CarOwner::RemoteHost;
	if (unheld)
		return CarOwner::Nobody;
	return CarOwner::Local;
}

enum class CarDamageVerdict : uint8_t {
	Apply,     // let the engine run
	Refuse,    // drop it here, nobody hears about it
	Forward,   // drop it here and send it to the owner
};

// Only the local player's own ray or melee reach is forwarded, and not into a
// wreck. Blasts are refused without forwarding because every machine replays
// the explosion and the owner's copy of it is the one that counts. Fire is
// refused for the same reason: a CFire on a car can come from a replayed
// explosion as easily as from a flamethrower, and forwarding it would count
// the owner's own fire twice.
//
// A car nobody holds takes blasts here as it always did: every machine replays
// the same explosion at the same place, so a car it kills dies everywhere and
// roadmap.md 5.8 carries the wreck. Everything else is refused, and the local
// player's own round goes to the session, which gives the car to the shooter.
inline CarDamageVerdict DecideCarDamage(CarOwner owner, bool byLocalPlayer,
                                        bool wrecked, uint8_t weapon) {
	if (owner == CarOwner::Local)
		return CarDamageVerdict::Apply;
	if (owner == CarOwner::Nobody && !IsForwardableDamage(weapon) &&
	    !IsFireDamage(weapon))
		return CarDamageVerdict::Apply;
	if (byLocalPlayer && !wrecked && IsForwardableDamage(weapon))
		return CarDamageVerdict::Forward;
	return CarDamageVerdict::Refuse;
}

// Which kind of car a replayed round has hit here (combat.h, ReplayTarget).
// Only a Local car can be ours; the rest belong to the machine the detour
// already refuses them for. Among ours, the shooter forwards a hit on the car
// we drive (they see a driven session car), on one we settle (a car in
// custody) and on our named traffic (a replica on their side). A parked car is
// Local on their machine too, so nothing is forwarded for it. A session car
// nobody holds is Nobody here and there, and never reaches this.
inline ReplayTarget ClassifyReplayCar(CarOwner owner, bool weDrive, bool weSettle,
                                      bool hostedTraffic, bool named) {
	if (owner != CarOwner::Local)
		return ReplayTarget::OtherMachines;
	if (weDrive)
		return ReplayTarget::CarWeDrive;
	if (weSettle)
		return ReplayTarget::CarWeSettle;
	if (hostedTraffic)
		return named ? ReplayTarget::NamedHostedCar : ReplayTarget::UnnamedHostedCar;
	return ReplayTarget::NobodysCar;
}

// BlowUpCar has no proof flag to test (it only reads bCanBeDamaged, at
// 0x0053BC69), so this is the whole gate. A car nobody holds may still go up
// here: a blast that kills it does so on every machine, and so does the fire
// timer of one whose custodian left while it burned.
inline bool MayBlowUpCar(CarOwner owner) {
	return owner == CarOwner::Local || owner == CarOwner::Nobody;
}

// How many machines get to decide that this car is a wreck, for the money a
// wreck pays (game/money.h). Only a Local car can be decided here at all.
// Among those, the one we drive or settle and the traffic we host are ours
// alone; a parked car or a session car nobody holds is Local on every
// machine that has it, and each of them may blow it up.
enum class WreckDecider : uint8_t {
	Elsewhere,    // BlowUpCar is refused here
	OnlyHere,
	Everywhere,
};

inline WreckDecider WhoDecidesWreck(CarOwner owner, bool weDrive, bool weSettle,
                                    bool hostedTraffic) {
	if (!MayBlowUpCar(owner))
		return WreckDecider::Elsewhere;
	if (weDrive || weSettle || hostedTraffic)
		return WreckDecider::OnlyHere;
	return WreckDecider::Everywhere;
}

// The same questions BlowUpCarCommon asks, for a car the engine is about to
// pay for. `key` is filled for a car decided Everywhere that the session can
// name - UNOWNED_SESSION or UNOWNED_PARKED - and left MONEY_AWARD_UNKEYED
// otherwise.
WreckDecider WhoDecidesWreckHere(void *vehicle, UnownedVehicleKey &key);

// The player the session says is driving this car - the Observed table's
// driver - or INVALID_PLAYER. Only compares pointers, so it is safe on a
// CEntity* that is not a car or no longer exists.
uint8_t RemoteDriverOf(const void *vehicle);

// Our flame has just reached a car (combat.h, IsOurFlame): does the car's
// owner need telling? Only when it isn't us. A session car nobody holds goes
// too, and the session makes us the one to light it. The fire the flame
// lights on that car here is a picture, not damage - DecideCarDamage above
// still refuses cause 9 on it and never forwards it - so the ignition is the
// one thing that has to travel.
inline bool FlameGoesToOwner(CarOwner owner) { return owner != CarOwner::Local; }

// combat.cpp's CFireManager::StartFire detour, when IsOurFlame says the flame
// is ours: queue the ignition for whoever owns this car, as C_VehicleHit for a
// car somebody drives or settles and C_CarHit for traffic somebody hosts, with
// cause 9 and no amount. False when the car is ours to burn, or when the
// throttle says the owner heard about it less than a second ago.
bool ReportOurFlameOnCar(void *vehicle, FlameReportThrottle &throttle, uint32_t nowMs);

// The receiving end: may a hit somebody else reported go into this car here?
// `atOurWheel` and `wheelEmpty` are the engine's CVehicle::m_pDriver, asked at
// the moment of applying; `settling` is the roster saying we hold custody and
// haven't yet said we're finished with it. Client only calls the seam when
// the roster says one of the two.
//
// A custody car also needs an empty driver's seat. Somebody sitting in it is
// a driver the server will already have recorded, which ends the custody -
// the packet saying so just hasn't been read here yet.
inline bool MayTakeReportedHit(bool atOurWheel, bool wheelEmpty, bool settling) {
	return atOurWheel || (settling && wheelEmpty);
}

// ---------------------------------------------------------------------------
// Cars nobody owns (docs/roadmap.md 5.8)
// ---------------------------------------------------------------------------
//
// The driver reports their own car. A parked car has no driver, so it had
// nobody - and blowing up a parked car is the commonest way a car is
// destroyed in this game.
//
// Two kinds of car, and this file owns one of them. A car the session has a
// row for - claimed, driven, parked, walked away from - travels as
// UNOWNED_SESSION with its netId, and the roster resolves that
// (Client::WreckSessionVehicle), because a netId is the session's name for a
// car rather than the engine's. What is below is the other kind.
//
// The name a parked car travels under is its car generator index. Every
// PARKED_VEHICLE comes out of CTheCarGenerators::CarGeneratorArray, the array
// is filled from the IPLs in file order, and the same files in the same order
// give the same index on every machine - so the map hands the identity out
// and the server never has to. addresses.h, "parked cars, and the name the
// map already gives them", has the proof, including that the generator's
// m_nVehicleHandle is a pool ref in exactly CPools::GetVehicleRef's encoding.
//
// The scope is deliberately one fact and not a state stream, and the reason
// is worth reading before anything is added to it. An explosion is already
// replayed on every machine at a position everyone agrees on, and
// CWorld::TriggerExplosion damages every car in the radius through
// CVehicle::InflictDamage with a multiplier that is a function of distance
// and nothing else. So a parked car blown up by a rocket is ALREADY a wreck
// everywhere. What does not converge is damage that accumulates - gunfire
// spread is rolled per machine, a shove from a replica is not a collision
// anybody simulated twice - and the engine's five-second fire timer turns a
// difference in health into a difference in whether the car ever explodes at
// all. This carries that difference and nothing else.
//
// Nothing else about an unowned car travels, on purpose:
//   - its position, because the map put it there on every machine and a
//     transform stream for every parked car in Liberty City is not a budget
//     anybody has (docs/population.md 2.1);
//   - its health, because writing health destroys nothing and a low one arms
//     the fire timer on the receiver, which is an observer deciding to
//     destroy somebody else's car five seconds later;
//   - its doors, panels and dents, because the cause already travels and the
//     divergence is a dent.

// The unowned cars this machine's engine destroyed since the last call. Same
// shape as DrainLocalVehicleBlasts, and off the same detour.
uint8_t DrainUnownedBlasts(UnownedBlast *out, uint8_t max);

// Make the car this key names a wreck here, through the engine's own
// BlowUpCar. Idempotent, and honest about "not right now": a backfilled wreck
// routinely names a car that is three streets away and not streamed in.
UnownedWreckOutcome WreckUnownedVehicle(const UnownedVehicleKey &key);

// Wires the two of those into the bridge. Separate from MakeWorldBridge for
// the same reason AddWorldToBridge is: they hang off a detour rather than the
// frame pump, and they install and fail on their own.
void AddVehicleBlastToBridge(WorldBridge &bridge);

// ---------------------------------------------------------------------------
// Extras
// ---------------------------------------------------------------------------
//
// "Hay autos que a un player le aparecen con extras que el otro jugador no ve
// y viceversa." The engine picks a car's extra components at spawn, on each
// machine, out of CVehicleModelInfo::ChooseComponent - so two machines roll
// independently and two players get two different cars.
//
// This is the same family of bug as the paint job and it carries on the wire
// next to it: EnterVehicleBody and S_VehicleSpawn now hold extra1/extra2, the
// claimer's own CVehicle::m_aExtras, and every observer fits those.
//
// It is NOT fixed the same way, and that is the only surprising thing here.
// A colour is read by the renderer every frame, so writing m_currentColour1/2
// after construction changes the car. m_aExtras is a record of a decision
// already taken: the components are RwAtomics that were cloned into the clump
// while the car was being built, and writing those two bytes afterwards
// changes the record and nothing on screen. So the choice is forced before
// the constructor, through the engine's own one-shot override, which exists
// because the garages restore a saved car's variation exactly this way.
//
// docs/protocol.md §1.12. The mechanism, its two traps, and the unchecked
// subscript that makes a wire value dangerous are in addresses.h under
// "a vehicle's extra components".

} // namespace coopiii::game
