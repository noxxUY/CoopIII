// Other players on the minimap, drawn the way the local player is drawn.
//
// ---------------------------------------------------------------------------
// What this used to be, and why it changed
// ---------------------------------------------------------------------------
//
// The first version of this file registered a BLIP_CHAR in the game's own
// CRadar::ms_RadarTrace and let CRadar::DrawBlips draw it. That worked - the
// blips appeared, the owner confirmed it - and it was still the wrong answer,
// for one reason stated in one sentence:
//
//     "se ve un coso verde no se ve el blip como el jugador local de la
//      flecha etc"
//
// A table entry can only ever be a square. Shape is not a field of
// sRadarTrace; m_eRadarSprite is, but DrawBlips only reaches a sprite for
// four of the twenty-one (BOMB, SAVE, SPRAY, WEAPON) and reaches it through
// DrawRadarSprite, which does not rotate. So the table cannot express "a
// person, facing that way", and a person facing a way is the whole of what a
// player marker is.
//
// The thing it could not express is not missing from the engine. It is what
// the engine draws for *you*: CRadar::DrawBlips draws the local player
// itself, before it touches the table at all, at the centre of the radar,
// with DrawRotatingRadarSprite and CentreSprite, at
//
//     angle = FindPlayerHeading() - (PI + TheCamera.GetForward().Heading())
//
// which is a heading measured against the camera - the reason it keeps
// pointing where you are facing on a radar that turns with the camera.
//
// So a remote player is drawn the same way, with the same sprite, the same
// geometry, the same transforms and the same alpha curve, at their place on
// the radar instead of at the centre. The blip table is gone; there is one
// system, not two.
//
// ---------------------------------------------------------------------------
// The four engine calls this is built out of
// ---------------------------------------------------------------------------
//
// Nothing here paints over the radar. Every step is the game's own, in the
// order the game's own blip loop does them (addresses.h quotes it):
//
//   TransformRealWorldPointToRadarSpace  world x/y -> radar space, rotated
//                                        by whatever the camera is doing,
//                                        including top-down and look-behind
//   LimitRadarPoint                      clamps to length 1, which is what
//                                        pins a distant player to the rim
//                                        instead of losing them, and returns
//                                        how far past the rim they were
//   CalculateBlipAlpha                   255 inside the radar, easing to 128
//                                        by five times its range. The game's
//                                        own "that direction, far".
//   TransformRadarPointToScreenSpace     radar space -> pixels
//
// and then CSprite2d::Draw, the four-corner overload, which is the one
// DrawRotatingRadarSprite itself ends in.
//
// The last of those is the only place CoopIII does not call the engine
// function whole. CRadar::DrawRotatingRadarSprite builds its colour in place
// as CRGBA(255, 255, 255, alpha) - only the alpha is an argument - so calling
// it gives a white arrow and nothing else, and two identical white arrows on
// one radar is exactly the confusion this feature exists to remove. Its body
// is nine lines of arithmetic over numbers that are all in addresses.h, so
// the quad is built here (MakeArrowQuad, below, and tools/clienttest holds it
// to the retail disassembly) and handed to the same CSprite2d::Draw with a
// colour. The sprite, the size, the rotation and the draw are all still the
// engine's.
//
// ---------------------------------------------------------------------------
// The colour
// ---------------------------------------------------------------------------
//
// Green on foot, red in a car. Not picked: they are the two colours GTA III
// itself attaches to a meaning, ADD_BLIP_FOR_CHAR's colour 1 and
// ADD_BLIP_FOR_CAR's colour 0, and they are what the square already was - so
// this change is a change of shape and nothing else. The actual RGBA is not
// written down here either; it comes out of CRadar::GetRadarTraceColour, the
// same lookup every blip on the same radar goes through, so a remote player
// is the same green as a mission marker rather than a green of CoopIII's own.
//
// White was available and is wrong. The local player is white, and the owner
// will be looking at his own arrow and somebody else's at the same time; that
// is the one pair that has to be told apart at a glance.
//
// ---------------------------------------------------------------------------
// The Widescreen Fix, and why this file answers it the opposite way to
// nametag.h
// ---------------------------------------------------------------------------
//
// nametag.h refuses to touch the HUD's 640x448 grid, because scaling x by
// screenWidth/640 reinstates precisely the stretch ThirteenAG's fix exists to
// remove, and a nametag is CoopIII's own element with no reason to inherit
// the HUD's aspect handling.
//
// An arrow on the radar is the opposite case and gets the opposite answer. It
// is not CoopIII's element. It is a copy of one of the game's, drawn on the
// game's radar, an inch from the original - and if the two are scaled
// differently they are visibly two different things. So this uses
// SCREEN_SCALE_X/Y exactly as DrawRotatingRadarSprite does, down to retail's
// truncation of both half-extents to whole pixels, and it reads the two
// reciprocals out of the image at runtime rather than compiling in 1/640 and
// 1/448, so that a mod which rescales the radar by patching them moves our
// arrow with the game's.
//
// ---------------------------------------------------------------------------
// Players with no ped (docs/roadmap.md §5.3)
// ---------------------------------------------------------------------------
//
// The blip table could not do this: a BLIP_CHAR has nothing to track without
// a ped, so the old BlipWanted required one and a player outside the
// streaming radius fell off the radar entirely. Drawing it ourselves costs
// one branch. A player with a live ped is drawn from the ped - or from
// ped->m_pMyVehicle when they are in one, which is what DrawBlips does and is
// what keeps the position, the heading and the colour from ever disagreeing
// about whether somebody is driving. A player without one is drawn from the
// pose on the wire, which is already there and already arriving. §5.3 is the
// path that had to survive the change, and it is now the shorter half of it.
//
// Everything in this header is pure arithmetic, so tools/clienttest covers it
// without the game. radar.cpp is the half that touches game memory, under the
// same rule as the rest of the game layer: no address or offset that has not
// been confirmed against the disassembly.
#pragma once

#include "addresses.h"

#include <cmath>
#include <cstddef>
#include <cstdint>

namespace coopiii {
class Client;
}

namespace coopiii::game {

// ---- installing -----------------------------------------------------------

// Detours CRadar::DrawBlips and starts drawing remote players on the radar.
// `client` is only ever read, and only on the game thread.
//
// DrawBlips rather than CHud::Draw, and the reason is not that CHud::Draw is
// taken (it is - nametag.cpp has it, and MinHook allows one hook per target -
// but that is an argument about plumbing). It is that DrawBlips is where the
// radar's own preconditions hold. It sets six render states at the top for
// exactly this kind of drawing and leaves them set; it runs only when the
// radar is actually on screen; and calling the original first puts our arrows
// over the game's blips and its compass, which is the right order for a thing
// that is a player rather than a map feature.
bool InstallRadarArrows(const Client &client);
void RemoveRadarArrows();
bool RadarArrowsInstalled();

// ---- the geometry, which is DrawRotatingRadarSprite's ---------------------

constexpr float RADAR_PI      = 3.14159265358979323846f;
constexpr float RADAR_HALFPI  = RADAR_PI * 0.5f;
constexpr float RADAR_QUARTPI = RADAR_PI * 0.25f;

// The half-extents, including retail's truncation.
//
// "Half-extent" is what DrawRotatingRadarSprite's own variables are called
// and it is slightly a lie: the four corners sit on a circle of this radius,
// so the quad is sqrt(2) times it on a side, about 11 px at the reference
// resolution where a DrawRadarSprite blip is 16. The arrow really is smaller
// than the compass on the same radar. That is the engine's decision, and
// matching the local player's arrow is the whole job.
//
// 0x004A5D10 computes SCREEN_SCALE_X(8.0) and SCREEN_SCALE_Y(8.0) and then
// puts each through `fnstcw / or byte [esp+5],0Ch / fistp qword / fldcw`,
// which is round-toward-zero into an integer - so the arrow's half-extents
// are whole pixels. re3 keeps them as floats. Drawing beside the game's own
// arrow means matching the game, not the decompilation.
//
// No lower clamp, deliberately. Below about 80 pixels of screen width the
// truncation reaches zero and the arrow disappears - and so does the game's
// own, in the same frame, for the same reason. A floor here would make
// CoopIII's arrow visible on a radar that no longer has one.
inline float RadarSpriteHalf(int screenPixels, float recipRef) {
	if (screenPixels <= 0 || !(recipRef > 0.0f))
		return 0.0f;
	const float v = static_cast<float>(screenPixels) * recipRef * RADAR_SPRITE_HALF_REF;
	if (!(v > 0.0f))
		return 0.0f;
	return std::floor(v);
}

// The camera term in DrawBlips' angle, recovered from the engine rather than
// read out of TheCamera.
//
// TransformRealWorldPointToRadarSpace is
//
//     out.x = s*y + c*x,  out.y = c*y - s*x
//
// over (in - vec2DRadarOrigin) / m_radarRange, where s and c are the sine and
// cosine of whatever heading the camera has settled on this frame. Feed it
// the point one radar range due north of the radar origin and x is 0 and y is
// 1, so it hands back (s, c) exactly - and it does so having already taken
// its own decision about top-down cameras, first person and looking behind,
// which is four branches and three globals CoopIII then does not have to
// find, read or keep right.
//
// It is also self-consistent by construction: the same function decides where
// the arrow goes, so the arrow can never point one way while the radar is
// rotated another.
inline float RadarCameraHeading(float s, float c) {
	return std::atan2(s, c);
}

// DrawBlips' own expression, for somebody else's heading.
//
//     angle = heading - (PI + cameraHeading)
//
// The top-down branch of DrawBlips uses `PI + FindPlayerHeading()` instead,
// and this covers that too rather than needing a second case: in top-down
// TransformRealWorldPointToRadarSpace returns s = 0, c = 1, so
// RadarCameraHeading gives 0 and this gives `heading - PI`, which is the same
// angle as `heading + PI`. Sine and cosine do not distinguish them and
// nothing downstream of here does anything else with the number.
inline float ArrowAngle(float heading, float cameraHeading) {
	return heading - (RADAR_PI + cameraHeading);
}

// The four corners CRadar::DrawRotatingRadarSprite builds, in the order it
// hands them to CSprite2d::Draw.
//
// The loop at 0x004A5D94 is four iterations of
//
//     a    = i * HALFPI + (angle - PI/4)
//     x[i] = cx + (0.0*cos(a) + 1.0*sin(a)) * halfX
//     y[i] = cy + (1.0*cos(a) - 0.0*sin(a)) * halfY
//
// with the 0.0 at 0x005F7110 and the 1.0 at 0x005F711C both still multiplied
// through in the retail code. They are kept out of the arithmetic here and
// left in the comment: writing `+ 0.0f * std::cos(a)` to be faithful to a
// disassembly is faithfulness to the compiler, not to the engine.
//
// The draw order is 3, 2, 0, 1 - a triangle fan that walks the quad's corners
// the right way round. Getting it wrong does not produce a wrong shape, it
// produces a bow tie.
struct ArrowQuad {
	float x[4] = {0.0f, 0.0f, 0.0f, 0.0f};
	float y[4] = {0.0f, 0.0f, 0.0f, 0.0f};
};

inline ArrowQuad MakeArrowQuad(float cx, float cy, float angle, float halfX,
                               float halfY) {
	ArrowQuad     quad;
	const float   corrected = angle - RADAR_QUARTPI;
	for (int i = 0; i < 4; ++i) {
		const float a = static_cast<float>(i) * RADAR_HALFPI + corrected;
		quad.x[i]     = cx + std::sin(a) * halfX;
		quad.y[i]     = cy + std::cos(a) * halfY;
	}
	return quad;
}

// The order CSprite2d::Draw wants them in. Named rather than spelled 3, 2, 0,
// 1 at the call site so that it is a quoted fact and not a typo waiting to
// happen: `sprite->Draw(curPosn[3], curPosn[2], curPosn[0], curPosn[1], col)`
// at the tail of 0x004A5D10.
constexpr int ARROW_DRAW_ORDER[4] = {3, 2, 0, 1};

// ---- what colour, and whether there is an arrow at all --------------------

// Green on foot, red in a car, which is ADD_BLIP_FOR_CHAR's colour and
// ADD_BLIP_FOR_CAR's colour respectively. These are indices into
// CRadar::GetRadarTraceColour, not RGBA - see the header comment.
inline uint32_t ArrowTraceColour(bool inVehicle) {
	return inVehicle ? RADAR_TRACE_RED : RADAR_TRACE_GREEN;
}

struct ArrowRgb {
	uint8_t r, g, b;
};

// GetRadarTraceColour hands back one dword. DrawBlips takes it apart as
// `(uint8)(color >> 24), (uint8)(color >> 16), (uint8)(color >> 8)` - so the
// packing is 0xRRGGBBAA and the low byte, the one that looks like alpha, is
// not used by anything: the blip loop passes its own alpha instead. This does
// the same, and CalculateBlipAlpha supplies the alpha, which is how a distant
// player comes out dimmer.
inline ArrowRgb UnpackTraceColour(uint32_t packed) {
	return ArrowRgb{static_cast<uint8_t>((packed >> 24) & 0xFFu),
	                static_cast<uint8_t>((packed >> 16) & 0xFFu),
	                static_cast<uint8_t>((packed >> 8) & 0xFFu)};
}

// Whether a roster slot should have an arrow on the radar this frame.
//
// Two predicates where the blip table needed three. `hasPed` is gone: an
// arrow is drawn from a position and a heading, and a player with no ped
// still has both, off the wire. What is left is that they are in the session
// and that something has said where they are - a marker at the world origin
// is a marker in the water off Portland, which is what a player whose first
// snapshot has not arrived would get.
inline bool ArrowWanted(bool active, bool havePose) {
	return active && havePose;
}

// ---- the radar's own range ------------------------------------------------
//
// Only used to build the probe point for RadarCameraHeading, and only ever
// read. DrawMap writes it every frame, 120 m on foot ramping to 350 in a fast
// car; a zero would make the probe meaningless and a negative would turn the
// radar inside out, so both are refused rather than divided by.
inline bool RadarRangeUsable(float range) {
	return range > 0.0f && range < 100000.0f;
}

} // namespace coopiii::game
