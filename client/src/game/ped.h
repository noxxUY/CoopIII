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

// While this machine plays a mission's cutscene the session's other players
// are not drawn, or they would stand in the middle of the scene
// (docs/missions.md 11.3). game/mission.cpp says when.
void SetRemotePlayersHidden(bool hidden);

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

// The same for a ped that is not a player's: GiveWeapon and SetCurrentWeapon
// once the weapon's model has streamed in, false until then. For
// population.cpp, which arms the replicas of other machines' pedestrians.
bool PutReplicaWeaponInHand(void *ped, uint8_t weapon);

// The eWeaponType in any ped's hand, read the way SampleLocalPlayer reads the
// local player's: m_currentWeapon bounded before it indexes m_weapons, and
// UNARMED for anything that is not an inventory weapon.
uint8_t HeldWeaponType(void *ped);

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

// The fire a burning player gets on every screen but their own (ped.cpp,
// LightRemoteFire): the tail of CFireManager::StartFire with the NPC arm left
// out, so it moves nothing and, on a bFireProof ped, costs nothing. For
// population.cpp, which puts the same fire on a burning pedestrian's replica.
// Returns the CFire, or nil when all 40 slots are taken.
void *LightWatchedPedFire(void *ped);

// Is `fire` the one this machine lit in slot `fireSlot`, still alight and
// still on `ped`? A slot is re-let the moment its fire goes out, so the index
// alone is not an identity.
bool WatchedPedFireIsOurs(int8_t fireSlot, void *ped, void *fire);

// Where the engine has a remote player's ped, for a desync probe. False with no
// ped, or while the engine is seating it.
bool SampleRemotePedPosition(const RemotePlayer &player, Vec3 &out);

// ---- for dllmain.cpp ------------------------------------------------------

// The detour on CPedIK::PointGunInDirection that bends a remote player's arms
// to their owner's pitch and reads our own aim for the snapshot. Not fatal if
// it fails: remote players aim level, as they did before it existed.
bool InstallAimPitchHook();
void RemoveAimPitchHook();

// A call-site redirect on CPed::ProcessControl's call to CalculateNewVelocity
// that turns a remote player's upper legs toward a diagonal walk, the way the
// engine only does for the local player (remoteloco.h, LegTwistFor). Not
// fatal if it fails: the legs run straight ahead, as they did before.
bool InstallLegTwist();
void RemoveLegTwist();

// The detour on CStreaming::RequestSpecialModel that lets a remote Claude
// wear his own clothes (game/look.h). Without it every remote Claude is
// built from our model 0, in whatever we're wearing, as before.
bool InstallLookHook();
void RemoveLookHook();

// Model 0's name, which is the clothes the local player has on, or null
// before the model info is there.
const char *LocalPlayerModelName();

// Whether gta3.img has a model of this name for RequestSpecialModel to load.
// It doesn't check itself (addresses.h, CStreaming__ms_pExtraObjectsDir).
bool LookInGameImage(const char *look);

// How many remote players' peds are built from this model: the ones
// HookedRequestSpecialModel takes down itself before a script renames it.
uint16_t RemotePlayerPedsBuiltFrom(int32_t modelId);

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

// The talk or wait-state overlay (chat, hail a taxi, hands up, cower, duck) on
// a ped's clump, or ANIM_NONE. What C_PedOverlay says; the whole-body animId
// above cannot, these are ASSOC_PARTIAL.
uint16_t ReadPedOverlayAnim(void *ped);

// Plays that overlay on a replica, or fades it for ANIM_NONE; `applied` is what
// this machine last put there. Safe to call every frame. False when it could
// not be played (no room on the clump, an id the animations do not have).
bool ApplyReplicaOverlay(void *ped, uint16_t animId, uint16_t &applied);

// Should the replica's base animation be blended this frame: the id changed,
// or it is one of the four looping locomotion ids (walk, run, sprint, idle)
// and nothing with it is live on the clump any more. Our engine takes a
// replica's walk off on its own - the first CPed::SetMoveAnim after a spawn
// blends the idle over it, and a get-up or a landing does the same - and a
// test on the id alone never puts it back, which leaves the replica sliding
// along the street on its idle (docs/protocol.md 1.54.2).
bool ReplicaAnimNeedsBlend(void *ped, uint16_t want, uint16_t applied);

// A copy in a car seat, a remote player's or a pedestrian replica's, plays
// the seat and nothing else (animrevive.h, SeatedPoseForbids): a knockdown,
// get-up, fall or walk found on it is taken off, a fall or get-up state our
// engine started is put back to PED_DRIVING, and the sitting animation is put
// back on the way PedSetInCarCB puts it. True when the engine has the ped in
// a vehicle at all, which is when nothing else may animate or place it.
bool KeepSeatedPose(void *ped);

// CPed::SetObjective then CPed::WarpPedIntoCar, in that order, because
// WarpPedIntoCar branches on m_objective and silently assigns no seat at all
// given anything else. Into `seat` when seatplan.h gives it, another free
// passenger seat when it does not. The seat he is in, AMBIENT_SEAT_NONE_FREE
// when there was none to give and nothing was touched, or
// AMBIENT_SEAT_REFUSED when the warp did not take and he has already been put
// back on foot.
int SeatReplicaPed(void *ped, void *car, uint8_t seat);

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

// What the local player's engine is doing about a car right now, so the other
// machines can play the same entry at the same time instead of being told
// about it once it is over. WorldBridge::SampleLocalCarEntry.
bool    SampleLocalCarEntry(LocalCarEntry &out);

// Take an unfinished entry off a ped and fade what it blended. Returns how
// many partial animations went, for the caller to report.
int     CancelCarEntry(void *ped);

// Give back the door a ped claimed for getting out, when it is being taken out
// of the car by hand rather than by the end of its own exit. Call before the
// ped's state is overwritten; does nothing unless it was mid-exit. addresses.h,
// GettingOutFlagsAfterUnseat.
bool    ReleaseExitDoor(void *ped, void *car);

// Changes while an entry's animation is playing, for EntryWatch (client.h).
uint32_t CarEntryMark(void *ped);

// Whether `assoc` is one of this clump's animations right now.
bool    ClumpHoldsAnim(void *clump, void *assoc);

// Take m_pVehicleAnim off a ped: faded out if the clump still holds it, only
// forgotten if it does not, since then it is freed memory. What it was.
constexpr int VEHICLE_ANIM_NONE     = 0;
constexpr int VEHICLE_ANIM_FADED    = 1;
constexpr int VEHICLE_ANIM_DANGLING = 2;
int     ForgetVehicleAnim(void *ped);

// Take the callbacks off a ped's car animations before CoopIII seats him,
// unseats him or nils his m_pMyVehicle by hand, and fade those animations out
// (game/animcb.h). A rolling door close still to finish is finished now, on
// the car it was for. Call while m_pMyVehicle still names the car he was in.
// How many there were.
int     DropCarChainCallbacks(void *ped);

// What every seat CoopIII changes by hand calls first: a ped halfway through
// a door has the entry ended the engine's way (CancelCarEntry, whose
// QuitEnteringCar gives the car its door and count back, then the above);
// anybody else just has the callbacks dropped.
int     LetGoOfCarChain(void *ped);

// The same for every ped whose m_pMyVehicle is `vehicle`, which is about to
// be deleted: the engine's reference will nil that pointer under them.
int     DropCarChainCallbacksOnCar(void *vehicle);

// Every association in the ped pool whose callback is `from` gets `to`, for
// taking a guard wrapper away while an animation still names it.
int     RepointAnimCallbacks(uintptr_t from, uintptr_t to);

// The ped half of COMMAND_WARP_CHAR_FROM_CAR_TO_COORD's handler, with no
// teleport and no car: on foot, idle, colliding, no objective, no car
// animation, standing in his idle animation. `leftASeat` puts the weapon
// back in his hand and the in-car animations off, for a ped who was sitting.
// The car is the caller's; this does not touch it.
void    PutPedOnFoot(void *ped, bool leftASeat);

// An ENTER_CAR objective taken off a ped who is walking to the door, and the
// walk ended where he stands.
void    ClearEnterObjective(void *ped);

// Whether our engine is taking this ped out of its seat through a jack played
// here, as a PullOut (client.h). For population.cpp's traffic drivers.
uint8_t PullOutOf(void *ped);

// SampleLocalCarEntry's reading of any ped: the car he is opening a door of,
// the seat it ends in and the door, as a seat. For population.cpp, which says
// it of the pedestrians this machine hosts (AMBIENT_PED_ENTERING).
bool    SamplePedCarEntry(void *ped, LocalCarEntry &out);

// The door-opening entry and the climb out, on a copy of somebody else's
// pedestrian, because his host's is doing it. The entry only goes for an
// empty seat (seatplan.h, DoorEntryMayTake); PollCarEntry and CancelCarEntry
// above follow it as they do the local player's. False when the engine would
// not start it, and nothing changed.
bool    StartReplicaCarEntry(void *ped, void *car, uint8_t seat, uint8_t doorSeat);
bool    StartCarExit(void *ped);

// The wire seat a ped holds in a car, 0 for the wheel, -1 for none.
int     WireSeatOf(void *car, void *ped);

// Bit s for each wire seat a warp may give in this car: nobody in it and
// nobody climbing in by its door (seatplan.h, FreeSeatMask).
uint16_t FreeSeatsForWarp(void *car);

// A sitting passenger along to another free passenger seat of the same car.
// The wire seat he is in afterwards.
int     MovePassengerToSeat(void *car, void *ped, uint8_t seat);

// A player's copy into the passenger seat the session gives him, from another
// one of the same car he was put in when it was taken (seatplan.h,
// PlanSettle). True when he moved.
bool    SettleRemoteSeat(RemotePlayer &player, RemoteVehicle &vehicle, uint8_t seat);

} // namespace coopiii::game
