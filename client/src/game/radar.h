// Other players on the minimap.
//
// Nothing in CoopIII drew a blip before this. There was no reference to
// CRadar anywhere in the tree, which made "los blips del resto de players no
// salen en el minimapa" a missing feature rather than a bug.
//
// ---------------------------------------------------------------------------
// The one decision this file is about
// ---------------------------------------------------------------------------
//
// CoopIII does not draw anything. It registers a blip with the game's radar
// and the game draws it, out of CHud::Draw, with its own DrawBlips.
//
// That is the project rule ("do what the engine does") but it is also just
// the better feature, and it is worth writing down everything it buys for
// free, because each item is something a hand-drawn sprite would have had to
// get right on its own:
//
//   - the position. DrawBlips reads the entity's matrix position every frame,
//     so a blip is never a snapshot behind and never needs correcting.
//   - the car. A BLIP_CHAR whose ped is in a vehicle is drawn at the
//     *vehicle's* position - DrawBlips does `if (ped->InVehicle())
//     blipEntity = ped->m_pMyVehicle` itself. A driving player's blip moves
//     with the car, not with the seat the engine will put them in next frame.
//   - the rim. LimitRadarPoint clamps anything past m_radarRange onto the
//     edge of the radar, so a player 400 m away already reads as "that
//     direction, far", which is the behaviour GTA III gives every blip.
//   - the radar rotating with the camera, the mask, the alpha, the draw
//     order, the HUD-off and cutscene gates, the flash when the radar is the
//     flashing HUD item. All of it, because it is the same code path the
//     game's own mission markers take.
//   - the teardown. ~CPed calls CRadar::ClearBlipForEntity(BLIP_CHAR,
//     GetPedRef(this)), so a ped CoopIII destroys takes its blip with it.
//
// And it needs no hook at all. There is no detour in this file: the engine
// already calls DrawBlips once a frame, so all CoopIII has to do is keep the
// table right. The one thing that does have to be got right is the table's
// bound, and addresses.h has the disassembly - 32 slots, no bounds check,
// and CDarkel's kill register immediately after it.
//
// ---------------------------------------------------------------------------
// What a remote player looks like, and why
// ---------------------------------------------------------------------------
//
// GTA III already has an answer to "what does a person you should be able to
// find look like on the radar", and it is opcode 391, ADD_BLIP_FOR_CHAR. Its
// handler (0x0044079E) is four calls long and it says:
//
//     SetEntityBlip(BLIP_CHAR, ped, RADAR_TRACE_GREEN, BLIP_DISPLAY_BOTH)
//     ChangeBlipScale(blip, 3)
//
// so: a plain coloured square, scale 3, green, tracking the ped. That is
// what a remote player gets. Nothing invented, no new sprite, no new colour.
//
// Two departures from that line, both deliberate:
//
// 1. BLIP_DISPLAY_BLIP_ONLY instead of BOTH. The difference between them is
//    the 3D marker drawn in the world, and DrawBlips draws that only when
//    CTheScripts::DbgFlag (0x0095CD87) is set, which a retail build never
//    does. So the two are identical in a normal game and BLIP_ONLY is the
//    one that stays identical if anything ever turns the flag on. Eight
//    debug crosses hanging in the street is not a radar feature.
//
// 2. A player in a car is RADAR_TRACE_RED, not green. The reasoning is the
//    same as for everything else here: it is the distinction the game itself
//    makes. ADD_BLIP_FOR_CHAR uses colour 1 (green) and ADD_BLIP_FOR_CAR uses
//    colour 0 (red), and those are the only two colours GTA III systematically
//    attaches to a meaning. Shape is not available - every non-sprite blip in
//    III is the same square, sprites are all named mission contacts (Asuka,
//    Luigi, the save disc) and using one of those for a player would be
//    inventing a vocabulary the game does not have. Scale is not available
//    either: ADD_BLIP_FOR_CAR uses scale 3 as well.
//
//    The blip stays a BLIP_CHAR either way. Only the colour changes, so the
//    engine keeps following the player through getting in and out, and the
//    colour and the position can never disagree about whether somebody is
//    driving - both come off the same two fields of the same CPed.
//
// One thing that is deliberately *not* corrected, and it is the opposite of
// the call nametag.h makes. ShowRadarTrace draws the square as
// SCREEN_SCALE_X(size) by SCREEN_SCALE_Y(size), which is the HUD's own 640 by
// 448 grid, so on this install the Widescreen Fix's HudWidthScale decides how
// square it comes out. A nametag steps around that because a label floating
// over somebody's head is CoopIII's own element and has no reason to inherit
// the HUD's aspect handling. A blip is not CoopIII's element: it is one of
// the game's own blips, drawn by the game's own code, and it has to look like
// the others on the same radar. Whatever the player's mod stack does to a
// mission marker it should do to this, and it does, because it is the same
// call.
//
// ---------------------------------------------------------------------------
// Written as a reconciliation
// ---------------------------------------------------------------------------
//
// Same shape as Client::UpdateRemoteSeats and for the same reason: there are
// too many races for one handler each. A player joins before their ped
// exists; their ped is streamed in later; the engine can reap it; they can
// die and be rebuilt; the campaign script can wipe the whole blip table on a
// game load (CRadar::Initialise) or run out of slots.
//
// So nothing here is an event handler. UpdateRemoteBlips runs every frame and
// drives the table towards what the roster says, and every one of those races
// falls out of the same loop.
//
// The load-bearing part is that **a blip handle is never trusted.** Every
// frame CoopIII asks the table whether the slot it thinks it owns still holds
// a BLIP_CHAR for its own ped ref, exactly the way ped.cpp's ResolveRemote
// asks the pool whether a ped handle still resolves. If it does not, CoopIII
// forgets it and makes a new one - and, importantly, does not clear it,
// because by then the slot may belong to the script. CRadar::Initialise
// resetting every m_BlipIndex to 1 is precisely the case a handle comparison
// would get wrong and an "is it still mine" comparison gets right.
//
// Everything in this header is pure arithmetic, so tools/clienttest covers it
// without the game. radar.cpp is the half that touches game memory, under the
// same rule as the rest of the game layer: no address or offset that has not
// been confirmed against the disassembly.
#pragma once

#include "addresses.h"

#include <cstddef>
#include <cstdint>

namespace coopiii {
class Client;
}

namespace coopiii::game {

// ---- installing -----------------------------------------------------------

// Checks the blip table looks like a blip table and starts putting remote
// players on it. There is no detour to install; this only decides whether the
// table is safe to write and says so in the log. `client` is only ever read,
// and only on the game thread.
bool InstallRadarBlips(const Client &client);

// Called from the frame pump, after the roster has spawned and despawned
// whatever it was going to this frame. Cheap: a handful of loads per player
// plus one pass over 32 bytes.
void UpdateRemoteBlips();

// Takes every blip CoopIII owns back off the radar. Safe to call twice.
void RemoveRadarBlips();
bool RadarBlipsInstalled();

// ---- how many slots CoopIII is allowed -------------------------------------
//
// The blip table is 32 entries and it belongs to the campaign script, which
// is the only thing in the game that creates blips: every caller of
// SetEntityBlip, SetCoordBlip and SetBlipSprite in the whole image is a
// script opcode handler. CoopIII is a guest in it.
//
// Being a guest matters more than usual here because SetEntityBlip has no
// bounds check. With all 32 slots in use it writes the 33rd entry anyway,
// over CDarkel::RegisteredKills, and hands back a handle for a slot that does
// not exist (addresses.h has the arithmetic). So the free-slot count is not a
// nicety, it is the guard - and CoopIII counts the slots itself rather than
// trusting the engine to refuse.
//
// The reserve on top of that is politeness with a reason. If the script is
// down to its last few slots, a mission marker the player needs is worth more
// than knowing which street a team-mate is on, and the script cannot be told
// to wait.

// A quarter of the table, kept for the game's own blips. CoopIII takes a slot
// only if this many would still be free afterwards.
constexpr int RADAR_SCRIPT_RESERVE = 8;

// At most one per other player. Named rather than spelled MAX_PLAYERS - 1 at
// the call site so the bound is a stated intention.
constexpr int RADAR_MAX_OUR_BLIPS = 7;

// May CoopIII take one more slot, given how many are free right now (ours
// already in use are not counted as free) and how many it already holds?
inline bool MayTakeTraceSlot(int freeSlots, int oursHeld) {
	if (oursHeld >= RADAR_MAX_OUR_BLIPS)
		return false;
	// Strictly greater: after taking one, RADAR_SCRIPT_RESERVE remain.
	return freeSlots > RADAR_SCRIPT_RESERVE;
}

// ---- the blip handle ------------------------------------------------------
//
// SetEntityBlip returns `slot | (m_BlipIndex << 16)`, and every ChangeBlip*
// call goes back through GetActualBlipArrayIndex, which returns -1 unless the
// top half still matches the slot's current m_BlipIndex. That is the engine's
// own staleness check and it is the reason a handle is worth keeping.
//
// It is also not enough on its own, twice over. GetActualBlipArrayIndex does
// not range-check the low half at all - `and eax,0FFFFh` and straight into
// the table - so a fabricated or corrupted handle reads and writes wherever
// its low word points. And CRadar::Initialise sets every m_BlipIndex back to
// 1, so a handle with generation 1 can start matching a slot it never owned.
//
// Hence both of the below, and hence TraceIsOurs further down.

constexpr int32_t NO_BLIP = -1;

inline int BlipSlot(int32_t handle) {
	return static_cast<int>(static_cast<uint32_t>(handle) & 0xFFFFu);
}
inline uint16_t BlipGeneration(int32_t handle) {
	return static_cast<uint16_t>((static_cast<uint32_t>(handle) >> 16) & 0xFFFFu);
}

// A handle CoopIII is willing to hand back to the engine. The generation has
// to be non-zero because GetNewUniqueBlipIndex only ever returns 1 or more,
// so a zero one is a handle that never came from it.
inline bool BlipHandleUsable(int32_t handle) {
	if (handle == NO_BLIP)
		return false;
	return BlipSlot(handle) < static_cast<int>(NUM_RADAR_BLIPS) &&
	       BlipGeneration(handle) != 0;
}

// ---- is that slot still ours ----------------------------------------------
//
// The whole of the staleness question, as arithmetic over the four fields
// worth reading out of the table. Deliberately does not look at the
// generation: what CoopIII cares about is not "is my handle still valid" but
// "is the thing on the radar still the blip I made for this player", and the
// second question survives CRadar::Initialise, a savegame load, a replay
// restore and the script recycling the slot, all of which the first gets
// wrong in the dangerous direction.
//
// entityHandle is the clincher. It is CPools::GetPedRef's ref for our own
// ped, and nothing else in the game knows that number, so a BLIP_CHAR
// carrying it in a slot marked in use is ours and nothing else can be.
inline bool TraceIsOurs(bool inUse, uint32_t blipType, int32_t entityHandle,
                        int32_t ourPedRef) {
	return inUse && blipType == BLIP_CHAR && entityHandle == ourPedRef &&
	       ourPedRef >= 0;
}

// Does one table entry look like something CRadar wrote? Used once, over all
// 32 slots, before CoopIII writes anything: if another mod has moved the
// table or the image is not what verify.cpp thought it was, this is the
// cheapest way to find out other than by corrupting it.
//
// A free slot is not inspected beyond m_bInUse, because ClearBlip leaves
// m_nColor, m_wScale and both position vectors untouched - a cleared slot
// keeps the last tenant's colour, so requiring anything of it would fail on a
// perfectly healthy table.
inline bool TraceEntrySane(bool inUse, uint32_t blipType, uint16_t display,
                           uint16_t sprite) {
	if (!inUse)
		return true;
	if (blipType < BLIP_CAR || blipType > BLIP_CONTACT_POINT)
		return false;
	if (display > BLIP_DISPLAY_BOTH)
		return false;
	// eRadarSprite runs 0..20 (RADAR_SPRITE_COUNT is 21), and RadarSprites is
	// indexed by it with no check in DrawRadarSprite.
	return sprite <= RADAR_SPRITE_WEAPON;
}

// ---- what colour, and whether there should be a blip at all ---------------

// Green on foot, red in a car, which is ADD_BLIP_FOR_CHAR's colour and
// ADD_BLIP_FOR_CAR's colour respectively. See the header comment.
inline uint32_t BlipColourFor(bool inVehicle) {
	return inVehicle ? RADAR_TRACE_RED : RADAR_TRACE_GREEN;
}

// Whether a roster slot should have a blip on the radar this frame.
//
// `hasPed` is the one that carries the design: a BLIP_CHAR has nothing to
// track without a ped, so today no ped means no blip. docs/roadmap.md §5.3
// settles that a player beyond the streaming radius will be a map blip and
// nothing else, and when that lands this is the single predicate that has to
// change - see the note at the bottom of this file.
//
// haveState is required for the same reason ped.cpp requires a sampled pose
// before spawning: a player whose first snapshot has not arrived has no
// position, and a blip at the world origin is a blip in the water off
// Portland.
inline bool BlipWanted(bool active, bool haveState, bool hasPed) {
	return active && haveState && hasPed;
}

// ---- when distant players land (docs/roadmap.md §5.3) ---------------------
//
// This is built so that change is a change to BlipWanted and one branch in
// radar.cpp, not a rewrite.
//
// A player outside the streaming radius will have no ped, so their blip
// cannot be a BLIP_CHAR. The engine's own answer for "a blip at a place
// rather than on a thing" is BLIP_COORD, which CRadar::SetCoordBlip
// (0x004A5590, recorded in addresses.h) creates and which DrawBlips draws
// from m_vec2DPos in its third loop - the same square, the same colour table,
// the same rim clamp. Nothing about how the blip looks would change, which is
// the point of having chosen the game's own blip: a player walking out of the
// streaming radius would hand over from an entity blip to a coord blip and
// look identical doing it.
//
// Three things are already in place for that and one is not:
//
//   - the slot accounting, the reserve and the not-trusting-the-handle rule
//     are all blip-kind-agnostic;
//   - the position is already on the wire and already interpolated
//     (RemotePlayer::interp), which is what a coord blip needs and an entity
//     blip does not;
//   - TraceIsOurs is the part that would need company, because a coord blip
//     carries no entity handle to recognise it by. m_nEntityHandle is zero
//     for one, so ownership would have to come from the slot plus the
//     generation, i.e. GetActualBlipArrayIndex, with BlipHandleUsable in
//     front of it. That is why both halves of the handle are already parsed
//     here rather than only where they are used today.
//
// The one genuinely new piece is that a coord blip's position is not written
// by any engine setter - the script only ever creates static ones - so
// CoopIII would have to write m_vec2DPos and m_vecPos itself each frame,
// at TRACE_POS_2D and TRACE_POS. That is a two-line write into a slot it
// owns, and the offsets are already in addresses.h with their witnesses.

} // namespace coopiii::game
