// Shattered glass: shop windows and the other panes CGlass breaks.
//
// docs/objects.md 10 is the investigation; addresses.h, "glass", has the
// instructions. What the file rests on:
//
// 1. **A window is a map object, named the way a lamp post is.** One of eight
//    models, placed by an IPL, a CDummyObject far away and a CObject inside
//    80 m, with m_objectMatrix holding the placement through every
//    conversion. The 42 placed in Liberty City are 3.54 m apart at the
//    closest within one model, so object.h's 0.25 m names exactly one.
//
// 2. **It never goes through ObjectDamage.** object.dat gives every glass
//    model a damage effect of 0, and the four calls that shatter one go to
//    CGlass::WindowRespondsToCollision instead, which latches bGlassBroken
//    on the object and moves it 100 m under the map. So the street object
//    seam never saw a window, and every machine broke its own.
//
// 3. **Two of the four causes did not agree.** A car into a window is a
//    collision only its owner's engine ran for real. And a round on a window
//    that is already cracked shatters it one time in four, by
//    CGeneral::GetRandomNumber, which every machine rolls for itself when it
//    replays the shot. A blast agreed (the same test on the same numbers),
//    and the first round's crack agreed.
//
// So: whichever machine shattered a window says so, with what the engine was
// handed, and every other machine runs the same function with the same
// arguments, which plays the engine's own panes and sound. A shatter is a
// latch and the engine refuses a second one on a broken window
// (`bGlassBroken? return` is its first test), so two machines that both
// shattered the same window and both said so cost one wasted packet and
// nothing else. A machine replaying somebody else's round does not roll the
// one in four at all: that round's dice are the shooter's.
//
// Nothing applied off the wire goes back out. The receiver calls the function
// directly, not through the four redirected calls, so nothing here even sees
// it; and a flag says so anyway.
//
// The engine puts a window back on its own: ConvertToDummyObject builds the
// dummy from m_objectMatrix, and the next CObject is a whole window. That is
// mirrored rather than fought. The server keeps a shattered window as an
// object record (OBJ_BREAK_GLASS) while anybody is near it, so a joiner and a
// player who comes back are told it is gone and see it gone, latched without
// the effect; once nobody is near, every copy is a dummy again and whole
// everywhere.
#pragma once

#include "game/addresses.h"

#include <coopiii/protocol.h>

#include <cstddef>
#include <cstdint>

namespace coopiii {
struct WorldBridge;
}

namespace coopiii::game {

// ---------------------------------------------------------------------------
// The decisions
// ---------------------------------------------------------------------------

// Which of the four calls to CGlass::WindowRespondsToCollision this is.
enum class GlassSite : uint8_t {
	Collision,   // CPhysical::ApplyCollision, either arm
	Round,       // CGlass::WasGlassHitByBullet, the one in four
	Blast,       // CGlass::WindowRespondsToExplosion, nearer than 10 m
};

// Does the engine's shatter run here at all?
//
// Everywhere, except a round this machine is drawing for somebody else. That
// one reached the window the way the shooter's did, and the shooter's engine
// already rolled for it; rolling again here would shatter the window on one
// screen and not the other one time in four, whichever way. What the
// shooter's roll decided arrives as a C_GlassBroken.
constexpr bool GlassShattersHere(GlassSite site, bool somebodyElsesRound) {
	return site != GlassSite::Round || !somebodyElsesRound;
}

// Does a shatter that just happened here go on the wire?
//
// Only an actual change (the engine refuses a broken window, and a call that
// did nothing says nothing), never one this machine was told about, only in a
// session, and only for a map object: the eight models are never a script's
// or a pickup's in the retail map, and an object nobody else has at that
// placement would only be refused at the other end.
constexpr bool GlassShatterGoesOut(bool wasBroken, bool isBroken, bool applying,
                                   bool haveSession, bool mapObject) {
	return !wasBroken && isBroken && !applying && haveSession && mapObject;
}

// The bitfield byte at +0x175, read and written the way the engine does.
constexpr bool GlassIsBroken(uint8_t objectFlags) {
	return (objectFlags & object::OBJ_GLASS_BROKEN) != 0;
}

// What WindowRespondsToCollision leaves in that byte: cracked first, then
// broken, and nothing else touched.
constexpr uint8_t GlassLatchedFlags(uint8_t objectFlags) {
	return static_cast<uint8_t>(objectFlags | object::OBJ_GLASS_CRACKED |
	                            object::OBJ_GLASS_BROKEN);
}

// IsGlass, over the engine's eight model ids (read out of the image at run
// time; -1 until the game has loaded, which matches nothing).
inline bool ModelIsGlass(int16_t model, const int16_t *ids, size_t count) {
	if (model < 0)
		return false;
	for (size_t i = 0; i < count; ++i)
		if (ids[i] == model)
			return true;
	return false;
}

// ---------------------------------------------------------------------------
// The wire
// ---------------------------------------------------------------------------

// How far from a window's placement its impact point may be. The engine
// staggers a small break by `distance * 100` ms from the point and throws the
// panes away from it, so a point off a socket that is nowhere near the window
// would freeze the panes for minutes; the window's own placement is used
// instead. 30 m is far wider than any shop window, and it has to be measured
// generously because the placement is the model's origin, not the middle of
// the pane.
constexpr float kGlassPointReach = 30.0f;

// The fastest the panes may leave, in units a frame: a car at full speed is
// under 2.
constexpr float kGlassSpeedMax = 3.0f;

// What a shatter off the wire is run with, after the checks.
struct GlassShatter {
	float amount    = 0.0f;
	Vec3  speed{};
	Vec3  point{};
	bool  explosion = false;
};

inline bool GlassFinite(float f) { return f == f && f > -1.0e9f && f < 1.0e9f; }

// The body the sender builds. The ident's pad bytes are zeroed: they ride
// into the session's record and are compared by nothing.
inline GlassBreakBody MakeGlassBody(const ObjectIdent &ident, float amount,
                                    const Vec3 &speed, const Vec3 &point,
                                    bool explosion) {
	GlassBreakBody body{};
	body.ident      = ident;
	body.ident.pad0 = 0;
	body.ident.pad1 = 0;
	body.amount     = amount;
	body.speed      = speed;
	body.point      = point;
	body.flags      = explosion ? GLASS_BREAK_EXPLOSION : 0;
	return body;
}

// Is this a body worth running the engine with, and with what?
//
// A placement that is not a number names nothing, so the whole packet goes.
// Everything else is mended rather than refused, because the sender's engine
// did shatter the window and the receiver should too: an amount that is not a
// number or is negative is a round's 0, a speed that is not one or is faster
// than any car is none, and a point that is not one or is far from the
// window is the window's own placement, which is the point the engine itself
// uses for a blast.
inline bool CleanGlassBody(const GlassBreakBody &in, GlassShatter &out) {
	const Vec3 &at = in.ident.pos;
	if (!GlassFinite(at.x) || !GlassFinite(at.y) || !GlassFinite(at.z))
		return false;

	out           = GlassShatter{};
	out.explosion = (in.flags & GLASS_BREAK_EXPLOSION) != 0;
	out.amount    = GlassFinite(in.amount) && in.amount > 0.0f ? in.amount : 0.0f;

	const Vec3 &s = in.speed;
	if (GlassFinite(s.x) && GlassFinite(s.y) && GlassFinite(s.z) &&
	    s.x * s.x + s.y * s.y + s.z * s.z <= kGlassSpeedMax * kGlassSpeedMax)
		out.speed = s;

	const Vec3 &p = in.point;
	out.point     = at;
	if (GlassFinite(p.x) && GlassFinite(p.y) && GlassFinite(p.z)) {
		const float dx = p.x - at.x, dy = p.y - at.y, dz = p.z - at.z;
		if (dx * dx + dy * dy + dz * dz <= kGlassPointReach * kGlassPointReach)
			out.point = p;
	}
	return true;
}

// ---------------------------------------------------------------------------
// The engine half (glass.cpp)
// ---------------------------------------------------------------------------

struct GlassCallbacks {
	// Tell the session a window shattered here. Returns whether it went out.
	bool (*Shattered)(const GlassBreakBody &body) = nullptr;
	// Is this machine drawing somebody else's round right now: combat.cpp's
	// replay of a shot through CWeapon::Fire, or its drive-by and sniper
	// impact. Null reads as "no", the single player answer.
	bool (*InSomebodyElsesRound)() = nullptr;
	// Is there a session to tell. Null reads as "no".
	bool (*HaveSession)() = nullptr;
};

void SetGlassCallbacks(const GlassCallbacks &callbacks);

// Four call sites, redirected, each only while it still calls
// CGlass::WindowRespondsToCollision (addresses.h, "glass"). Not fatal: a site
// that is not taken leaves its cause to break windows on one screen, and the
// log says which.
bool InstallGlassHooks();
void RemoveGlassHooks();

// Somebody else's engine shattered this window: ours runs the engine's own
// shatter with the same arguments, panes and sound included. Nothing when
// our copy is a dummy (nobody here is near it) or already broken.
void OnGlassBrokenElsewhere(const GlassBreakBody &body);

// The session's record of a shattered window, for a joiner or a machine that
// has just built the window again: latched as the engine leaves it, without
// panes or sound, because nobody here watched it go.
void OnGlassRecord(const ObjectBreakBody &body);

void AddGlassToBridge(WorldBridge &bridge);

struct GlassStats {
	uint32_t shatteredHere    = 0;   // our engine shattered one, any cause
	uint32_t reported         = 0;
	uint32_t unsent           = 0;   // no session to tell
	uint32_t leftToShooter    = 0;   // somebody else's round, not rolled here
	uint32_t received         = 0;
	uint32_t receivedUnmatched = 0;  // no live copy here, usually distance
	uint32_t receivedNoop     = 0;   // ours was already broken
	uint32_t applied          = 0;
	uint32_t refused          = 0;   // not a window here, or not a body
	uint32_t latched          = 0;   // from the session's record
	uint32_t identCollisions  = 0;
	uint8_t  sitesTaken       = 0;   // of four
};

const GlassStats &GetGlassStats();

} // namespace coopiii::game
