// Nametags over remote players: the weapon they're holding on the left, their
// name above their health on the right.
//
// The design was settled as an HTML mockup before any of this was written, so
// the numbers below are transcriptions rather than inventions. What the mockup
// fixed:
//
//   - the weapon icon comes out of the game's own hud.txd sprite set, indexed
//     by weapon type, not drawn by hand;
//   - the font is CFont/FONT_HEADING, the face the clock, the money and the
//     health counter are drawn in;
//   - health is a number with the heart beside it, never a bar;
//   - the colours are the CRGBA constants the game hands to CFont::SetColor,
//     not colours picked off a screenshot.
//
// Everything in this header is pure arithmetic over those numbers, so it
// builds and runs outside GTA III and tools/clienttest covers it. The half
// that touches game memory is nametag.cpp, and it holds to the same rule as
// the rest of the game layer: no address or offset that hasn't been confirmed
// against the disassembly.
#pragma once

#include "addresses.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>

namespace coopiii {
class Client;
}

namespace coopiii::game {

// ---- installing -----------------------------------------------------------

// Detours CHud::Draw. The tags are drawn from there because that is the one
// point in the frame where the world is already on screen, the 2D render
// state is set up, CFont has been given its per-frame sprite banks, and the
// game's own CFont::DrawFonts is about to run and flush whatever text is in
// them. `client` is only ever read, and only on the game thread.
bool InstallNametags(const Client &client);

// How prominent a nametag should be, as a multiple of the built-in size
// (CoopIII.ini, nametagScale). Multiplies the one unit every length in
// MeasureTag is derived from, so the whole tag grows together and none of
// its internal proportions move. Call before InstallNametags.
void SetNametagScale(float scale);
void RemoveNametags();
bool NametagsInstalled();

// ---- how big, and how solid ------------------------------------------------
//
// One curve for size and one for opacity, both straight off the mockup.
// Distance is the view-space depth CSprite::CalcScreenCoors hands back, which
// saves having to find the camera.

constexpr float TAG_NEAR_M      = 6.0f;     // full size at arm's length
constexpr float TAG_FAR_M       = 140.0f;
constexpr float TAG_NEAR_SCALE  = 1.15f;
constexpr float TAG_SCALE_FALL  = 0.82f;
constexpr float TAG_MIN_SCALE   = 0.32f;

// Past this there is nothing to label. A player outside the streaming radius
// is a blip and nothing else (docs/roadmap.md §5.3), so the tag has to be gone
// before the ped is, or it pops. The band is what makes it a handover instead.
// Once the streaming policy grows a real radius, this is still the number that
// has to follow it.
//
// 50 m, down from 90. The band keeps the same fifth of the range it had at 90,
// so a tag reads as solid for the first 40 m and spends the last 10 going. Two
// reasons for that proportion rather than a fixed band: it's the shape of the
// curve that was already looked at and agreed, only the scale changed, and a
// band that stayed at 18 would have started fading at 32, with a third of the
// useful range spent half transparent.
constexpr float TAG_RANGE_M     = 50.0f;
constexpr float TAG_FADE_BAND_M = 10.0f;

struct TagPlan {
	bool    draw  = false;
	float   scale = 0.0f;   // 1.0 is the size the tag is drawn at 6 m
	uint8_t alpha = 0;
};

inline TagPlan PlanTag(float depth) {
	TagPlan plan;
	// Behind the camera, or past the point where there's a ped to label.
	if (!(depth > 0.0f) || depth >= TAG_RANGE_M)
		return plan;

	float t = (depth - TAG_NEAR_M) / (TAG_FAR_M - TAG_NEAR_M);
	if (t < 0.0f)
		t = 0.0f;
	// The size curve is untouched by the range change, so a player at a given
	// distance is exactly the size they always were. With the range at 50 the
	// curve only ever runs from 1.15 down to about 0.88 and never reaches the
	// floor; the floor stays because it belongs to the curve, not to the range.
	const float scale = TAG_NEAR_SCALE - t * TAG_SCALE_FALL;
	plan.scale        = scale < TAG_MIN_SCALE ? TAG_MIN_SCALE : scale;

	float opacity = 1.0f;
	if (depth > TAG_RANGE_M - TAG_FADE_BAND_M)
		opacity = (TAG_RANGE_M - depth) / TAG_FADE_BAND_M;
	if (opacity < 0.0f)
		opacity = 0.0f;

	plan.alpha = static_cast<uint8_t>(opacity * 255.0f + 0.5f);
	plan.draw  = plan.alpha > 0;
	return plan;
}

// ---- behind walls ----------------------------------------------------------
//
// A tag is hidden when the player it belongs to can't be seen. The test is a
// line of sight from the camera to the player, through CWorld, and the reason
// this needs any thought at all is that the test is the expensive part of the
// whole feature. Three things keep it cheap, and all three are here rather
// than in the .cpp because they're arithmetic and tools/clienttest can hold
// them to account.
//
// 1. Nothing is tested until it has already survived the free gates. Off the
//    side of the screen, behind the camera and past the range all cost a
//    compare, and a player who fails one of those never costs a ray.
//
// 2. One ray per frame, round robin. Eight players do not mean eight rays;
//    they mean a full lobby of seven other people retests each of them every
//    seventh frame, about 120 ms at 60 FPS, and a quieter session retests more
//    often because there is less to share the ray between. Whether somebody is
//    behind a pillar is not a question that needs answering sixty times a
//    second, and the answer from last time is kept for everyone not tested
//    this frame.
//
// 3. Nothing snaps. The answer drives a 0..1 value that takes a fifth of a
//    second to travel end to end, and that value multiplies the alpha the
//    distance curve already produced. There's one alpha path, not two.
//
// The fade being slower than the round robin is the part that matters on
// screen. Walking behind a lamp post blocks the line for a tenth of a second,
// which is often less than the gap between two tests of the same player, so it
// frequently isn't sampled at all. When it is, the tag dips partway and comes
// straight back up rather than blinking out. Only something that stays in the
// way long enough to be caught twice running takes a tag all the way down.

// End to end, either direction.
constexpr uint32_t TAG_OCCLUSION_FADE_MS = 220;

// Rays per frame across the whole roster.
constexpr int TAG_PROBE_BUDGET = 1;

// The frame gap the fade is willing to believe. Without this, coming back from
// a loading screen or a menu would step the fade by however long that took and
// every tag would snap to its target, which is the one thing the fade exists
// to stop.
constexpr uint32_t TAG_MAX_STEP_MS = 100;

// The ray is aimed at the top of the ped's collision box. The question worth
// asking is whether you can see the player, not whether you can see the empty
// air their name floats in, and the two differ exactly where it matters:
// somebody standing behind a low wall with their head and shoulders showing.
//
// This used to sit below the tag's own anchor, which was 20 cm higher. The
// anchor has since come down to meet it, for the reasons at TAG_HEAD_Z_M, so
// the ray and the tag now point at the same spot. Kept as its own name because
// they are two different questions that happen to share an answer.
constexpr float TAG_SIGHT_Z_M = 0.90f;

// Which slots to spend this frame's rays on. Round robin from `cursor` over
// the slots `eligible` marks, at most `budget` of them, writing slot indices
// into `out` and leaving `cursor` just past the last one taken.
//
// Round robin over slot index rather than over a list of this frame's
// candidates, because the candidate list churns as people walk on and off
// screen and a cursor into it would keep landing on whoever happened to be
// first. Slot index is a player id and it doesn't move.
inline int PlanProbes(const bool *eligible, int count, int budget, uint8_t &cursor,
                      uint8_t *out) {
	if (count <= 0 || budget <= 0)
		return 0;
	if (cursor >= count)
		cursor = 0;

	int picked = 0;
	for (int step = 0; step < count && picked < budget; ++step) {
		const int slot = (cursor + step) % count;
		if (!eligible[slot])
			continue;
		out[picked++] = static_cast<uint8_t>(slot);
		cursor        = static_cast<uint8_t>((slot + 1) % count);
	}
	return picked;
}

// Moves a tag's 0..1 visibility towards whatever the last ray said.
inline float StepVisibility(float current, bool clear, uint32_t deltaMs) {
	if (deltaMs > TAG_MAX_STEP_MS)
		deltaMs = TAG_MAX_STEP_MS;

	const float step = static_cast<float>(deltaMs) /
	                   static_cast<float>(TAG_OCCLUSION_FADE_MS);
	float next = clear ? current + step : current - step;
	if (next < 0.0f)
		next = 0.0f;
	if (next > 1.0f)
		next = 1.0f;
	return next;
}

// What the distance curve wanted, taken down by how much of the player can be
// seen. One alpha, arrived at in two steps.
inline uint8_t TagAlpha(uint8_t distanceAlpha, float visible) {
	const float a = static_cast<float>(distanceAlpha) * visible;
	if (a <= 0.0f)
		return 0;
	if (a >= 255.0f)
		return 255;
	return static_cast<uint8_t>(a + 0.5f);
}

// ---- the layout ------------------------------------------------------------
//
// Every length a tag needs comes out of one number, the screen height, and
// everything else here is a ratio. That is deliberate and it took a run in the
// game to arrive at, so the reasoning is worth writing down.
//
// The first version worked in the HUD's own 640x448 grid, scaling x by
// screenWidth/640 and y by screenHeight/448, because that is what every
// SCREEN_SCALE_* in Hud.cpp does. Two things were wrong with that.
//
// It was about three times too big. The sizes were taken from the design
// mockup as a ratio against the mockup's drawing of the HUD, but the mockup
// drew the HUD much smaller relative to its own viewport than GTA III draws it
// relative to a real screen, so the ratio did not carry across. A tag ended up
// nearly the size of the game's own health readout, which for a label floating
// over somebody's head is enormous.
//
// The second thing is the one that matters more. Scaling x by width and y by
// height stretches the letters as the screen gets wider, and this install has
// ThirteenAG's Widescreen Fix precisely to stop the HUD doing that. Its ini
// replaces the game's HUD scale factors with its own (HudWidthScale, whose
// original it gives as 1.0, and HudHeightScale, original 1.0714285, which is
// 480/448), so on this machine the corner HUD is no longer drawn with the
// formula in Hud.cpp at all. Copying that formula would have applied an
// aspect correction the fix had already made, a second time and in the wrong
// direction.
//
// So a tag does not use the HUD's scaling path, and does not read the fix's
// numbers either. Both axes come from screen height alone. That gives the
// three properties that were actually asked for:
//
//   - the letters keep their shape at any aspect ratio, because nothing here
//     ever looks at the screen width;
//   - a tag is the same fraction of the screen at 800x600 and at 4K;
//   - it keeps a fixed ratio to the game's own health readout at every
//     resolution, because that readout is also proportional to screen height
//     under both the stock formula and the fix's. Change resolution and the
//     two move together, which is the test for getting this right.
//
// The one thing it deliberately does not follow is the player's own
// HudWidthScale/HudHeightScale sliders. Those are a preference about how big
// the corner HUD should be, not about how big a label over somebody's head
// should be.

// The shape of one glyph, which is the HUD's own FONT_HEADING pair. Carried as
// a ratio rather than as a scale: these two set how tall a letter is against
// how wide, and the size comes from the multiplier below.
constexpr float TAG_FONT_SHAPE_X = 0.80f;
constexpr float TAG_FONT_SHAPE_Y = 1.35f;

// How big the two lines are. 0.28 puts a name at roughly two percent of the
// screen height and about a tenth of the on-screen height of the player it
// belongs to, at ten metres. The health line keeps the 0.79 of the name that
// the mockup drew it at.
constexpr float TAG_NAME_SIZE = 0.28f;
constexpr float TAG_HP_SIZE   = 0.22f;

// The icon is measured against the two lines of text beside it rather than
// being a size of its own, which is what the mockup did: 24 px of icon against
// a 27 px column. It is a little smaller here than the mockup's 0.89, because
// a solid filled sprite reads heavier than thin letters do, and because the
// mockup's glyph cells were square while the game's are not.
constexpr float TAG_ICON_OF_COLUMN = 0.80f;

// Square on screen, always. The game's own weapon icon is SCREEN_SCALE_X(64)
// by SCREEN_SCALE_Y(64), which is stretched wide at 16:9, and stopping exactly
// that is what the Widescreen Fix is installed for.

constexpr float TAG_ICON_GAP_OF_ICON = 0.15f;   // icon to text column
constexpr float TAG_LINE_GAP_OF_NAME = 0.10f;   // name to health
constexpr float TAG_HEAD_GAP_OF_TAG  = 0.35f;   // head to the bottom of the tag

// How far above a ped's origin to put both the tag and the sight ray.
// CTempColModels::Initialise builds every ped's collision box with max z = 0.9
// (retail 0x0041260A writes 0x3F666666 into it, which is re3
// collision/TempColModels.cpp:77), so this is the top of the player's head.
//
// It used to be 1.10, leaving 20 cm of air above the head before the tag's own
// clearance was added on top. With a tag a third of the size that reads as
// floating rather than sitting above somebody, and 20 cm of world space is the
// wrong way to express a gap anyway: it grows on screen as you walk closer.
// The clearance is TAG_HEAD_GAP_OF_TAG instead, in screen pixels, as a
// fraction of the tag's own height, so it looks the same at every distance.
constexpr float TAG_HEAD_Z_M = 0.90f;

// The drop shadow is the HUD's own idiom, not CFont::SetDropShadowPosition:
// DrawHealth and DrawMoney both print the string once in black at +2 px and
// then again in colour. CFont::InitPerFrame resets dropShadowPosition to 0
// every frame anyway, so doing it the HUD's way leaves no state behind. A
// fraction of the name rather than the HUD's flat 2 px, so it doesn't turn
// into a smear at 4K or vanish at 800x600.
constexpr float TAG_SHADOW_OF_NAME = 0.09f;
constexpr float TAG_SHADOW_MIN_PX  = 1.0f;

// Every length a tag needs, in screen pixels. One screen dimension in, so
// there is nothing in here that can disagree with itself.
struct TagMetrics {
	float nameScaleX = 0.0f, nameScaleY = 0.0f;
	float hpScaleX = 0.0f, hpScaleY = 0.0f;
	float nameH = 0.0f, hpH = 0.0f, lineGap = 0.0f;
	float columnH = 0.0f;
	float icon = 0.0f, iconGap = 0.0f;   // icon is square, so one number
	float headGap = 0.0f;
	float shadow = 0.0f;
};

inline TagMetrics MeasureTag(float screenHeight, float distanceScale) {
	TagMetrics m;

	// One HUD reference unit in pixels, from height and nothing else.
	const float unit = screenHeight * (1.0f / HUD_REF_HEIGHT) * distanceScale;

	m.nameScaleX = TAG_FONT_SHAPE_X * TAG_NAME_SIZE * unit;
	m.nameScaleY = TAG_FONT_SHAPE_Y * TAG_NAME_SIZE * unit;
	m.hpScaleX   = TAG_FONT_SHAPE_X * TAG_HP_SIZE * unit;
	m.hpScaleY   = TAG_FONT_SHAPE_Y * TAG_HP_SIZE * unit;

	m.nameH   = FONT_CELL_HEIGHT * m.nameScaleY;
	m.hpH     = FONT_CELL_HEIGHT * m.hpScaleY;
	m.lineGap = TAG_LINE_GAP_OF_NAME * m.nameH;
	m.columnH = m.nameH + m.lineGap + m.hpH;

	m.icon    = TAG_ICON_OF_COLUMN * m.columnH;
	m.iconGap = TAG_ICON_GAP_OF_ICON * m.icon;

	m.headGap = TAG_HEAD_GAP_OF_TAG * (m.columnH > m.icon ? m.columnH : m.icon);

	m.shadow = TAG_SHADOW_OF_NAME * m.nameH;
	if (m.shadow < TAG_SHADOW_MIN_PX)
		m.shadow = TAG_SHADOW_MIN_PX;

	return m;
}

// ---- the colours -----------------------------------------------------------
//
// re3 src/render/Hud.cpp:80-96 collects the CRGBA constants the game inlines
// into CFont::SetColor calls. Two of the three below are also witnessed in the
// retail image, which is a better check than trusting the decompilation:
//
//   HEALTH_COLOR(186, 101, 50)       0x005066B3: push 0BAh / 65h / 32h / 0FFh
//   WASTEDBUSTED_COLOR(170, 123, 87) 0x00508F40: push 0AAh / 7Bh / 57h / 0FFh
//
// The off-white the name is drawn in is the mockup's, not one of Hud.cpp's
// constants. GTA III has no "player name" colour to borrow, because it never
// had player names.

struct TagColor {
	uint8_t r, g, b;
};

constexpr TagColor TAG_NAME_COLOR   {233, 230, 222};
constexpr TagColor TAG_HEALTH_COLOR {186, 101, 50};
constexpr TagColor TAG_WASTED_COLOR {170, 123, 87};
constexpr TagColor TAG_SHADOW_COLOR {0, 0, 0};

// GTA III does not recolour health as it drops. It flashes it instead, off
// CHud::m_ItemToFlash, and that flag is about the local player's own HUD. So
// one colour, always, which is also what the mockup settled on.
inline TagColor TagNameColor(float health) {
	return health > 0.0f ? TAG_NAME_COLOR : TAG_WASTED_COLOR;
}
inline TagColor TagHealthColor(float health) {
	return health > 0.0f ? TAG_HEALTH_COLOR : TAG_WASTED_COLOR;
}

// ---- the strings -----------------------------------------------------------

// In FONT_HEADING this is the heart the HUD prints beside the health number.
// CHud::Draw builds it as a one-character string at 0x005064E2, and the byte
// it writes is 0x7B.
constexpr char TAG_HEART = '{';

// Longest nickname a tag will draw. Anything longer is cut rather than shrunk,
// because a tag that changes width with the name is a tag that moves.
constexpr size_t TAG_NAME_MAX = 20;

// One character, made safe for CFont.
//
// Three things can go wrong with a nickname off the wire. CFont indexes
// Size[style][c - ' '], an array of 193, so anything outside printable ASCII
// reads past it. '~' opens a formatting token, which eats the rest of the
// string and can change the colour. And CFont::PrintString returns without
// drawing anything at all if the first character is '*' (cmp word [esi],2Ah
// at 0x00500F64), so one player could make their own tag invisible.
//
// Uppercasing is the fourth thing, and it's a choice rather than a fix: this
// font's lowercase is a different style from its uppercase, and every other
// piece of text in GTA III's HUD is caps.
inline char TagGlyph(char c) {
	if (c >= 'a' && c <= 'z')
		return static_cast<char>(c - 'a' + 'A');
	if (c == '~')
		return '-';
	if (c == '*')
		return '+';
	if (c < ' ' || c > '~')
		return '?';
	return c;
}

// Writes at most `size - 1` glyphs plus a terminator. An empty or unusable
// nickname still gets something drawable, since a tag with no name is just a
// floating weapon icon.
inline void TagName(const char *nick, char *out, size_t size) {
	if (size == 0)
		return;
	size_t limit = size - 1;
	if (limit > TAG_NAME_MAX)
		limit = TAG_NAME_MAX;

	size_t n = 0;
	for (; nick && nick[n] != '\0' && n < limit; ++n)
		out[n] = TagGlyph(nick[n]);
	if (n == 0 && limit >= 1) {
		out[n++] = '?';
	}
	out[n] = '\0';
}

// "{ 64", or the word when there's nothing left. A living player never reads
// 0: rounding 0.4 down to nothing says dead when they aren't.
inline void TagHealth(float health, char *out, size_t size) {
	if (size == 0)
		return;
	if (!(health > 0.0f)) {
		std::snprintf(out, size, "WASTED");
		return;
	}
	int hp = static_cast<int>(health + 0.5f);
	if (hp < 1)
		hp = 1;
	if (hp > 999)
		hp = 999;
	std::snprintf(out, size, "%c %d", TAG_HEART, hp);
}

// ---- eight of them at once -------------------------------------------------
//
// This is where an overlay like this usually falls apart: a co-op squad stands
// in a doorway together and the tags turn into one unreadable pile. Two things
// stop that, and both are cheap. Tags are laid out nearest first, so the
// player closest to you keeps the spot directly over their head. Anything that
// would land on top of an already placed tag gets lifted clear of it instead.

struct TagBox {
	float left = 0.0f, right = 0.0f, top = 0.0f, bottom = 0.0f;
};

inline bool TagBoxesOverlap(const TagBox &a, const TagBox &b, float pad) {
	return a.left < b.right + pad && b.left < a.right + pad &&
	       a.top < b.bottom + pad && b.top < a.bottom + pad;
}

// How far up `box` has to move to clear every box already placed. Screen y
// grows downwards, so the return value is subtracted from both edges.
//
// The loop runs at most once per placed box: each pass either finishes or
// clears one of them, and lifting can only ever move a box further from the
// ones below it.
inline float TagLift(const TagBox &box, const TagBox *placed, int count, float pad) {
	float lift = 0.0f;
	for (int pass = 0; pass <= count; ++pass) {
		const TagBox at{box.left, box.right, box.top - lift, box.bottom - lift};
		bool         moved = false;
		for (int i = 0; i < count; ++i) {
			if (!TagBoxesOverlap(at, placed[i], pad))
				continue;
			lift += (at.bottom - placed[i].top) + pad;
			moved = true;
			break;
		}
		if (!moved)
			break;
	}
	return lift;
}

} // namespace coopiii::game
