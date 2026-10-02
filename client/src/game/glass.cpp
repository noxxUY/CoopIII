#include "glass.h"

#include "addresses.h"
#include "client.h"
#include "leadcheck.h"
#include "log.h"
#include "object.h"

#include <windows.h>

#include <cstring>

namespace coopiii::game {
namespace {

namespace obj = ::coopiii::game::object;

GlassCallbacks g_cb;
GlassStats     g_stats;

// Set while a shatter somebody else reported is run here. The receiver calls
// the function directly rather than through any of the four redirected calls,
// so nothing below would see it anyway; this is the second lock on the same
// door, because a loop between two machines would never end.
bool g_applying = false;

// CGlass::WindowRespondsToCollision, __cdecl, nine dwords: the window, the
// amount, the speed and the point by value, and the engine's bool pushed as a
// whole dword (`push 0` / `push 1`), which is passed through untouched.
using ShatterFn = void(__cdecl *)(void *window, float amount, float sx, float sy, float sz,
                                  float px, float py, float pz, uint32_t explosion);

struct Site {
	uintptr_t   at;
	const char *what;   // for the log, when it is not taken
	bool        taken = false;
};

Site g_sites[] = {
    {obj::GLASS_COLLISION_PED_CALL, "somebody on foot walking through a window"},
    {obj::GLASS_COLLISION_CALL, "a car driving through a window"},
    {obj::GLASS_BULLET_BREAK_CALL, "a round shattering a cracked window"},
    {obj::GLASS_BLAST_BREAK_CALL, "a blast shattering a window"},
};

uint8_t &ObjectFlags(void *object) { return Field<uint8_t>(object, obj::OBJECT_FLAGS); }

bool IsWindowModel(void *object) {
	int16_t ids[obj::MI_GLASS_COUNT];
	for (size_t i = 0; i < obj::MI_GLASS_COUNT; ++i)
		ids[i] = Global<int16_t>(obj::MI_GLASS_FIRST + i * obj::MI_GLASS_STRIDE);
	return ModelIsGlass(Field<int16_t>(object, offs::MODEL_INDEX), ids, obj::MI_GLASS_COUNT);
}

bool IsMapObject(void *object) {
	return Field<uint8_t>(object, obj::CREATED_BY) == obj::GAME_OBJECT &&
	       (ObjectFlags(object) & obj::OBJ_IS_PICKUP) == 0;
}

// Where the map put it: m_objectMatrix, which a shatter leaves alone while it
// sends the window's own matrix 100 m down.
ObjectIdent IdentOf(void *object) {
	ObjectIdent id{};
	const float *p = &Field<float>(object, obj::OBJECT_MATRIX_POS);
	id.pos.x      = p[0];
	id.pos.y      = p[1];
	id.pos.z      = p[2];
	id.modelIndex = Field<int16_t>(object, offs::MODEL_INDEX);
	return id;
}

void Shatter(GlassSite site, void *window, float amount, float sx, float sy, float sz,
             float px, float py, float pz, uint32_t explosion) {
	const ShatterFn original = Func<ShatterFn>(obj::CGlass__WindowRespondsToCollision);
	if (!window) {
		original(window, amount, sx, sy, sz, px, py, pz, explosion);
		return;
	}

	const bool theirs = g_cb.InSomebodyElsesRound && g_cb.InSomebodyElsesRound();
	if (!GlassShattersHere(site, theirs)) {
		// The shooter's engine rolled for this round. Unless the window is
		// already broken, in which case the engine would have done nothing.
		if (!GlassIsBroken(ObjectFlags(window)))
			++g_stats.leftToShooter;
		return;
	}

	const bool was = GlassIsBroken(ObjectFlags(window));
	original(window, amount, sx, sy, sz, px, py, pz, explosion);
	const bool is = GlassIsBroken(ObjectFlags(window));
	if (was || !is)
		return;
	++g_stats.shatteredHere;

	const bool session = g_cb.HaveSession && g_cb.HaveSession();
	if (!GlassShatterGoesOut(was, is, g_applying, session, IsMapObject(window)))
		return;

	const GlassBreakBody body =
	    MakeGlassBody(IdentOf(window), amount, Vec3{sx, sy, sz}, Vec3{px, py, pz},
	                  (explosion & 0xFF) != 0);
	RememberObject(body.ident);
	if (g_cb.Shattered && g_cb.Shattered(body)) {
		if (g_stats.reported++ == 0)
			Log("glass: a window (model %d at %.1f %.1f %.1f) shattered here, %s; told the "
			    "session so it shatters on every screen",
			    static_cast<int>(body.ident.modelIndex), body.ident.pos.x, body.ident.pos.y,
			    body.ident.pos.z,
			    site == GlassSite::Collision ? "something ran into it"
			    : site == GlassSite::Round   ? "a round"
			                                 : "a blast");
	} else {
		++g_stats.unsent;
	}
}

void __cdecl ShatterOnCollision(void *window, float amount, float sx, float sy, float sz,
                                float px, float py, float pz, uint32_t explosion) {
	Shatter(GlassSite::Collision, window, amount, sx, sy, sz, px, py, pz, explosion);
}

void __cdecl ShatterOnRound(void *window, float amount, float sx, float sy, float sz,
                            float px, float py, float pz, uint32_t explosion) {
	Shatter(GlassSite::Round, window, amount, sx, sy, sz, px, py, pz, explosion);
}

void __cdecl ShatterInBlast(void *window, float amount, float sx, float sy, float sz,
                            float px, float py, float pz, uint32_t explosion) {
	Shatter(GlassSite::Blast, window, amount, sx, sy, sz, px, py, pz, explosion);
}

uintptr_t HookFor(uintptr_t site) {
	if (site == obj::GLASS_BULLET_BREAK_CALL)
		return reinterpret_cast<uintptr_t>(&ShatterOnRound);
	if (site == obj::GLASS_BLAST_BREAK_CALL)
		return reinterpret_cast<uintptr_t>(&ShatterInBlast);
	return reinterpret_cast<uintptr_t>(&ShatterOnCollision);
}

// Points the `call` at `site` at `to`, only while it still calls `from`.
bool RedirectCall(uintptr_t site, uintptr_t from, uintptr_t to) {
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

// Our copy of a window somebody named, or null. Counts a double match, which
// the map measurement says cannot happen (3.54 m between the closest two).
void *FindWindow(const ObjectIdent &ident) {
	bool  ambiguous = false;
	void *window    = FindObjectByIdent(LiveObjectPool(), ident, &ambiguous);
	if (ambiguous)
		++g_stats.identCollisions;
	return window;
}

} // namespace

void SetGlassCallbacks(const GlassCallbacks &callbacks) { g_cb = callbacks; }

const GlassStats &GetGlassStats() { return g_stats; }

void OnGlassBrokenElsewhere(const GlassBreakBody &body) {
	++g_stats.received;
	GlassShatter s;
	if (!CleanGlassBody(body, s)) {
		++g_stats.refused;
		return;
	}
	RememberObject(body.ident);

	void *window = FindWindow(body.ident);
	if (!window) {
		// The ordinary case: nobody here is within 80 m, so our copy is a
		// dummy. If somebody stays near it the session keeps the record, and
		// walking over there is told it is broken (OnGlassRecord).
		++g_stats.receivedUnmatched;
		return;
	}
	if (!IsWindowModel(window)) {
		++g_stats.refused;
		return;
	}
	if (GlassIsBroken(ObjectFlags(window))) {
		++g_stats.receivedNoop;
		return;
	}

	g_applying = true;
	Func<ShatterFn>(obj::CGlass__WindowRespondsToCollision)(
	    window, s.amount, s.speed.x, s.speed.y, s.speed.z, s.point.x, s.point.y, s.point.z,
	    s.explosion ? 1u : 0u);
	g_applying = false;

	if (g_stats.applied++ == 0)
		Log("glass: shattered a window another machine broke (model %d at %.1f %.1f %.1f), "
		    "with the engine's own panes and sound",
		    static_cast<int>(body.ident.modelIndex), body.ident.pos.x, body.ident.pos.y,
		    body.ident.pos.z);
}

void OnGlassRecord(const ObjectBreakBody &body) {
	if ((body.state & OBJ_BREAK_GLASS) == 0)
		return;
	const Vec3 &at = body.ident.pos;
	if (!GlassFinite(at.x) || !GlassFinite(at.y) || !GlassFinite(at.z)) {
		++g_stats.refused;
		return;
	}
	RememberObject(body.ident);

	void *window = FindWindow(body.ident);
	if (!window)
		return;   // a dummy here; built again later, it asks (object.h)
	if (!IsWindowModel(window)) {
		++g_stats.refused;
		return;
	}
	uint8_t &flags = ObjectFlags(window);
	if (GlassIsBroken(flags))
		return;

	// What WindowRespondsToCollision leaves, without what it shows: the two
	// bits, and the window's own matrix 100 m under the map. The engine
	// writes only that one float (0x0050461F) and neither UpdateRW nor
	// RemoveAndAdd, and nor does this: a window is drawn by CGlass, which
	// skips a broken one, and its collision reads the CMatrix.
	flags = GlassLatchedFlags(flags);
	Field<float>(window, offs::POSITION + 8) = obj::GLASS_GONE_Z;

	if (g_stats.latched++ == 0)
		Log("glass: a window the session holds as broken (model %d at %.1f %.1f %.1f) was "
		    "whole here; it is gone now, as it is on the screens that watched it go",
		    static_cast<int>(body.ident.modelIndex), at.x, at.y, at.z);
}

bool InstallGlassHooks() {
	g_stats    = GlassStats{};
	g_applying = false;
	uint8_t taken = 0;
	for (Site &s : g_sites) {
		if (!s.taken)
			s.taken = RedirectCall(s.at, obj::CGlass__WindowRespondsToCollision, HookFor(s.at));
		if (s.taken)
			++taken;
		else
			Log("glass: 0x%08X is not a call to CGlass::WindowRespondsToCollision in this "
			    "image, so %s stays on one screen",
			    static_cast<unsigned>(s.at), s.what);
	}
	g_stats.sitesTaken = taken;
	if (taken == 4)
		Log("glass: took the four calls that shatter a window: a window breaks on every "
		    "screen, and a round somebody else fired leaves its one in four to the shooter");
	return taken == 4;
}

void RemoveGlassHooks() {
	g_cb = GlassCallbacks{};
	for (Site &s : g_sites)
		if (s.taken &&
		    RedirectCall(s.at, HookFor(s.at), obj::CGlass__WindowRespondsToCollision))
			s.taken = false;
	g_applying = false;
}

void AddGlassToBridge(WorldBridge &bridge) {
	bridge.GlassBroken  = &OnGlassBrokenElsewhere;
	bridge.GlassLatched = &OnGlassRecord;
}

} // namespace coopiii::game
