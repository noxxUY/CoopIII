#include "world.h"

#include "addresses.h"
#include "../log.h"

namespace coopiii::game {

namespace {

// A bound on the walk, not a claim about the game. The list holds every
// physical the engine is simulating, which in practice is a few hundred. The
// number matters only because a corrupted `next` chain can be circular, and
// an infinite loop on the game thread is harder to diagnose than the crash
// this function exists to prevent.
constexpr uint32_t MAX_NODES = 8192;

// Whether anything has been reported yet, so a list that has gone wrong
// produces one log line and not sixty a second.
bool g_reported = false;

// Everything that touches untrusted memory lives in here, with no C++ object
// that needs unwinding, so the structured exception handler is legal and the
// audit cannot itself become the crash.
MovingListAudit Walk() {
	MovingListAudit out;
#ifdef _MSC_VER
	__try {
#endif
		uintptr_t *const head = reinterpret_cast<uintptr_t *>(
		    CWorld__ms_listMovingEntityPtrs);
		uintptr_t node = *head;
		uintptr_t prev = 0;

		while (node != 0) {
			if (out.walked >= MAX_NODES) {
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

} // namespace

MovingListAudit AuditMovingList() { return Walk(); }

uint32_t GuardMovingList() {
	const MovingListAudit audit = Walk();

	if (audit.unlinked == 0 && !audit.faulted && !audit.truncated)
		return 0;

	if (!g_reported) {
		g_reported = true;
		Log("world: CWorld::ms_listMovingEntityPtrs is damaged. %u node(s) walked, "
		    "%u unlinked%s%s",
		    audit.walked, audit.unlinked, audit.faulted ? ", the walk faulted" : "",
		    audit.truncated ? ", and it is longer than any real list" : "");
		if (audit.unlinked != 0)
			Log("world: the first bad node was at %08X holding item %08X, which is "
			    "not a pointer to anything. CWorld::Process would have read "
			    "[item+4Ch] off it and taken the game with it",
			    static_cast<unsigned>(audit.firstBadNode),
			    static_cast<unsigned>(audit.firstBadItem));
		Log("world: the game keeps running. Everything CoopIII owns is listed above "
		    "this line, so whatever was spawned or destroyed just before it is the "
		    "thing to look at");
	}

	return audit.unlinked;
}

} // namespace coopiii::game
