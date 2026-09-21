// Guarding CWorld's own lists against the entities CoopIII puts in them.
//
// CoopIII creates and destroys entities the engine did not ask for, at a rate
// it never would, and it has now crashed GTA III twice on a pointer the
// engine still held. The second one was a node in
// CWorld::ms_listMovingEntityPtrs whose `item` was 0x0000020E, a small
// integer where an entity pointer belongs, dereferenced at 0x004B1B25 by the
// first of the four walks CWorld::Process makes over that list.
//
// This is the seatbelt. It runs before CGame::Process, looks at the list the
// engine is about to walk, and takes out anything the engine would fault on.
// It does not pretend to know why a bad node got there; it makes the failure
// a log line with the evidence in it instead of a crash with none.
//
// The decisions are pure arithmetic in addresses.h (LooksLikeGameObject,
// MovingListNodeIsSane) so tools/clienttest covers them, and the walk itself
// cannot fault: it is wrapped in a structured exception handler, because the
// one thing an audit of untrusted memory must not do is become the crash.
#pragma once

#include <cstdint>

namespace coopiii::game {

struct MovingListAudit {
	uint32_t  walked        = 0;    // nodes visited
	uint32_t  unlinked      = 0;    // nodes taken out this pass
	uintptr_t firstBadItem  = 0;    // the value that was sitting where an
	                                // entity pointer should have been
	uintptr_t firstBadNode  = 0;
	bool      faulted       = false;   // the walk itself hit bad memory
	bool      truncated     = false;   // hit the node cap, so the list loops
};

// Walk CWorld::ms_listMovingEntityPtrs and unlink anything CWorld::Process
// would fault on. Cheap enough for every frame: a few hundred nodes, three
// loads and two compares each.
//
// A node it takes out is leaked rather than freed. CoopIII does not know
// whose 12 bytes those are any more, and handing a block it cannot identify
// back to CPools::ms_pPtrNodePool is how one bad node becomes two.
MovingListAudit AuditMovingList();

// The same audit with the logging and the once-per-occurrence rate limit,
// which is what PreFrame calls. Returns how many nodes it took out.
uint32_t GuardMovingList();

} // namespace coopiii::game
