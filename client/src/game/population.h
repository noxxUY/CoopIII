// The ambient population seam: one door in, one door out.
//
// docs/population.md §1.1. `CWorld::Add` is the single function every entity
// walks through on its way into the world, whoever created it and for
// whatever reason - the pedestrian generator, the traffic generator, a script
// spawn, a car falling out of a crane. Hooking it means all of them register
// the same way, with no list of creation paths to keep up to date and nothing
// to forget when the engine takes a path this project has never seen.
//
// What this file does *not* do is decide anything. It watches entities
// arrive, tells the client which of them are ambient pedestrians this machine
// now hosts, and builds replicas of the ones other machines host. The
// engine's own generator is untouched: population.md §3 step 3 is the part
// that tells it how crowded the street really is, and until that lands this
// seam doubles the crowd rather than sharing it.
//
// ## The asymmetry, which is the sharp edge here
//
// `CWorld::Add` and `CWorld::Remove` are not a matching pair. Both are gated
// on `bIsStatic` (addresses.h, the moving-list block), which is what once
// left a freed node in `ms_listMovingEntityPtrs` and crashed the game. So
// nothing in here assumes a Remove for every Add: the Remove hook is a fast
// path and the authority is a sweep over the pool handles, which notices a
// ped that went away through any route at all, including no route.
#pragma once

#include "../client.h"

#include <cstdint>

namespace coopiii::game {

// Installs the CWorld::Add / CWorld::Remove detours. Safe to call twice.
// A failure is recorded through the usual hook diagnostics and is not fatal:
// with no hooks, CoopIII simply never notices an ambient ped, which is where
// this project was before any of it existed.
bool InstallPopulationHooks();
void RemovePopulationHooks();

// Fills the ambient-population half of the bridge. Additive, the same shape
// as AddWorldToBridge and AddSeatToBridge.
void AddPopulationToBridge(WorldBridge &bridge);

// The local engine has just killed a pedestrian. Queued for the session if it
// is one this machine hosts and the session already has a name for it, and
// ignored otherwise - a replica, or a ped still inside the round trip of its
// own naming.
//
// Called from game/combat.cpp, which owns the CPed::SetDie detour: one
// address carries one hook, and that hook has captured the local player's
// death animation since M4. The caller has already established that this
// particular call actually killed the ped - SetDie returns without doing
// anything for one that was already dying. The filter this side is about
// entitlement, not about whether a death happened.
void NoteHostedPedDeath(void *ped, uint16_t animId);

// How many ambient peds this machine is currently hosting, for diagnostics.
uint32_t HostedAmbientPedCount();

// How many ambient traffic cars this machine is hosting, and how many
// replicas of other machines' cars it is holding. Both for the crowd
// measurement (docs/population.md §1.3.1 and §3 step 4) rather than for
// anything that decides.
uint32_t HostedAmbientCarCount();
uint32_t AmbientCarReplicaCount();

// While one of these is alive, the CWorld::Add and CWorld::Remove detours
// ignore whatever walks through them.
//
// Exists because CoopIII calls both functions itself, to put replicas into
// the world and take them out again, and without the suppression the Add
// detour would hand a replica straight back as an entity this machine
// created - announcing somebody else's pedestrian, or somebody else's car,
// to the session as one of ours. The failure is loud (an entity that exists
// twice on every other screen) but only after it has been on the wire.
//
// A plain flag rather than a thread_local: every call CoopIII makes is on the
// game thread inside the frame pump, and the engine has no other thread that
// adds entities.
//
// game/vehicle.cpp uses it too, which is why it is declared here rather than
// kept private to population.cpp.
struct ReplicaScope {
	ReplicaScope();
	~ReplicaScope();
	ReplicaScope(const ReplicaScope &) = delete;
	ReplicaScope &operator=(const ReplicaScope &) = delete;
};

} // namespace coopiii::game
