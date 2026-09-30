// Other players on the minimap, drawn the way the local player is drawn.
//
// ---------------------------------------------------------------------------
// What this used to be, and why it changed
// ---------------------------------------------------------------------------
//
// The first version of this file registered a BLIP_CHAR in the game's own
// CRadar::ms_RadarTrace and let CRadar::DrawBlips draw it. That worked - the
// blips appeared, a live game confirmed it - and it was still the wrong answer,
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
#include "../chatfeed.h"

#include <coopiii/protocol.h>

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

// A player's arrow is in their chat colour, so a name in the chat and a mark on
// the radar are the same person at a glance. In a vehicle it is a shade darker,
// which is all that is left of green on foot and red in a car.
constexpr uint8_t ARROW_IN_VEHICLE_SHADE = 160;   // of 255

inline ArrowRgb PlayerArrowRgb(uint8_t playerId, bool inVehicle) {
	const NickColour c = ChatNickColour(playerId);
	if (!inVehicle)
		return ArrowRgb{c.r, c.g, c.b};
	return ArrowRgb{static_cast<uint8_t>(c.r * ARROW_IN_VEHICLE_SHADE / 255),
	                static_cast<uint8_t>(c.g * ARROW_IN_VEHICLE_SHADE / 255),
	                static_cast<uint8_t>(c.b * ARROW_IN_VEHICLE_SHADE / 255)};
}

// A player who is dead keeps an arrow, since where they lie is where their
// team-mates are headed, but not a live one: the arrow goes grey at the
// brightness of their own colour, and half as solid, until they respawn.
constexpr uint8_t ARROW_DEAD_SHADE = 150;   // of 255

inline ArrowRgb DeadArrowRgb(ArrowRgb live) {
	const unsigned luma  = (live.r * 77u + live.g * 150u + live.b * 29u) >> 8;
	const uint8_t  grey  = static_cast<uint8_t>(luma * ARROW_DEAD_SHADE / 255);
	return ArrowRgb{grey, grey, grey};
}

inline uint8_t DeadArrowAlpha(uint8_t alpha) {
	return static_cast<uint8_t>(alpha / 2);
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

// ---- the host's contacts, in place of ours ----------------------------------
//
// The session's campaign is the host's save (MissionSync::FollowsHostsCampaign,
// protocol.h C_ContactMarkers). A contact is two things the engine draws off
// one BLIP_CONTACT_POINT in CRadar::ms_RadarTrace, the icon on the radar
// (DrawBlips' third loop) and the ring of markers in the street
// (CRadar::Draw3dMarkers), both left out while $ONMISSION is 1. So on a guest:
//
//   - its own contacts are taken out of both draws, and out of nothing else.
//     For the length of each of the two calls their m_eBlipDisplay reads
//     NEITHER, and it is put back the moment the call returns. The table,
//     the save and the scripts never see anything but what they wrote; a
//     save the guest makes is its own game's, contacts and all.
//   - the host's are drawn after each call, the way the engine draws its own:
//     DrawRadarSprite at the point the radar's own transforms give, and
//     C3dMarkers::PlaceMarkerSet with the arguments Draw3dMarkers passes, both
//     only while this machine's own $ONMISSION is not 1.
//
// DrawBlips is radar.cpp's detour already. Draw3dMarkers is reached through
// its one call, in CHud::Draw, redirected; the function itself is untouched.
//
// CRadar::Draw3dMarkers (0x004A4C70) walks ms_RadarTrace, and its
// BLIP_CONTACT_POINT arm (jump table 0x005F71E0, entry 4) is
//     call 00439410 / test al,al / jne next        IsPlayerOnAMission
//     cmp  word [ebp+006ED60Ah],3 / je / cmp ..,1   display BOTH or MARKER_ONLY
//     push 0 / push [005F7120h] / push 800h         rotate, 0.2f, 2048
//     push 80h / push 0FFh / push 80h / push 0      a, b, g, r
//     push [005F7124h] / push eax(&m_vecPos) / push 4
//     push (m_BlipIndex << 16 | i) / call 0051BB80  C3dMarkers::PlaceMarkerSet
// and the one call to it is CHud::Draw's `E8 A7 C7 F9 FF` at 0x005084C4, right
// after its gates on the HUD and the widescreen bars.
constexpr uintptr_t CRadar__Draw3dMarkers       = 0x004A4C70;
constexpr uintptr_t CHUD_DRAW_3D_MARKERS_CALL   = 0x005084C4;
constexpr uintptr_t C3dMarkers__PlaceMarkerSet  = 0x0051BB80;
constexpr uintptr_t CONTACT_MARKER_SIZE_AT      = 0x005F7124;   // 2.0f
constexpr uintptr_t CONTACT_MARKER_PULSE_AT     = 0x005F7120;   // 0.2f
constexpr float     CONTACT_MARKER_SIZE         = 2.0f;
constexpr float     CONTACT_MARKER_PULSE        = 0.2f;
constexpr uint32_t  CONTACT_MARKER_TYPE         = 4;
constexpr uint32_t  CONTACT_MARKER_PERIOD       = 2048;
constexpr uint8_t   CONTACT_MARKER_RGBA[4]      = {0, 128, 255, 128};
// A marker set is known by its id from one frame to the next. The engine's are
// `m_BlipIndex << 16 | slot` with the slot below 32, and a location check's
// highlight is a script's address; this is neither.
constexpr uint32_t  HOST_CONTACT_MARKER_ID      = 0xC0DEFF00u;

// A trace that is one of this machine's own contacts that start missions:
// what a guest does not draw, and what the host sends.
inline bool IsMissionContact(bool inUse, uint32_t blipType, uint32_t sprite) {
	return inUse && blipType == BLIP_CONTACT_POINT && ContactSpriteIsAMissions(sprite);
}

// The engine's two display tests, the radar's and the street's.
inline bool ContactShowsIcon(uint32_t display) {
	return display == BLIP_DISPLAY_BOTH || display == BLIP_DISPLAY_BLIP_ONLY;
}
inline bool ContactShowsMarker(uint32_t display) {
	return display == BLIP_DISPLAY_BOTH || display == BLIP_DISPLAY_MARKER_ONLY;
}

inline uint32_t HostContactMarkerId(size_t i) {
	return HOST_CONTACT_MARKER_ID | static_cast<uint32_t>(i & 0xFF);
}

// This machine's contacts that start missions, as its radar has them, in slot
// order: what the host sends (MissionBridge::ReadContactMarkers).
size_t ReadContactMarkers(ContactMarker *out, size_t max);

// ---- the host's places, beside ours (protocol.h, C_PlaceBlips) --------------
//
// The Ammu-Nations, bomb shops, Pay'n'Sprays and safehouses are each game's
// own radar blips, made by its own campaign as it goes: Last Requests adds
// Staunton's Ammu-Nation and Pay'n'Spray (02A8, 39_frank4.sc:677), init.sc
// the safehouses (02A7). A guest whose save has not got as far as the host's
// did not have them. So the host's go to everybody, and a guest draws each
// one its own radar has no icon of the same sprite near, after DrawBlips, as
// DrawRadarSprite would draw its own. Nothing goes into the guest's table:
// its save, and what its own script later adds, stay its own.

// A trace that is one of the four places, showing its icon: a coordinate
// blip or a contact point, which are the two the script makes them with.
inline bool IsPlaceTrace(bool inUse, uint32_t blipType, uint32_t sprite, uint32_t display) {
	return inUse && (blipType == BLIP_COORD || blipType == BLIP_CONTACT_POINT) &&
	       IsPlaceSprite(sprite) && ContactShowsIcon(display);
}

// The same place twice: one sprite, within this of each other. The script
// moves a safehouse's blip a few metres between two versions of init.sc and
// save.sc (893.5 against 870.0), and no two places of one sprite stand this
// close.
constexpr float PLACE_SAME_M = 40.0f;

inline bool SamePlace(uint32_t spriteA, float ax, float ay, uint32_t spriteB, float bx, float by) {
	const float dx = ax - bx, dy = ay - by;
	return spriteA == spriteB && dx * dx + dy * dy <= PLACE_SAME_M * PLACE_SAME_M;
}

// This machine's places, as its radar has them, in slot order: what the host
// sends (MissionBridge::ReadPlaceBlips).
size_t ReadPlaceBlips(ContactMarker *out, size_t max);

} // namespace coopiii::game
