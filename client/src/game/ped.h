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

} // namespace coopiii::game
