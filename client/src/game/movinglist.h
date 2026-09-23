// The walk over CWorld::ms_listMovingEntityPtrs, with nothing engine-shaped
// in it.
//
// This used to live inside game/world.cpp, where no test could reach it: the
// only headless coverage was the two predicates it calls. The walk itself -
// the splice arithmetic, the head case, the tail case, the node cap - was
// never run outside the game, and the splice arithmetic is the part that
// writes into the engine's own list. So it is here, header-only and
// parameterised on the list head, and tools/clienttest drives it over a real
// list it builds itself.
//
// Nothing in here reads a game global or calls a game function. The one
// engine-facing thing the sweep also has to do - look at the entity behind a
// node - goes through a function pointer the caller supplies, so this header
// stays linkable into a test binary that has no gta3.exe under it.
#pragma once

#include <cstdint>

#include "addresses.h"

namespace coopiii::game {

struct MovingListAudit {
	uint32_t  walked        = 0;    // nodes visited
	uint32_t  unlinked      = 0;    // nodes taken out this pass
	uint32_t  inspected     = 0;    // nodes handed to the entity inspector
	uintptr_t firstBadItem  = 0;    // the value that was sitting where an
	                                // entity pointer should have been
	uintptr_t firstBadNode  = 0;
	bool      faulted       = false;   // the walk itself hit bad memory
	bool      truncated     = false;   // hit the node cap, so the list loops
};

// Called once for every node whose item survived the sanity test, with that
// entity. Null means "just check the links", which is what the headless
// tests pass and what a build with no inspector registered does.
using EntityInspectFn = void (*)(void *entity);

// A bound on the walk, not a claim about the game. The list holds every
// physical the engine is simulating, which in practice is a few hundred. The
// number matters only because a corrupted `next` chain can be circular, and
// an infinite loop on the game thread is harder to diagnose than the crash
// this function exists to prevent.
constexpr uint32_t MOVING_LIST_MAX_NODES = 8192;

// Walk the list at `head`, unlink anything CWorld::Process would fault on,
// and hand every surviving entity to `onSane`.
//
// A node it takes out is leaked rather than freed. CoopIII does not know
// whose 12 bytes those are any more, and handing a block it cannot identify
// back to CPools::ms_pPtrNodePool is how one bad node becomes two.
//
// Everything that touches untrusted memory is in here, with no C++ object
// that needs unwinding, so the structured exception handler is legal and the
// audit cannot itself become the crash.
inline MovingListAudit SweepMovingList(uintptr_t *head, EntityInspectFn onSane) {
	MovingListAudit out;
#ifdef _MSC_VER
	__try {
#endif
		uintptr_t node = *head;
		uintptr_t prev = 0;

		while (node != 0) {
			if (out.walked >= MOVING_LIST_MAX_NODES) {
				out.truncated = true;
				break;
			}
			++out.walked;

			const uintptr_t item =
			    *reinterpret_cast<uintptr_t *>(node + PTRNODE_ITEM);
			const uintptr_t next =
			    *reinterpret_cast<uintptr_t *>(node + PTRNODE_NEXT);

			// The vtable is only read once the value has been shown to be
			// capable of being a pointer at all. That ordering is the whole
			// point: 0x20E never reaches a dereference.
			const bool sane =
			    LooksLikeGameObject(item) &&
			    IsImageAddress(*reinterpret_cast<uintptr_t *>(item));

			if (sane) {
				if (onSane) {
					++out.inspected;
					onSane(reinterpret_cast<void *>(item));
				}
				prev = node;
				node = next;
				continue;
			}

			// Splice it out. The engine's own CPtrList::DeleteNode writes the
			// same two links; what it also does, and this does not, is give
			// the block back to the pool.
			if (prev != 0)
				*reinterpret_cast<uintptr_t *>(prev + PTRNODE_NEXT) = next;
			else
				*head = next;
			if (next != 0)
				*reinterpret_cast<uintptr_t *>(next + PTRNODE_PREV) = prev;

			if (out.unlinked == 0) {
				out.firstBadItem = item;
				out.firstBadNode = node;
			}
			++out.unlinked;
			node = next;
		}
#ifdef _MSC_VER
	} __except (1 /* EXCEPTION_EXECUTE_HANDLER */) {
		out.faulted = true;
	}
#endif
	return out;
}

} // namespace coopiii::game
