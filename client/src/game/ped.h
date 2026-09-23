// Reading and writing CPeds - the engine side of WorldBridge.
//
// Everything here dereferences raw game pointers using the offsets in
// addresses.h. That makes it the most dangerous file in the project, and
// the one that can least afford to guess: a field offset off by four
// doesn't crash, it writes a float into a neighbouring member and produces
// a bug that looks exactly like bad netcode.
//
// So nothing here uses an offset that hasn't been confirmed against the
// disassembly. Where a capability needs an address we don't have yet, the
// function is simply absent rather than approximated - see SpawnRemotePlayer.
#pragma once

#include "client.h"

#include <coopiii/protocol.h>

namespace coopiii::game {

// Fills the WorldBridge with the functions below. Whatever is not yet
// implemented is left null, which Client handles (tools/clienttest covers it).
WorldBridge MakeWorldBridge();

// Reads the local player. False when there is no player ped: menus, loading,
// between a death and a respawn.
bool SampleLocalPlayer(PlayerStateBody &out);

// True if a player ped exists right now.
bool LocalPlayerExists();

// ---- for game/combat.cpp --------------------------------------------------

// This player's live CPed, or null when the engine has taken it away. Same
// pool-handle-plus-vtable check every write in ped.cpp goes through - it's
// how a lost ped gets noticed instead of silently written into.
void *ResolveRemotePed(RemotePlayer &player);

// Put `weapon` in the ped's hand if it isn't already there. False means the
// model is still streaming and the caller should try again - which also
// answers "may this player's shot be replayed yet": firing a gun the ped
// isn't holding plays the wrong animation, wrong stance, no model in hand.
bool GiveRemoteWeapon(RemotePlayer &player, void *ped, uint8_t weapon);

// Whether this session reports ammunition honestly (protocol.h,
// SESSION_AMMO_SYNC). Set from S_Welcome; read wherever the engine is about
// to decide how much ammunition a remote ped has.
void SetAmmoSync(bool enabled);
bool AmmoSyncOn();

// Put a remote ped's weapon slot back to what its owner reported, and set
// the weapon state that follows from it. The wire is the authority for
// somebody else's ammunition and the local engine is not, so this is what
// combat.cpp calls after a replayed shot has spent rounds that were never
// spent on the machine that fired them.
//
// Does nothing for a slot the ped has not been given, or a slot number that
// is not a weapon.
void WriteRemoteSlotAmmo(void *ped, uint8_t slot, uint16_t clip, uint32_t total);

// Is this CPed one of the remote players, and if so which one?
//
// The reverse of RemotePlayer::poolHandle, for the one caller that gets
// handed a raw pointer by the engine and has to decide, right there, whether
// the local machine is allowed to hurt it. Answers through the pool
// reference rather than by comparing pointers, so a recycled slot says no.
bool RemotePlayerForPed(const void *ped, uint16_t &netId);

// How many animations ASSOCGRP_STD actually holds in this build, or 0 before
// the anim files have loaded. The bound every id off the wire gets measured
// against, read from the engine rather than taken from re3 - the retail 1.0
// table is one entry shorter than re3's enum (addresses.h, ANIM_STD_NUM).
int32_t StdAnimGroupCount();

// ---- for game/world.cpp ---------------------------------------------------

// Keep one clump inside the bound RpAnimBlendClumpUpdateAnimations does not
// check. Registered as the moving-list sweep's entity inspector, so it runs
// for every entity CWorld::Process is about to hand to that function - not
// only for the peds CoopIII has a roster entry for, because a ped CoopIII
// merely shot, seated, dismembered or replicated is a ped CoopIII changed.
// Does nothing to a clump that is inside the bound, which is all of them in
// an ordinary frame.
void ClampClumpAnimations(void *entity);

// ---- for game/population.cpp ----------------------------------------------
//
// The four things a replicated *ambient* ped needs that a replicated player
// already had. Exported rather than duplicated: each of them is a piece of
// engine knowledge this project paid for once, and a second copy beside a
// different roster type is a second place to get it wrong.

// Put a ped where it is not moving itself: matrix, RenderWare frame, and the
// sector grid. Writing the position alone is what made the first two remote
// players invisible - the long version is on PlaceRemotePed in ped.cpp.
// `inWorld` is false for a ped that has not been through CWorld::Add yet.
void PlaceReplicaPed(void *ped, const Vec3 &pos, float heading, bool inWorld);

// The dominant whole-body animation on a ped's clump, or ANIM_NONE. Highest
// blendAmount wins, the same rule RpAnimBlendClumpGetMainAssociation uses.
// This is the one field an ambient ped's stream carries beyond its transform,
// and §1.13.4 is why it has to be the animation and not the move state.
uint16_t ReadPedBaseAnim(void *ped);

// Blend one animation onto a replica's clump through CAnimManager, with the
// anim-group bound of docs/protocol.md §1.8.1 applied. This, and not
// m_nMoveState, is what makes a non-player ped walk (§1.13.4).
bool BlendReplicaAnim(void *ped, uint16_t animId);

// CPed::SetObjective then CPed::WarpPedIntoCar, in that order, because
// WarpPedIntoCar branches on m_objective and silently assigns no seat at all
// given anything else. False means the warp did not take, and the ped has
// already been put back on foot.
bool SeatReplicaPed(void *ped, void *car, uint8_t seat);

// The other direction, and also a plain "make sure this ped is on foot". It
// clears the objective as well as the seat: a CCivilianPed left holding
// ENTER_CAR_AS_DRIVER and a car pointer walks back to the car and tries to
// get in again, fighting whatever is positioning it.
void UnseatReplicaPed(void *ped);

// The engine's own animated way into a car, exposed for the local player.
//
// The replica path has walked a remote ped to the door and opened it since
// entercar landed. game/seat.cpp had no way to reach that, so the seat key put
// the local player in with CPed::WarpPedIntoCar - instant, and instant is what
// both screens showed. These three are the same calls the replica path makes,
// nothing more: the state around them, and the fallback when the engine says
// no, belong to whoever is driving them.
bool    StartCarEntry(void *ped, void *car, uint8_t seat);
uint8_t PollCarEntry(void *ped, void *car, uint8_t seat);

// Take an unfinished entry off a ped and fade what it blended. Returns how
// many partial animations went, for the caller to report.
int     CancelCarEntry(void *ped);

} // namespace coopiii::game
