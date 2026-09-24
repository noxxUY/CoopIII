// Pickups: making sure two players cannot take the same one.
//
// docs/pickups.md is the investigation and the design. What a reader of this
// file needs is the three sentences the whole thing rests on:
//
// 1. **Nobody has to spawn anything.** CoopIII does not suppress the main
//    script on clients (that is M5 Tier 2, not built), so every machine runs
//    main.scm and creates all 448 script pickups itself, from literal
//    coordinates, through a CPickups::GenerateNewOne that never calls the
//    RNG. The worlds already agree. What was missing was only exclusivity.
//
// 2. **The client detects, the server arbitrates, the engine awards - in
//    that order.** "Am I standing on this" is a place, and a question you
//    answer about yourself, exactly like roadmap.md 5.7's fire. But unlike
//    fire the resource is exclusive, so two people can both be correctly
//    standing on the same shotgun and somebody still has to say who gets it.
//    Only the server sees both claims.
//
//    The ordering is the load-bearing part. A pickup reward cannot honestly
//    be revoked - ammo already fired, health already spent in a fight, a
//    bribe that already cleared a wanted star - so the arbitration has to
//    happen before the award and not after it.
//
// 3. **The lever is m_pObject, and nothing here re-implements the engine.**
//    CPickup::Update returns false immediately when m_pObject is nil
//    (0x00430880), *below* the m_bRemoved respawn branch and *above* the
//    mine switch, the touch test and the whole award switch. So CoopIII
//    stashes and nils m_pObject for every pickup it has not been granted,
//    calls the engine's own CPickups::Update, and puts them back. The engine
//    physically cannot award a pickup CoopIII has not unblocked, and every
//    pickup it *does* unblock goes through the engine's own detection, its
//    own CanBePickedUp, its own per-type award switch and its own removal.
//
//    Not m_eType, which would have been the obvious blank: GenerateNewOne's
//    free-slot scan reads m_eType, and a mine detonating inside
//    CPickups::Update can kill a ped and reach CreateDeadPedMoney ->
//    GenerateNewOne reentrantly. Blanking m_eType in that window hands out a
//    live slot. m_pObject has no such reader.
//
// ---------------------------------------------------------------------------
// The one pickup the script does not make
// ---------------------------------------------------------------------------
//
// A dead pedestrian's money and guns. docs/pickups.md 10, and the short
// version is that the interesting question turned out to be already answered:
//
// - **It is already made exactly once in the session.** CPed::SetDead calls
//   CreateDeadPedWeaponPickups and CreateDeadPedMoney for any ped that is not
//   the local player. On a replica of somebody else's ped both come to
//   nothing - the money creator's fourth gate is
//   `CharCreatedBy == MISSION_CHAR` and every replica CoopIII builds is one,
//   and the weapon creator walks an inventory a replica has never been given.
//   So the machine that hosts the pedestrian is the only one that rolls
//   anything, which is CoopIII's ownership rule holding by construction
//   rather than by a lock. There is no race to close and no double roll to
//   suppress. What was missing is that nobody else heard about it.
//
// - **Except for a remote player's ped, where the same code was a bug.**
//   An observer's copy of a player *is* given a weapon (game/ped.cpp, with
//   CoopIII's own invented 1000 rounds), CreateDeadPedWeaponPickups has no
//   CharCreatedBy gate at all, and the dying player's own machine skips the
//   whole thing because there the ped is FindPlayerPed(). So a death put a
//   gun on the pavement of every machine except the one that mattered. The
//   detours below refuse it, which makes every machine agree by agreeing
//   with the engine's own rule: nobody drops their own player's weapons.
//
// - **The money is rolled and the weapons are not.** CreateDeadPedMoney takes
//   five draws from CGeneral::GetRandomNumber - the amount, and two per
//   pickup for the scatter. CreateDeadPedWeaponPickups takes none: the
//   scatter is `i * 1.75f` and its line-of-sight retry checks buildings only.
//   That was worth measuring and it is not what the design rests on, because
//   an observer has neither the ped's inventory nor its health on the wire
//   and so could not re-derive the deterministic half either.
//
// So a drop rides a new packet, it carries what the engine actually created
// rather than what CoopIII predicts it would, and the observers replay it
// through CPickups::GenerateNewOne. From the next frame it is an ordinary
// pickup and the exclusivity above owns it.
//
// ---------------------------------------------------------------------------
// What this file does NOT do
// ---------------------------------------------------------------------------
//
// - It does not decide whether a pickup was collected. The engine does.
// - It does not apply a reward. The engine does, on the machine that won.
// - It does not re-implement the touch test or CanBePickedUp. The 4 m
//   proximity test below is CoopIII's own network heuristic: it decides when
//   to *ask*, never whether something was collected. Getting it wrong costs
//   a wasted packet or a late grant, never a wrong award.
// - It does not touch mines (types 8..13). They are script-only, retail III
//   barely uses them, and their branch is about arming and exploding rather
//   than about giving anybody anything. They stay entirely local.
#pragma once

#include "client.h"

#include <coopiii/protocol.h>

#include <cstddef>
#include <cstdint>

namespace coopiii::game {

// ---------------------------------------------------------------------------
// Identity
// ---------------------------------------------------------------------------

// How close two pickups of the same model have to be before they are treated
// as the same one. Generous on purpose: script pickups are bit-identical on
// every machine, and the tolerance is there for a ped drop whose z came out
// of CWorld::FindGroundZFor3DCoord.
//
// Nothing in the retail script aliases at this distance. 312 script pickup
// coordinates were compared pairwise within each model: 29 same-model pairs
// are within 2 m and all 29 are at distance exactly zero, and every one of
// those is a coordinate written twice for a pickup that is re-created after
// being destroyed - the Ammu-Nation counter's in-stock/out-of-stock pair in
// the two arms of one `if`, and the 20 rampages, each of which runs
// `0215 destroy_pickup` before re-creating itself. The 100 hidden packages
// are at least 30.9 m apart.
constexpr float kIdentTolerance   = 0.25f;
constexpr float kIdentToleranceSq = kIdentTolerance * kIdentTolerance;

// How near the local player has to be before CoopIII claims.
//
// The engine collects at |dz| < 2.0 and dx*dx + dy*dy < 1.8, i.e. about
// 1.34 m flat. Four metres is roughly half a second of walking of slack,
// which is more than a round trip on anything worth playing on.
constexpr float kClaimRadius   = 4.0f;
constexpr float kClaimRadiusSq = kClaimRadius * kClaimRadius;

// Do these two idents name the same pickup?
//
// Pure, so tools/clienttest covers it without the game.
bool SameIdent(const PickupIdent &a, const PickupIdent &b);

// ---------------------------------------------------------------------------
// Who may make a ped's drop
// ---------------------------------------------------------------------------

enum class DropDecision : uint8_t {
	// Do not run the engine's creator at all. This ped's decisions belong to
	// another machine, and what it leaves behind is one of them.
	REFUSE,
	// Run it and say nothing. The local engine's own business, exactly as in
	// single player: a script ped, a cop, anything main.scm made - every
	// machine has its own copy and its own copy will drop its own.
	LOCAL,
	// Run it and tell the session what it made. A pedestrian this engine
	// generated, which under docs/population.md 1.1 is one this machine hosts.
	ANNOUNCE,
};

// The whole decision, as arithmetic, so tools/clienttest can walk the truth
// table without a game. The engine-facing half of the seam does nothing but
// fetch these three facts and act on the answer.
//
// `charCreatedBy` is CPed::CharCreatedBy at +0x160.
// CHAR_CREATED_BY_RANDOM is written by the population generator and by
// nothing else: every ped CoopIII builds is set to MISSION_CHAR on purpose
// (it is what stops the engine reaping it) and so is every ped the script
// creates.
constexpr DropDecision DecideDrop(bool replicaOfSomebodyElses, bool haveSession,
                                  uint8_t charCreatedBy) {
	// First, and unconditionally. A replica is refused whether or not there
	// is still a session: a ped CoopIII built for somebody else must not drop
	// their weapons on our pavement even while the connection is going down.
	if (replicaOfSomebodyElses)
		return DropDecision::REFUSE;
	if (haveSession && charCreatedBy == 1 /* CHAR_CREATED_BY_RANDOM */)
		return DropDecision::ANNOUNCE;
	return DropDecision::LOCAL;
}

// ---------------------------------------------------------------------------
// The per-slot state CoopIII keeps beside the engine's array
// ---------------------------------------------------------------------------

enum class PickupGate : uint8_t {
	// Default. m_pObject is stashed away for the duration of
	// CPickups::Update, so the engine never sees the pickup at all.
	BLOCKED,
	// A claim is out and no answer has come back. Still blocked.
	CLAIMED,
	// The server has reserved it for us. Unblocked, so the engine may take
	// it - and the engine is the one that decides whether it does. Held
	// until the engine takes it (reported as a collection) or the player
	// walks out of the claim radius (reported as a release).
	GRANTED,
};

struct PickupSlot {
	PickupGate gate = PickupGate::BLOCKED;

	// The ident the claim went out with, so a reply can be matched back to a
	// slot even if the slot has been recycled underneath us. Matched by
	// SameIdent, never by slot number.
	PickupIdent ident{};

	// Frame the claim went out on, so a claim that is never answered - a
	// dropped connection, a server that has forgotten us - gets retried
	// instead of leaving the slot blocked forever. For a grant, the frame it
	// arrived on.
	uint32_t claimedFrame = 0;

	// ---- the skull (game/rampagevote.h) ----
	// No claim before this frame. Set when a claim on a skull is turned down,
	// so a failed vote isn't opened again while the player is still standing
	// on it.
	uint32_t retryFrame = 0;
	// The grant came out of a vote that passed. Taken even if the player has
	// walked off it, and never given back for walking away.
	bool     voted = false;
};

// A claim on a skull goes out on the touch, not at 4 m: it opens a vote
// (protocol.h, PICKUP_F_RAMPAGE), and walking past one must not. The touch is
// CPickup::Update's own - on foot, |dz| < 2.0 and dx * dx + dy * dy < 1.8 -
// and the skull's own gate with it (addresses.h, "the skull's own gate").
constexpr bool SkullTouched(bool inVehicle, float dx, float dy, float dz) {
	return !inVehicle && (dz < 0.0f ? -dz : dz) < 2.0f && dx * dx + dy * dy < 1.8f;
}

// How long a skull claim may wait: a vote runs 15 s, and nothing comes back
// until it ends.
constexpr uint32_t kSkullClaimTimeoutFrames = 20 * 60;
// How long after a no before the same skull can be asked for again.
constexpr uint32_t kSkullRetryFrames = 4 * 60;
// How long the engine gets to take a voted skull itself before CoopIII takes
// it for it. CPickups::Update visits the first 320 slots a sixth at a time
// (`imul edi,35h` at 0x00430409), so a slot can wait six frames for its turn.
constexpr uint32_t kSkullEngineGraceFrames = 8;

// Pickups CoopIII removed on somebody else's behalf that the engine will
// never bring back by itself, and that the script may therefore re-create.
//
// Kept as a list of *idents* rather than as a flag on the slot, and that is
// the point of it: `CPickups::GenerateNewOne` hands out the first free slot,
// so a re-created pickup almost never lands in the one it came out of. A
// per-slot flag would have missed every case it exists for.
//
// Only the types with no respawn window go in here - the rest expire on the
// server's own clock and need nothing said about them. Sixteen is generous:
// the entries that live longest are hidden packages and rampages, and the
// script re-creates a rampage within a frame or two of destroying it.
constexpr size_t kRemovedRingSize = 16;

// How many frames a live pickup has to match a removed ident before the
// release goes out. Zero would be fine; one frame of hysteresis costs nothing
// and keeps a single stray read of a half-written table from talking.
constexpr uint32_t kRecreateConfirmFrames = 2;


// How many frames a claim may go unanswered before it is made again.
// CH_EVENT is reliable and ordered, so this only fires on a real fault; it
// exists so the failure is a retry and a log line rather than a pickup that
// silently stops working.
constexpr uint32_t kClaimTimeoutFrames = 180;   // ~3 s at 60 fps

// ---------------------------------------------------------------------------
// What the client half has to provide
// ---------------------------------------------------------------------------
//
// Same shape as the rest of the engine seam: pickup.cpp knows the engine and
// nothing about the wire, Client knows the wire and nothing about the engine.
struct PickupCallbacks {
	// Ask the session to claim this pickup. Called at most once per slot per
	// claim, from the game thread.
	void (*Claim)(const PickupIdent &ident) = nullptr;
	// Tell the session a key is free again - a grant we could not consume, a
	// reservation the player walked away from, or a key the script has
	// re-created. docs/pickups.md 7.
	void (*Release)(const PickupIdent &ident) = nullptr;
	// The engine took it. Detected, not decided: the seam holds the
	// reservation, the engine's own award switch runs, and the slot going
	// empty underneath is what this reports.
	void (*Collected)(const PickupIdent &ident) = nullptr;

	// A pedestrian this machine owns just died and its drop landed here.
	// Called once per pickup, from inside CPed::SetDead, on the game thread.
	// Detected the same way a collection is: the pickup table is snapshotted
	// around the engine's own creator and what appeared is what is reported.
	//
	// Returns whether it actually went out. Being wired is not the same as
	// being connected - the callbacks are set once at boot and the client
	// reconnects for as long as the game runs - and a seam that says "the
	// session was told" while the socket is down is the same lie as a seam
	// that says nothing when it works. This one says which.
	bool (*Dropped)(const PickupDropBody &drop) = nullptr;

	// Is this ped one CoopIII built as somebody else's? The roster knows and
	// the engine does not, so the answer has to come from the client half:
	// a remote player's ped and an ambient replica are both CCivilianPed,
	// both MISSION_CHAR and indistinguishable from a script ped by anything
	// in the pool.
	//
	// Takes the engine's own reference from CPools::GetPedRef rather than a
	// pointer, because that is what RemotePlayer::poolHandle and
	// RemoteAmbientPed::poolHandle hold - and because a pool slot is reused
	// the moment it is freed, so a raw pointer would also match a brand new
	// ped standing in a dead one's slot. This side does the conversion; the
	// client half compares two integers and knows nothing about the engine.
	//
	// Null means there is no session to ask, and then nothing is suppressed
	// and nothing is announced - single player behaves exactly as it always
	// did, which is the same rule the CPickups::Update detour follows.
	bool (*IsReplicatedPed)(int32_t pedRef) = nullptr;

	// Is there actually a session right now?
	//
	// **Being wired is not being connected**, and the difference was a real
	// bug found by watching a live log rather than by reading this file. The
	// callbacks are set once at boot and the client reconnects for as long as
	// the game runs, so every check below that asked "is the seam wired"
	// was answering "yes" while the socket was down - which meant
	// CPickups::Update blocked every pickup in the world and claimed into a
	// dead socket, and a single player who never started a server could not
	// pick anything up at all. docs/roadmap.md 5.6 says that must not
	// happen. The log said it plainly and nobody had read it:
	//
	//   pickup: a claim for model 1361 went unanswered for 180 frames
	//
	// Null is treated as "yes", so a caller that does not wire it keeps the
	// old behaviour rather than silently turning the seam off.
	bool (*HaveSession)() = nullptr;
};

void SetPickupCallbacks(const PickupCallbacks &callbacks);

// ---------------------------------------------------------------------------
// Installation
// ---------------------------------------------------------------------------

// Detours CPickups::Update (0x004303D0). One detour covers everything: a
// whole-image scan for rel32 targets equal to CPickup::Update found exactly
// two call sites and both are inside that function, so there is no other way
// to collect a pickup in this build.
//
// Must not be called from DllMain - docs/compat.md 2.2, same rule as every
// other hook here.
bool InstallPickupHook();
void RemovePickupHook();
bool PickupHookInstalled();

// Detours CPed::CreateDeadPedMoney (0x00433490) and
// CPed::CreateDeadPedWeaponPickups (0x00433660) - the two functions
// CPed::SetDead calls for any ped that is not the local player, and the only
// pickups in the game that main.scm does not create.
//
// A whole-image rel32 scan finds exactly one reference to each and both are
// inside SetDead, so these two detours cover every ped drop there is.
//
// Separate from InstallPickupHook because they are a separate claim: the
// exclusivity seam works without them (it just never sees a drop on more than
// one machine) and they work without it (they would replicate drops nobody
// arbitrates). A failure of either is logged and is not fatal.
bool InstallPedDropHooks();
void RemovePedDropHooks();
bool PedDropHooksInstalled();

// ---------------------------------------------------------------------------
// Inbound
// ---------------------------------------------------------------------------

// The server granted this pickup to us. Unblocks it; the engine takes it on
// the next CPickups::Update that reaches its slot.
//
// Returns false when the pickup is no longer there to take, in which case it
// has already called Release - CoopIII never sits on a grant it cannot use.
bool OnPickupGrantedToUs(const PickupIdent &ident);

// Somebody else got it. Replays the engine's own removal on our copy, and
// pushes the collection into CPickups::aPickUpsCollected so this machine's
// own rampage and reward scripts see it - which is the whole mechanism
// behind the shared decisions in docs/pickups.md 6.
//
// A hidden package is the one reward an observer applies, and the ident's own
// `type` says so (PICKUP_COLLECTABLE1) without anybody having to look up a
// model index. Sharing it is one increment, because every machine is running
// packages.sc and rewards.sc already - roadmap.md 5.11.
void OnPickupTakenByOther(const PickupIdent &ident);

// Our claim lost. Ends the claim; the slot stays blocked.
void OnPickupDenied(const PickupIdent &ident);

// Somebody else's pedestrian dropped this. Builds the same pickup here, with
// the numbers the owner's engine actually wrote into its own table, through
// the engine's own CPickups::GenerateNewOne - so from the next frame on it is
// an ordinary pickup and goes through the ordinary claim/grant/collect
// exchange like any of the 448 the script makes.
//
// Idempotent: a drop whose ident already names a live pickup here is dropped
// on the floor rather than made twice.
void OnPickupDroppedElsewhere(const PickupDropBody &drop);

// Everything CoopIII has done to the pickup table, undone. Called on
// disconnect: with no session there is nobody to arbitrate, so every pickup
// goes back to being the local engine's own business and single player
// behaves exactly as it always did.
void ReleaseAllPickups();

// ---------------------------------------------------------------------------
// Diagnostics
// ---------------------------------------------------------------------------

struct PickupStats {
	uint32_t claimsSent      = 0;
	uint32_t grants          = 0;
	uint32_t grantsUnusable  = 0;   // granted, and the object had gone
	uint32_t denials         = 0;
	uint32_t remoteRemovals  = 0;
	uint32_t releases        = 0;
	uint32_t collected       = 0;   // grants the engine actually consumed
	uint32_t walkedAway      = 0;   // reservations given back unspent
	uint32_t identCollisions = 0;   // two live pickups with the same ident
	uint32_t claimTimeouts   = 0;
	uint32_t blockedPerPass  = 0;   // last pass, for a sanity read in the log

	// ---- ped drops --------------------------------------------------------
	uint32_t dropsMade       = 0;   // pickups our own peds left and announced
	uint32_t dropsUnsent     = 0;   // made, with no session to tell
	uint32_t dropsSuppressed = 0;   // creators refused on a ped we do not own
	uint32_t dropsReceived   = 0;   // somebody else's, built here
	uint32_t dropsDuplicate  = 0;   // already had one at that ident
	uint32_t dropsLost       = 0;   // the pickup table had no room
};

const PickupStats &GetPickupStats();

// Adds the three inbound functions to a bridge that has already been built.
// Called from dllmain, the same way worldstate.h and seat.h do it, so this
// stays independent of ped.cpp's MakeWorldBridge.
void AddPickupsToBridge(WorldBridge &bridge);

} // namespace coopiii::game
