#include "mine.h"

#include "leadcheck.h"
#include "../client.h"
#include "../clock.h"
#include "../log.h"

#include <cstring>

#include <windows.h>

namespace coopiii::game {

namespace {

using AddExplosionFn = bool(__cdecl *)(void *, void *, int, const float *, uint32_t);

// Where our own mines went off since Client last drained them. A handful a
// frame at the very most; the bound only keeps a runaway from growing it.
constexpr uint8_t MAX_QUEUED = 8;
Vec3              g_queued[MAX_QUEUED];
uint8_t           g_queuedCount = 0;

RecentMineBlasts g_recent;

bool g_installed     = false;
bool g_saidOurs      = false;
bool g_saidTheirs    = false;
bool g_saidNoMine    = false;
bool g_saidQueueFull = false;

// What CPickup::Update's mine branch calls instead of AddExplosion. Same
// arguments, same answer: the explosion is the engine's, and all this adds is
// where it was.
bool __cdecl MineExplosion(void *entity, void *culprit, int type, const float *pos,
                           uint32_t lifetime) {
	const bool added =
	    Func<AddExplosionFn>(CExplosion__AddExplosion)(entity, culprit, type, pos, lifetime);
	if (!pos)
		return added;
	const Vec3 at{pos[0], pos[1], pos[2]};
	g_recent.Note(at, WallClock::NowMs());
	if (g_queuedCount < MAX_QUEUED) {
		g_queued[g_queuedCount++] = at;
	} else if (!g_saidQueueFull) {
		g_saidQueueFull = true;
		Log("mine: %u blasts are already waiting to be said; one at %.1f %.1f %.1f is not",
		    static_cast<unsigned>(MAX_QUEUED), at.x, at.y, at.z);
	}
	if (!g_saidOurs) {
		g_saidOurs = true;
		Log("mine: one of our mines went off at %.1f %.1f %.1f", at.x, at.y, at.z);
	}
	return added;
}

bool Redirect(uintptr_t site, uintptr_t from, uintptr_t to) {
	if (!RelCallAt(Ptr<uint8_t>(site), site, from))
		return false;
	DWORD old = 0;
	if (!VirtualProtect(reinterpret_cast<void *>(site), 5, PAGE_EXECUTE_READWRITE, &old))
		return false;
	const int32_t rel = static_cast<int32_t>(to - (site + 5));
	std::memcpy(reinterpret_cast<void *>(site + 1), &rel, sizeof rel);
	VirtualProtect(reinterpret_cast<void *>(site), 5, old, &old);
	FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void *>(site), 5);
	return true;
}

// Our copy of the mine somebody else's went off at `pos`, as its slot, or -1.
// The nearest one inside SameMinePlace, in case two were dropped close.
int FindMine(const Vec3 &pos, Vec3 &where) {
	int   found = -1;
	float best  = 0.0f;
	for (size_t i = 0; i < NUM_PICKUPS; ++i) {
		void *const slot = Ptr<uint8_t>(CPickups__aPickUps + i * SIZEOF_PICKUP);
		if (!IsMinePickup(Field<uint8_t>(slot, offs::PICKUP_TYPE)))
			continue;
		void *const object = Field<void *>(slot, offs::PICKUP_OBJECT);
		if (!object)
			continue;
		const float *p = &Field<float>(object, offs::POSITION);
		const Vec3   at{p[0], p[1], p[2]};
		if (!SameMinePlace(at, pos))
			continue;
		const float dx = at.x - pos.x, dy = at.y - pos.y;
		const float d  = dx * dx + dy * dy;
		if (found < 0 || d < best) {
			found = static_cast<int>(i);
			best  = d;
			where = at;
		}
	}
	return found;
}

// What the mine branch does after its explosion (addresses.h,
// 0x004309F0..0x00430A15): the object out of the world and deleted, and the
// slot freed for good.
void TakeMine(int index) {
	void *const slot   = Ptr<uint8_t>(CPickups__aPickUps + static_cast<size_t>(index) * SIZEOF_PICKUP);
	void *const object = Field<void *>(slot, offs::PICKUP_OBJECT);
	if (object) {
		Func<void(__cdecl *)(void *)>(CWorld__Remove)(object);
		void *const vtable = Field<void *>(object, 0);
		if (vtable)
			reinterpret_cast<void(__thiscall *)(void *, int)>(
			    reinterpret_cast<uintptr_t *>(vtable)[0])(object, 1);
	}
	Field<uint8_t>(slot, offs::PICKUP_REMOVED) = 1;
	Field<void *>(slot, offs::PICKUP_OBJECT)   = nullptr;
	Field<uint8_t>(slot, offs::PICKUP_TYPE)    = PICKUP_NONE;
}

uint8_t DrainLocalMineBlasts(Vec3 *out, uint8_t max) {
	const uint8_t n = g_queuedCount < max ? g_queuedCount : max;
	for (uint8_t i = 0; i < n; ++i)
		out[i] = g_queued[i];
	for (uint8_t i = n; i < g_queuedCount; ++i)
		g_queued[i - n] = g_queued[i];
	g_queuedCount = static_cast<uint8_t>(g_queuedCount - n);
	return n;
}

void ApplyRemoteMineBlast(const Vec3 &pos) {
	const uint32_t now   = WallClock::NowMs();
	Vec3           where = pos;
	const bool     had   = g_recent.Had(pos, now);
	const int      mine  = had ? -1 : FindMine(pos, where);
	switch (PlanMineBlast(had, mine >= 0)) {
	case MineBlastHere::Already:
		return;
	case MineBlastHere::OurMine:
		TakeMine(mine);
		if (!g_saidTheirs) {
			g_saidTheirs = true;
			Log("mine: somebody else's copy of our mine at %.1f %.1f %.1f went off first; "
			    "ours goes with it",
			    where.x, where.y, where.z);
		}
		break;
	case MineBlastHere::NoMine:
		if (!g_saidNoMine) {
			g_saidNoMine = true;
			Log("mine: a mine went off at %.1f %.1f %.1f where we have none; the "
			    "explosion alone",
			    pos.x, pos.y, pos.z);
		}
		break;
	}
	g_recent.Note(where, now);
	const float at[3] = {where.x, where.y, where.z};
	Func<AddExplosionFn>(CExplosion__AddExplosion)(nullptr, nullptr, EXPLOSION_MINE, at, 0);
}

} // namespace

bool InstallMineHooks() {
	g_queuedCount = 0;
	g_recent      = RecentMineBlasts{};
	if (g_installed)
		return true;
	g_installed = Redirect(CPickup__Update_MineExplosion, CExplosion__AddExplosion,
	                       reinterpret_cast<uintptr_t>(&MineExplosion));
	if (g_installed)
		Log("mine: a mine's explosion at 0x%08X comes to us first",
		    static_cast<unsigned>(CPickup__Update_MineExplosion));
	else
		Log("mine: FAILED to redirect the mine's explosion at 0x%08X; it no longer calls "
		    "0x%08X, so a mine goes off on its own machine only",
		    static_cast<unsigned>(CPickup__Update_MineExplosion),
		    static_cast<unsigned>(CExplosion__AddExplosion));
	return g_installed;
}

void RemoveMineHooks() {
	if (g_installed && Redirect(CPickup__Update_MineExplosion,
	                            reinterpret_cast<uintptr_t>(&MineExplosion),
	                            CExplosion__AddExplosion))
		g_installed = false;
}

void AddMinesToBridge(WorldBridge &bridge) {
	// Somebody else's blast is taken with or without the redirect; only
	// saying ours needs it.
	bridge.ApplyRemoteMineBlast = &ApplyRemoteMineBlast;
	if (g_installed)
		bridge.DrainLocalMineBlasts = &DrainLocalMineBlasts;
}

} // namespace coopiii::game
