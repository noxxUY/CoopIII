// Guarding CWorld's own lists against the entities CoopIII puts in them, and
// against the engine limit CoopIII can drive a clump past.
//
// CoopIII creates and destroys entities the engine did not ask for, at a rate
// it never would, and GTA III has now faulted at 0x004B1B25 three times -
// inside CWorld::Process, on the first of the four walks it makes over
// CWorld::ms_listMovingEntityPtrs (0x008F433C):
//
//     004B1B20  mov ebp,[edi]        node->item
//     004B1B22  mov edi,[edi+8]      node->next
//     004B1B25  mov eax,[ebp+4Ch]    entity->m_rwObject    <-- fault
//
// This is the seatbelt. It sweeps the list the engine is about to walk and
// takes out anything the engine would fault on.
//
// **Where it runs is the whole point of this file.** It used to run from
// PreFrame, i.e. before CGame::Process. It never once fired, in any session,
// including the one whose crash dump had node->item == 0 - a value
// LooksLikeGameObject(0) rejects on sight. A sweep that cannot see the thing
// it is looking for is not evidence that the thing is absent, it is evidence
// that it is looking in the wrong place: everything CGame::Process does
// before CWorld::Process - CTheScripts::Process, CPopulation, CCarCtrl, the
// fire and explosion managers - happens in the gap. So the sweep is now a
// detour on CWorld::Process itself (0x004B1A60) and runs on entry, with
// nothing between it and the dereference. That closes the window rather than
// narrowing it.
//
// The second half is the bound the engine does not check. Walk 1 is the walk
// that calls RpAnimBlendClumpUpdateAnimations (0x004024B0), and that function
// indexes a twelve-slot node array with no bound at all: past eleven
// associations on one clump it writes over its own saved registers and then
// hands CWorld::Process's walk back a node cursor made of animation data.
// addresses.h has the stack map and the arithmetic. So every entity the
// sweep passes is also handed to an inspector, and the client's inspector is
// the one that keeps a clump under that limit - any clump, not just the ones
// CoopIII drives, because CoopIII is not the only thing that adds to them.
//
// The walk itself is in game/movinglist.h, header-only and parameterised on
// the list head, so tools/clienttest runs the real splice code rather than a
// copy of it. The predicates it uses (LooksLikeGameObject,
// MovingListNodeIsSane) are in addresses.h and covered there too.
#pragma once

#include <cstdint>

#include "movinglist.h"

namespace coopiii::game {

// Walk CWorld::ms_listMovingEntityPtrs and unlink anything CWorld::Process
// would fault on, without logging. The entity inspector is not run.
MovingListAudit AuditMovingList();

// The same audit with the entity inspector, the logging and the
// once-per-occurrence rate limit. Returns how many nodes it took out.
uint32_t GuardMovingList();

// What every surviving entity gets handed to. Set once at startup; null
// means the sweep only checks links. It lives behind a setter so this file
// keeps no dependency on game/ped.cpp - which in turn is what lets
// tools/clienttest link the walk.
void SetMovingListEntityInspector(EntityInspectFn fn);

// Detour CWorld::Process (0x004B1A60) and sweep on entry. This is the one
// that matters; GuardMovingList from PreFrame is the fallback for when this
// fails to install.
bool InstallWorldProcessGuard();
void RemoveWorldProcessGuard();
bool WorldProcessGuardInstalled();

// Microseconds the last sweep took, and the worst one seen. Read by the
// frame heartbeat so the cost of this is a number in the log rather than an
// opinion in a comment.
uint32_t LastSweepMicros();
uint32_t WorstSweepMicros();
uint32_t LastSweepNodes();

} // namespace coopiii::game
