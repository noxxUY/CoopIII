// Nothing synced appears or disappears in front of a player.
//
// GTA III never pops its own crowd in or out on its player's screen. Its two
// generators make nothing in view at close range (no car on screen nearer the
// camera than 90 m, no pedestrian in view nearer than 40 m), and fade in what
// they do make, from clump alpha 0. Its two reapers take what is off the
// screen at once and only set bFadeOut on what is on it, so it goes when its
// alpha has run out (addresses.h, "how the engine keeps its own crowd from
// popping").
//
// It does all of that against one camera, its own. In a session the crowd one
// machine makes is drawn on the others, and three things came out of that:
//
//   - a host's generator made a car or a pedestrian behind its own player, and
//     the player beside him, looking the other way, watched it appear;
//   - a copy was built the frame its spawn arrived, at full alpha;
//   - a copy went the frame its despawn arrived, whatever the reason: the host
//     drove off, died, crossed to another island or left, and nobody near
//     could take it over (the let-go of protocol.md 1.45 and 1.58 hands it on
//     when somebody can).
//
// So the same three rules run here against every camera in the session:
//
//   - a host's generators pass over a spot inside another player's view at
//     close range, with that player's camera off the wire (C_PlayerView);
//   - a copy is built faded out and fades in, and a fresh one announced inside
//     our own view at close range is held unbuilt until it is out of it;
//   - a copy its host gave up on (C_CrowdGone) fades out on our screen and then
//     goes, and goes at once when it is off the screen.
//
// Pure: no game global is read here. game/crowdfade.cpp is the engine half and
// tools/clienttest/crowdfade.cpp walks the rules.
#pragma once

#include "carletgo.h"

#include <coopiii/protocol.h>

#include <cstdint>

namespace coopiii {
struct WorldBridge;
}

namespace coopiii::game {

// The engine half: the local camera, whether a copy is on our screen, and its
// clump alpha with bFadeOut. Never fails to install: it hooks nothing.
void AddCrowdFadeToBridge(WorldBridge &bridge);

// ---- whose view ------------------------------------------------------------

// Half the angle a camera sees across, as a cosine. GTA III's field of view is
// 70 degrees across a 4:3 screen and a widescreen fix widens it, and the view
// we hold for another player can be a quarter of a second old; 60 degrees
// either side covers both, at the cost of now and then passing over a spot
// just outside somebody's screen.
constexpr float VIEW_HALF_ANGLE_COS = 0.5f;

// The engine's own close-range limits for making something in view
// (addresses.h, CAR_GEN_INVIEW_MIN_M_AT and PED_GEN_INVIEW_MIN_M_AT), at a
// multiplier of 1.
constexpr float CAR_INVIEW_MIN_M = 90.0f;
constexpr float PED_INVIEW_MIN_M = 40.0f;

inline float InViewMinFor(bool car) { return car ? CAR_INVIEW_MIN_M : PED_INVIEW_MIN_M; }

// Does a camera at `v` see `at`? Flat, as both generators measure: a cone of
// VIEW_HALF_ANGLE_COS around the camera's forward axis on the ground. A camera
// looking straight down or up has no direction on the ground and sees all
// round it, which is what a top-down view does.
inline bool ViewSees(const PlayerViewBody &v, const Vec3 &at) {
	const float dx = at.x - v.pos.x, dy = at.y - v.pos.y;
	const float d2 = dx * dx + dy * dy;
	if (!(d2 == d2))
		return false;
	if (d2 < 1.0f)
		return true;
	const float f2 = v.fwd.x * v.fwd.x + v.fwd.y * v.fwd.y;
	if (!(f2 == f2))
		return false;
	if (f2 < 0.01f)
		return true;
	const float dot = dx * v.fwd.x + dy * v.fwd.y;
	if (dot <= 0.0f)
		return false;
	// cos >= c  <=>  dot^2 >= c^2 * |d|^2 * |f|^2, with dot positive.
	return dot * dot >= VIEW_HALF_ANGLE_COS * VIEW_HALF_ANGLE_COS * d2 * f2;
}

// In view and nearer the camera than `minM`, flat.
inline bool InViewWithin(const PlayerViewBody &v, const Vec3 &at, float minM) {
	const float dx = at.x - v.pos.x, dy = at.y - v.pos.y;
	return dx * dx + dy * dy < minM * minM && ViewSees(v, at);
}

// A spot our generator picked: inside any of the other players' views at the
// engine's close range for that kind of thing? Then it is passed over, as the
// engine passes over one in its own.
inline bool SpotInSomebodysView(const Vec3 &spot, bool car, const PlayerViewBody *views,
                                uint32_t count) {
	const float minM = InViewMinFor(car);
	for (uint32_t i = 0; views && i < count; ++i)
		if (InViewWithin(views[i], spot, minM))
			return true;
	return false;
}

// ---- a copy coming in ------------------------------------------------------

// How long a copy takes to fade in. The engine's +16 a frame is a quarter of a
// second at 60 frames and half a second at 30; this sits between.
constexpr uint32_t CROWD_FADE_IN_MS = 400;

// A copy announced this recently (a live spawn, never a backfill) and never
// built here yet, inside our own view at the engine's close range, waits
// unbuilt until it is out of it. Not for ever: past this it is built anyway,
// and fades in.
constexpr uint32_t BIRTH_HOLD_MAX_MS = 8000;

inline bool HoldBirthInView(bool car, bool mission, bool everBuilt, uint32_t ageMs,
                            bool haveView, const PlayerViewBody &ours, const Vec3 &at) {
	if (mission || everBuilt || !haveView || ageMs >= BIRTH_HOLD_MAX_MS)
		return false;
	return InViewWithin(ours, at, InViewMinFor(car));
}

inline uint8_t FadeInAlpha(uint32_t elapsedMs) {
	if (elapsedMs >= CROWD_FADE_IN_MS)
		return 255;
	return static_cast<uint8_t>(255u * elapsedMs / CROWD_FADE_IN_MS);
}

// ---- a copy going ----------------------------------------------------------

// How long a copy on our screen takes to fade out: the engine's -8 a frame
// from full is about half a second at 60 frames and a second at 30.
constexpr uint32_t CROWD_FADE_OUT_MS = 700;

inline uint8_t FadeOutAlpha(uint32_t elapsedMs) {
	if (elapsedMs >= CROWD_FADE_OUT_MS)
		return 0;
	return static_cast<uint8_t>(255u - 255u * elapsedMs / CROWD_FADE_OUT_MS);
}

// Does a copy its host gave up on fade out rather than go at once? Only one
// that is built and on our screen, never the mission's (a mission's pedestrian
// and car come and go on the mission's word, and fading one would hold it past
// an instruction that names it).
inline bool FadesOut(bool built, bool mission, bool onScreen) {
	return built && !mission && onScreen;
}

// Is a fading copy done? Its alpha has run out, or it has gone off our screen,
// where the engine would have taken it at once, or it is not built any more.
inline bool FadeOutOver(uint32_t elapsedMs, bool built, bool onScreen) {
	return !built || !onScreen || elapsedMs >= CROWD_FADE_OUT_MS;
}

// ---- what a host says when its engine gives up on something ------------------

// A car of ours our engine took away, that nobody was there to take over: the
// session is told it may fade (C_CrowdGone) when the reason was our own
// player's - a distance, a respawn's clear, an island left - and it was
// named and not the mission's. A wreck and everything else stay a despawn.
inline bool CarFadesWhenDropped(CarReap reap, bool named, bool mission) {
	return named && !mission && ReapHandsOn(reap);
}

// The same for a pedestrian, a corpse the reaper faded out included.
inline bool PedFadesWhenDropped(PedDrop drop, bool named, bool mission) {
	return named && !mission && drop != PedDrop::Other;
}

// The view we send of our own camera, every this often.
constexpr uint32_t PLAYER_VIEW_SEND_MS = 250;
// And how long a view we were sent is believed: a player whose views stop
// coming (a dropped packet is nothing, a paused game is) is nobody's camera.
constexpr uint32_t PLAYER_VIEW_STALE_MS = 2000;

} // namespace coopiii::game
