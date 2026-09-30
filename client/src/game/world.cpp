#include "world.h"

#include "addresses.h"
#include "hook/hook.h"
#include "../log.h"

#ifdef _WIN32
#include <windows.h>
#endif

namespace coopiii::game {

namespace {

// Whether anything has been reported yet, so a list that has gone wrong
// produces one log line and not sixty a second.
bool g_reported = false;

EntityInspectFn g_inspect = nullptr;

Detour g_process;

// Cost, measured rather than asserted. The sweep runs once per frame on the
// game thread, so "a few hundred nodes is cheap" has to be a number.
uint32_t g_lastMicros  = 0;
uint32_t g_worstMicros = 0;
uint32_t g_lastNodes   = 0;

uintptr_t *ListHead() {
	return reinterpret_cast<uintptr_t *>(CWorld__ms_listMovingEntityPtrs);
}

// CWorld::Process is a static member, so __cdecl with no arguments. Getting
// this wrong corrupts the stack every frame.
using ProcessFn = void(__cdecl *)();

void Report(const MovingListAudit &audit) {
	if (audit.unlinked == 0 && !audit.faulted && !audit.truncated)
		return;
	if (g_reported)
		return;

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

// One sweep, timed. Separate from the detour body so the fallback path in
// PreFrame gets the same measurement.
MovingListAudit Sweep(EntityInspectFn inspect) {
#ifdef _WIN32
	LARGE_INTEGER freq{}, a{}, b{};
	QueryPerformanceFrequency(&freq);
	QueryPerformanceCounter(&a);
#endif
	const MovingListAudit audit = SweepMovingList(ListHead(), inspect);
#ifdef _WIN32
	QueryPerformanceCounter(&b);
	if (freq.QuadPart > 0) {
		const long long us =
		    ((b.QuadPart - a.QuadPart) * 1000000LL) / freq.QuadPart;
		g_lastMicros = static_cast<uint32_t>(us < 0 ? 0 : us);
		if (g_lastMicros > g_worstMicros)
			g_worstMicros = g_lastMicros;
	}
#endif
	g_lastNodes = audit.walked;
	return audit;
}

// The whole reason this file grew a detour. Nothing runs between this and
// `mov ebp,[edi]` at 0x004B1B20.
void __cdecl HookedWorldProcess() {
	Report(Sweep(g_inspect));
	g_process.Original<ProcessFn>()();
}

} // namespace

MovingListAudit AuditMovingList() {
	return SweepMovingList(ListHead(), nullptr);
}

uint32_t GuardMovingList() {
	const MovingListAudit audit = Sweep(g_inspect);
	Report(audit);
	return audit.unlinked;
}

void SetMovingListEntityInspector(EntityInspectFn fn) { g_inspect = fn; }

bool InstallWorldProcessGuard() {
	if (g_process.IsInstalled())
		return true;

	if (!g_process.Install("CWorld::Process",
	                       reinterpret_cast<void *>(CWorld__Process),
	                       reinterpret_cast<void *>(&HookedWorldProcess))) {
		Log("world: FAILED to hook CWorld::Process at 0x%08X; the moving-list "
		    "sweep falls back to PreFrame, which cannot see a node created "
		    "after it runs", CWorld__Process);
		for (const auto &f : HookFailures())
			Log("world:   %s: %s", f.name.c_str(), f.reason.c_str());
		return false;
	}

	Log("world: hooked CWorld::Process at 0x%08X; the moving list is swept on "
	    "entry, with nothing between the sweep and 0x004B1B25",
	    CWorld__Process);
	return true;
}

void RemoveWorldProcessGuard() {
	if (!g_process.IsInstalled())
		return;
	g_process.Remove();
	Log("world: CWorld::Process hook removed; worst sweep was %u us over %u "
	    "node(s)", g_worstMicros, g_lastNodes);
}

bool WorldProcessGuardInstalled() { return g_process.IsInstalled(); }

uint32_t LastSweepMicros() { return g_lastMicros; }
uint32_t WorstSweepMicros() { return g_worstMicros; }
uint32_t LastSweepNodes() { return g_lastNodes; }

} // namespace coopiii::game
