// Breakable street objects: lamp posts, traffic lights, parking meters, bins,
// cones, crates, barriers.
//
// docs/objects.md is the investigation and the design. Four sentences carry
// the whole file:
//
// 1. **Breaking is a latch, not a lifecycle.** `CObject::ObjectDamage`
//    (0x004BB240) never frees anything, never allocates anything and never
//    touches the world lists - every one of its nine arms writes flags on the
//    CObject that is already standing there. So "this is broken" is a piece of
//    state that only ever goes one way, applying it twice is a no-op, and
//    there is no creation, no deletion and no ownership handshake anywhere in
//    here. That is why this is a much smaller feature than a ped or a car.
//
// 2. **It is named by where the map put it.** `m_objectMatrix`'s position
//    plus the model index. Not the pool slot: the object pool churns from the
//    first second, because CPopulation::ManagePopulation turns every
//    map object more than 80 m from *the local player* into a dummy and back
//    again and the streamer centres on one player. The coordinate, by
//    contrast, is sscanf'd out of an IPL text file that is identical on every
//    install and copied into m_objectMatrix, where it survives every
//    conversion untouched. docs/objects.md 4 has the pairwise measurement
//    that says 0.25 m cannot name the wrong one.
//
// 3. **An explosion is already agreed and says nothing.** The object arm of
//    CWorld::TriggerExplosionSectorList computes its damage as
//    `300 * min((radius - distance) * 2 / radius, 1)` - two positions and a
//    radius, no RNG and no physics - and CoopIII already replays every
//    explosion at a position every machine agrees on. So objects blown up by
//    a blast break identically everywhere for free, and this file
//    deliberately stays quiet inside CWorld::TriggerExplosion rather than
//    sending a packet per bin per rocket.
//
// 4. **A bullet has never broken one.** CWeapon::FireInstantHit's
//    ENTITY_TYPE_OBJECT arm applies a move force and eight sparks and then
//    stops; there is no call to ObjectDamage anywhere in CWeapon. A scan of
//    the whole image finds five call sites and all five are CPhysical's
//    collision arms, CWorld's explosion arms and CObject::ProcessControl.
//    So what is left, and the only thing this file exists for, is **an
//    object somebody drove into**.
//
// ---------------------------------------------------------------------------
// What this file does NOT do
// ---------------------------------------------------------------------------
//
// 5. **Uprooting is one bit and one list, and it is not breaking.** An
//    object falls over when bIsStatic (CEntity byte A bit 2) is cleared and
//    CPhysical::AddToMovingList is handed the object; from there ordinary
//    physics takes it. ObjectDamage never clears that bit - its smash arm
//    *sets* it - so a post can be bent without coming loose and can come
//    loose without being bent. Three places decide it and all three read
//    m_fUprootLimit: a collision (impulse > limit), a blast (power > limit),
//    and a bullet, a pellet or a bat (limit <= 0). addresses.h has the
//    instructions.
//
//    Two of those three already agree on every machine and the third does
//    not, which is the same shape as points 3 and 4. A blast's power is a
//    pure function of two positions and a radius. A bullet's uproot is
//    replayed with the shot itself (protocol.md 1.9.2 fires the remote ped's
//    own CWeapon through the engine, out of the wire's muzzle and along the
//    wire's direction, so every observer's own DoBulletImpact runs the same
//    object arm) - and in any case object.dat gives every lamp post an
//    uproot limit of 400, every traffic light 500 and every meter, bin,
//    postbox and hydrant 100, so a bullet cannot knock any of them over at
//    all. **What no other machine ran is somebody else's collision.**
//
//    So what travels is the resting place, once, when the engine's own sleep
//    test decides the object has stopped - not an impulse and not a stream,
//    because roadmap.md 2.4 says two machines handed the same impulse do not
//    put the post down in the same place, and a machine that never uprooted
//    it has nothing to integrate.
//
// - **It does not suppress a local break.** An observer whose own engine
//   breaks the object - because a replica of somebody else's car really did
//   push through it here - is allowed to break it. It just does not *report*
//   it. Suppressing would leave a car sitting inside an intact lamp post; not
//   reporting is enough, because the owner's report is coming and applying a
//   break twice is a no-op.
//
// - **It does not keep a table, and the server does not either.** The engine
//   throws the state away at 80 m. There is nothing to back-fill a joiner
//   with and nothing to remember.
#pragma once

#include "client.h"

#include <coopiii/protocol.h>

#include <cstddef>
#include <cstdint>

namespace coopiii::game {

// ---------------------------------------------------------------------------
// Identity
// ---------------------------------------------------------------------------

// How close two objects of the same model have to be before they are treated
// as the same one.
//
// The same number the pickups use, and measured the same way rather than
// copied: every one of the 1851 breakable map instances in the 13 IPLs
// gta3.dat loads was compared pairwise inside its own model index. The
// closest same-model pair in Liberty City is 0.5992 m, nothing is under
// 0.50 m, and the 118 pairs inside 2 m are all stacked boxes, cones and
// newspaper boxes. 0.25 m clears the nearest ambiguity by 2.4x.
//
// Strictly it could be an exact compare - both machines read the coordinate
// out of the same text file - and it is a tolerance anyway, because a design
// that depends on two processes formatting a float identically is a design
// with a trap in it.
constexpr float kObjectIdentTolerance   = 0.25f;
constexpr float kObjectIdentToleranceSq =
    kObjectIdentTolerance * kObjectIdentTolerance;

// Do these two idents name the same object? Pure, so the tests cover it
// without a game.
bool SameObject(const ObjectIdent &a, const ObjectIdent &b);

// ---------------------------------------------------------------------------
// How broken is it
// ---------------------------------------------------------------------------

// The two CEntity flag bytes CObject::ObjectDamage writes, turned into the
// two bits that go on the wire.
//
// `flagsA` is +0x51 (bUsesCollision is bit 0) and `flagsB` is +0x52
// (bIsVisible is bit 2, bRenderDamaged is bit 7). "Smashed" is the pair
// rather than either one alone because that is what every smash arm writes
// together and neither half means it by itself: a glass pane is created with
// bIsVisible already false, and a ped warped into a car has bUsesCollision
// cleared without anything being broken.
//
// Pure arithmetic on two bytes, so the truth table is a test rather than a
// live game.
constexpr uint8_t BreakStateFromFlags(uint8_t flagsA, uint8_t flagsB) {
	uint8_t state = 0;
	if (flagsB & 0x80)   // bRenderDamaged
		state |= OBJ_BREAK_RENDER_DAMAGED;
	if ((flagsB & 0x04) == 0 && (flagsA & 0x01) == 0)   // !bIsVisible && !bUsesCollision
		state |= OBJ_BREAK_SMASHED;
	return state;
}

// Has this one come loose?
//
// The whole of "uprooted", and it is a CEntity flag rather than anything on
// CObject: bIsStatic is byte A bit 2, the engine clears it when the impulse
// beats m_fUprootLimit, and CWorld::Process then simulates the object every
// frame until CPhysical::ProcessControl's own sleep test sets it back.
//
// Deliberately NOT folded into BreakStateFromFlags. That function answers
// "how broken is it", which is a latch the receiver replays; this answers
// "is it still standing", which is a transform the receiver is told. Mixing
// them would make a smash - which sets bIsStatic - look like an un-uprooting.
constexpr bool ObjectIsLoose(uint8_t flagsA) {
	return (flagsA & 0x04) == 0;   // offs::ENTITY_IS_STATIC
}

// The whole state byte as it goes on the wire: how broken, plus whether it is
// standing.
constexpr uint8_t BreakStateWithUproot(uint8_t flagsA, uint8_t flagsB) {
	uint8_t state = BreakStateFromFlags(flagsA, flagsB);
	if (ObjectIsLoose(flagsA))
		state |= OBJ_BREAK_UPROOTED;
	return state;
}

// How many more times the engine's own ObjectDamage has to run here before
// this object looks like the one on the wire.
//
// At most two, and the two exists for exactly one effect:
// DAMAGE_EFFECT_CHANGE_THEN_SMASH sets bRenderDamaged on the first hit and
// smashes on the second, so an object that arrives at SMASHED from a machine
// that hit it twice needs both hits replayed here. Everything else is one
// call or none.
//
// Never negative: a local copy that is *more* broken than the wire says is
// left exactly as it is. There is no way to un-break an object in this engine
// and it would be the wrong thing to do anyway - the break already happened
// here, and the reporter's packet is just late.
//
// The uproot bit is masked off at the top rather than handled: ObjectDamage
// cannot uproot anything, so replaying it more times would never produce the
// bit, and a loose local copy is not a reason to hit a standing one again.
constexpr int BreakReplaysNeeded(uint8_t local, uint8_t wanted) {
	local  &= static_cast<uint8_t>(~OBJ_BREAK_UPROOTED);
	wanted &= static_cast<uint8_t>(~OBJ_BREAK_UPROOTED);
	if (local & OBJ_BREAK_SMASHED)
		return 0;   // already at the end state, whatever the wire says
	if (wanted & OBJ_BREAK_SMASHED)
		return (local & OBJ_BREAK_RENDER_DAMAGED) ? 1 : 2;
	if ((wanted & OBJ_BREAK_RENDER_DAMAGED) && !(local & OBJ_BREAK_RENDER_DAMAGED))
		return 1;
	return 0;
}

// ---------------------------------------------------------------------------
// A resting place off the wire
// ---------------------------------------------------------------------------

// Is this something we are willing to write into an entity's matrix?
//
// Nine floats off a socket go into the matrix the collision code reads, and
// pedanim.h's rule applies unchanged: NaN propagates, reaches collision, and
// faults somewhere with no obvious connection to netcode. The test is the
// cheapest thing that is still a real test - each row has to be finite and
// close to unit length - and it also rejects the all-zero body a truncated,
// zero-filled or hand-forged packet carries, which would otherwise collapse
// the object's collision volume to a point.
//
// Deliberately loose rather than exact. The rows were orthonormal when the
// reporter's engine produced them and they travel as the same 32-bit
// patterns, so this is a guard against a wrong packet, not a tolerance for a
// drifting one.
bool SaneRotation(const ObjectRestBody &body);

// ---------------------------------------------------------------------------
// Finding our copy, over a pool we are handed rather than the live one
// ---------------------------------------------------------------------------

// GTA III's CPool header, as CPopulation::ManagePopulation's own inlined walk
// reads it: an entry array, a parallel byte of flags per slot with
// POOLFLAG_ISFREE in bit 7, and a size.
//
// Taken as an argument instead of read off CPools::ms_pObjectPool inside the
// search, for one reason: it is what makes the matcher testable. The lookup
// is the part of this feature that decides whether it works at all, and
// "0.25 m never names the wrong lamp post" should be something a test can
// check rather than something a comment claims.
struct ObjectPoolView {
	uint8_t *entries = nullptr;
	uint8_t *flags   = nullptr;
	int32_t  size    = 0;
	size_t   stride  = 0;   // 0x19C for the object pool - NOT sizeof(CObject)
};

// The live object pool, or an empty view if the game has not made it yet.
ObjectPoolView LiveObjectPool();

// Nearest live map object of the same model inside kObjectIdentTolerance.
//
// Skips free slots, anything that is not ObjectCreatedBy == GAME_OBJECT, and
// any object flagged bIsPickup. `ambiguous` is set when more than one object
// matched, which the caller logs: the map measurement says it should never
// happen, and a design that silently picked one of two would be the kind of
// thing that is only ever noticed as "sometimes the wrong crate breaks".
void *FindObjectByIdent(const ObjectPoolView &pool, const ObjectIdent &want,
                        bool *ambiguous);

// ---------------------------------------------------------------------------
// Who reports
// ---------------------------------------------------------------------------

// What CoopIII knows about the entity that caused a break.
enum class BreakCause : uint8_t {
	// An entity this machine owns: the local player, their car, or an
	// ambient ped or car this engine generated and hosts
	// (docs/population.md 1). The physics that did it ran here.
	OURS,
	// A replica of somebody else's. Their machine ran the real collision;
	// ours ran a copy of it against an interpolated transform, and the
	// numbers are not the same numbers.
	REPLICA,
	// Nothing, or something that is neither a ped nor a vehicle. A stale
	// m_pDamageEntity, an object falling onto another object, a ped pushed
	// by nobody.
	NOBODY,
};

// The whole decision, as arithmetic, so the tests can walk the truth table.
//
// This is roadmap.md 5.8's model - exactly one machine reports, and it is the
// one the session says owns the thing - moved from the entity that is
// *broken* to the entity that *broke it*. It has to move, and the binary is
// what says so: CPopulation::ManagePopulation turns any map object more than
// 80 m from the local player back into a dummy, so an object across town
// from the host is not a CObject on the host at all and the host has nothing
// to observe. 5.8's answer for a parked car works because the server holds a
// row for that car whatever the distance; there is no row here and there must
// not be one.
//
// What does transplant is the shape: ownership decides, and the host owns
// whatever nobody owns.
constexpr bool MayReportBreak(uint8_t createdBy, bool isPickupObject,
                              bool haveSession, bool insideExplosion,
                              BreakCause cause, bool isHost) {
	// Only the map's own furniture. A MISSION_OBJECT is the script's and only
	// the host runs the script (docs/campaign.md), a TEMP_OBJECT is debris
	// that deletes itself on a timer, and a CUTSCENE_OBJECT is not in the
	// world in any sense that matters here.
	if (createdBy != 1 /* GAME_OBJECT */)
		return false;
	// A pickup's collision object is also created with ObjectCreatedBy ==
	// GAME_OBJECT, because CPickups::GenerateNewOne builds it through the
	// ordinary CObject constructor and CObject::Init sets that. bIsPickup is
	// what tells the two apart, and pickups are docs/pickups.md's business
	// from end to end.
	if (isPickupObject)
		return false;
	if (!haveSession)
		return false;
	// Already agreed everywhere, for free. docs/objects.md 5.
	if (insideExplosion)
		return false;

	switch (cause) {
	case BreakCause::OURS:
		return true;
	case BreakCause::REPLICA:
		return false;   // their machine is reporting this one
	case BreakCause::NOBODY:
		return isHost;
	}
	return false;
}

// Who is entitled to say where an object that just came loose ended up?
//
// The same three answers, reached one step earlier. An uproot happens a frame
// *before* the break it usually comes with - CPhysical's collision code
// clears bIsStatic while it is resolving the contact, and CObject::
// ProcessControl only passes m_fDamageImpulse to ObjectDamage on the next
// frame - so the cause has to be read at the moment the object is handed to
// the moving list, not at the moment it breaks.
//
// `hadImpulse` is `m_fDamageImpulse > 0` on the object right now, and that is
// a reliable question rather than a hopeful one: CPhysical::ProcessControl
// zeroes both m_fDamageImpulse and m_pDamageEntity at its top
// (0x00495F78/0x00495F82), so a non-zero impulse was written this frame by
// the collision that is unwinding around us, and the entity beside it is that
// collision's.
//
// No impulse means nothing collided with it, and there is exactly one other
// thing in the image that can clear bIsStatic on a breakable map object: the
// object arm shared by CWeapon::DoBulletImpact, CWeapon::FireShotgun and
// CWeapon::FireMelee. That arm only ever runs inside somebody's
// CWeapon::Fire - ours, or the one combat.cpp is replaying for a remote
// player right now. So **the shooter knows a break was theirs because the
// engine is still inside their own trigger pull when it happens**, which is
// a fact rather than an inference from a stale pointer.
//
// The alternative - leave it with the host and make the host's report travel
// - is worse, and docs/objects.md 5 already contains the proof without
// applying it here: CPopulation::ManagePopulation turns any map object more
// than 80 m from the local player back into a dummy, so the host is the one
// participant in the session who is not guaranteed to have the object at
// all. Handing an ownerless event to the host hands it to the machine most
// likely to be somewhere else.
constexpr BreakCause UprootCause(bool hadImpulse, BreakCause collisionCause,
                                 bool insideReplayedShot) {
	if (hadImpulse)
		return collisionCause;
	return insideReplayedShot ? BreakCause::REPLICA : BreakCause::OURS;
}

// ---------------------------------------------------------------------------
// What the client half has to provide
// ---------------------------------------------------------------------------
//
// Same split as the rest of the engine seam: object.cpp knows the engine and
// nothing about the wire, Client knows the wire and nothing about the engine.
struct ObjectCallbacks {
	// Tell the session an object broke here. Returns whether it actually went
	// out - being wired is not being connected, which is a distinction this
	// project has already paid for once in pickup.h.
	bool (*Broken)(const ObjectBreakBody &body) = nullptr;

	// Tell the session where an object we knocked loose came to rest. Same
	// contract as Broken: false means it did not go out.
	bool (*Settled)(const ObjectRestBody &body) = nullptr;

	// Is combat.cpp replaying somebody else's shot through the engine right
	// now? Null means "no", which makes every bullet uproot read as ours -
	// the single-player answer, and the one a client with nothing wired
	// should give.
	bool (*InReplayedShot)() = nullptr;

	// Is this ped / vehicle one CoopIII built as somebody else's? Takes the
	// engine's own pool reference rather than a pointer, for the same reason
	// the pickup seam does: a pool slot is reused the moment it is freed.
	bool (*IsReplicatedPed)(int32_t pedRef)     = nullptr;
	bool (*IsReplicatedVehicle)(int32_t vehRef) = nullptr;

	// Is there a session at all, and are we the host of it? Null for either
	// means "no", which turns the seam off rather than guessing - a client
	// that has not wired these behaves exactly like single player.
	bool (*HaveSession)() = nullptr;
	bool (*IsHost)()      = nullptr;
};

void SetObjectCallbacks(const ObjectCallbacks &callbacks);

// ---------------------------------------------------------------------------
// Installation
// ---------------------------------------------------------------------------

// Three detours.
//
// CObject::ObjectDamage (0x004BB240) is the one door: a whole-image scan for
// rel32 targets equal to it finds five call sites and all five are inside the
// engine's own collision and explosion code, so nothing can break an object
// in this build without coming through here.
//
// CWorld::TriggerExplosion (0x004B1140) is the guard that makes the first one
// quiet during a blast. It has exactly two callers in the image, both inside
// CExplosion, so one scope covers every explosion there is. Without it a
// rocket would send one reliable packet per bin in its radius, all of them
// duplicates of something the receiving engine had already worked out for
// itself.
//
// CPhysical::AddToMovingList (0x004958F0) is the one door for the other half.
// Every uproot in the image goes through it - the CPhysical collision arm,
// both explosion arms and the three weapon arms - and it is the only moment
// at which "who knocked this loose" is still a live fact. The detour is a
// read-only bracket: it calls the original and then looks at the object. It
// never manipulates the list, which is the thing client/src/game/movinglist.h
// exists to clean up after.
//
// Must not be called from DllMain - docs/compat.md 2.2, same rule as every
// other hook here.
bool InstallObjectHooks();
void RemoveObjectHooks();
bool ObjectHooksInstalled();

// ---------------------------------------------------------------------------
// The frame pump
// ---------------------------------------------------------------------------

// How many objects this machine can be carrying a resting place for at once.
//
// A car ploughing a row of lamp posts is the worst realistic case and it is
// nowhere near this; a rocket is bigger and is deliberately excluded, since
// an explosion uproots the same objects on every machine for the same reason
// it breaks them. Overflow drops the newest and counts it, because the
// alternative is growing a table on the game thread.
constexpr int kMaxUprootedWatched = 24;

// Called from PostFrame, after CGame::Process, which is exactly where the
// engine has just finished deciding whether each loose object is asleep.
//
// Walks the handful of objects this machine is the reporter for and sends one
// ObjectRestBody for each that has gone back to bIsStatic - the engine's own
// answer (CPhysical::ProcessControl counts m_nStaticFrames past 10 and then
// calls SetIsStatic(true)), not a threshold of ours. An entry whose object
// has left the pool, been converted back to a dummy or stopped being the one
// we named is dropped silently: the 80 m horizon does that constantly and it
// is not an error.
void TickUprootedObjects();

// ---------------------------------------------------------------------------
// Inbound
// ---------------------------------------------------------------------------

// Somebody else's machine broke this one. Finds our copy by ident and replays
// the engine's own ObjectDamage on it with the amount the reporter's engine
// used, until our copy is in the same state - so the particles, the sound and
// every flag each case sets are the engine's, not ours.
//
// Silently does nothing when there is no matching object here, which is the
// ordinary case rather than an error: the reporter was standing next to it
// and we are 300 m away, so our copy is a CDummyObject and there is nothing
// to break. When we walk over there the engine will build a pristine one,
// which is exactly what it does in single player after you drive away.
void OnObjectBrokenElsewhere(const ObjectBreakBody &body);

// Somebody else's machine watched this one fall over and stop. Finds our copy
// by ident - which still works, because the ident is where the *map* put it
// and that number has not moved on either machine - writes the matrix through
// the engine's own CMatrix::UpdateRW and CEntity::UpdateRwFrame, re-files it
// with CPhysical::RemoveAndAdd, and puts it back to sleep.
//
// RemoveAndAdd is not optional. An entity whose position is written from
// outside stays filed in the sector it was added in, and CRenderer::ScanWorld
// only walks the sectors around the camera - so a post that fell into the
// next sector would simply stop being drawn (addresses.h, "moving an entity
// the engine already owns").
//
// Setting bIsStatic is all the moving list needs: CWorld::Process's own loop
// unlinks any moving entity that has become static, at 0x004B1BA8 and
// 0x004B1BF3. Nothing here writes into that list.
void OnObjectSettledElsewhere(const ObjectRestBody &body);

void AddObjectsToBridge(WorldBridge &bridge);

// ---------------------------------------------------------------------------
// Diagnostics
// ---------------------------------------------------------------------------

struct ObjectStats {
	uint32_t breaksSeen        = 0;   // ObjectDamage actually changed something
	uint32_t reported          = 0;
	uint32_t unsent            = 0;   // worth reporting, no session to tell
	uint32_t skippedExplosion  = 0;   // already agreed everywhere
	uint32_t skippedReplica    = 0;   // somebody else's car did it
	uint32_t skippedUnowned    = 0;   // no owner and we are not the host
	uint32_t skippedNotMapObj  = 0;   // script object, debris, pickup
	uint32_t received          = 0;
	uint32_t receivedUnmatched = 0;   // no live copy here, usually distance
	uint32_t receivedNoop      = 0;   // ours was already at least that broken
	uint32_t replaysRun        = 0;   // ObjectDamage calls made on receipt
	uint32_t applyFailed       = 0;   // replayed and the state did not move
	uint32_t identCollisions   = 0;   // two live objects inside the tolerance

	// ---- uprooting --------------------------------------------------------
	uint32_t uprootsSeen       = 0;   // a map object was handed to the moving list
	uint32_t uprootsWatched    = 0;   // ...and it was ours to follow
	uint32_t uprootsNotOurs    = 0;   // somebody else's car did it
	uint32_t uprootsBlast      = 0;   // an explosion, agreed everywhere already
	uint32_t uprootsDropped    = 0;   // the watch table was full
	uint32_t uprootsLost       = 0;   // it left the pool before it stopped
	uint32_t restsSent         = 0;
	uint32_t restsUnsent       = 0;   // worth sending, no session to tell
	uint32_t restsReceived     = 0;
	uint32_t restsUnmatched    = 0;   // no live copy here, usually distance
	uint32_t restsApplied      = 0;
	uint32_t restsRefused      = 0;   // the matrix on the wire was not one
	uint32_t looseFromWire     = 0;   // dropped a standing post on a break
};

const ObjectStats &GetObjectStats();

} // namespace coopiii::game
