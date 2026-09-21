// Drawing the nametags. nametag.h has the design and every number this file
// uses; what's here is the part that touches game memory.
//
// ---------------------------------------------------------------------------
// Why the draw hangs off CHud::Draw
// ---------------------------------------------------------------------------
//
// A tag is a HUD element that happens to be positioned from a world point, so
// it belongs wherever the rest of the HUD is drawn and nowhere else. The frame
// pump can't do it: PreFrame and PostFrame bracket CGame::Process, which is
// the simulation step, and the frame isn't drawn until well after PostFrame
// returns. Anything painted from there is painted over by the world.
//
// CHud::Draw is called from exactly one place, Render2dStuff (0x0048E420), and
// by the time it runs four things are already true that this code would
// otherwise have to arrange for itself:
//
//   - the world has been rendered, so a 2D overlay lands on top of it;
//   - Render2dStuff has set the 2D render state (z-test and z-write off,
//     vertex alpha on, src-alpha/inv-src-alpha blending, culling off);
//   - CFont::InitPerFrame has handed CFont its sprite banks for this frame,
//     from inside CGame::Process;
//   - CFont::DrawFonts runs four calls after CHud::Draw returns, so text
//     queued here is flushed by the game's own code and nothing has to be
//     flushed by hand.
//
// The tags go in *before* the original runs, which puts the local player's own
// HUD on top of them. Someone else's name should never cover your health.
//
// ---------------------------------------------------------------------------
// What it will not draw
// ---------------------------------------------------------------------------
//
// The same three tests CHud::Draw opens with, because a tag is HUD: the player
// turned the HUD off with the second pad, the camera is in widescreen (which
// is what a cutscene looks like from here), or the tag has nothing to say.
//
// Plus anything the camera cannot see. That one is not free, so it is rationed
// and smoothed rather than asked every frame; nametag.h has the scheduling and
// the reasoning, and it is arithmetic so tools/clienttest holds it to account.
//
// Plus the pause menu. CoopIII keeps the world running while the menu is up
// and lets only the presentation half of the engine see a pause, and
// CHud::Draw is one of the four readers on that side of the split
// (game/pause.h). A tag is presentation drawn from inside CHud::Draw, so it
// takes the same side: while the menu is up, the HUD behaves as it does on the
// pause screen. The frontend is drawn over the top of all this afterwards
// anyway, so the only thing tags could do there is peek out around the edges
// of the menu.
//
// ---------------------------------------------------------------------------
// This pass never changes anything
// ---------------------------------------------------------------------------
//
// It resolves each player's ped read-only. ped.cpp's ResolveRemote clears the
// handle and re-arms the spawn when a ped has gone, which is the right thing
// for the network path and the wrong thing here: a drawing pass that decides a
// ped is lost is a drawing pass deciding when a player respawns. If the ped
// isn't there this frame, there's no tag this frame, and PreFrame sorts it out
// on its own schedule.
#include "nametag.h"

#include "client.h"
#include "hook/hook.h"
#include "log.h"

#include <cstdint>
#include <cstring>

namespace coopiii::game {

namespace {

// CVector / RwV3d, and CRGBA. CRect's member order is the one documented in
// addresses.h: left, bottom, right, top, which is not the order its
// constructor takes them in.
struct Vec3f {
	float x, y, z;
};
struct Rgba {
	uint8_t r, g, b, a;
};
struct Rect {
	float left, bottom, right, top;
};

using HudDrawFn    = void(__cdecl *)();
using CalcCoorsFn  = int(__cdecl *)(const Vec3f *, Vec3f *, float *, float *, int);
using GetPedFn     = void *(__cdecl *)(int32_t);
using LineOfSightFn = int(__cdecl *)(const Vec3f *, const Vec3f *, int, int, int, int,
                                     int, int, int);
using SpriteDrawFn = void(__thiscall *)(void *, const Rect *, const Rgba *, float,
                                        float, float, float, float, float, float,
                                        float);
using FontScaleFn  = void(__cdecl *)(float, float);
using FontFloatFn  = void(__cdecl *)(float);
using FontIntFn    = void(__cdecl *)(int);
using FontVoidFn   = void(__cdecl *)();
using FontColorFn  = void(__cdecl *)(const Rgba *);
using FontPrintFn  = void(__cdecl *)(float, float, const uint16_t *);
using FontWidthFn  = float(__cdecl *)(const uint16_t *, int);

Detour        g_hud;
const Client *g_client = nullptr;

// What the last ray said about each player, and how far the tag has got
// towards believing it. Kept here rather than on RemotePlayer because it is
// nothing to do with the roster: it is a property of where this machine's
// camera happens to be standing.
struct Sight {
	float visible = 1.0f;   // 0 hidden, 1 shown
	bool  clear   = true;   // the last answer, reused on the frames in between
};
Sight    g_sight[MAX_PLAYERS];
uint8_t  g_probeCursor = 0;
uint32_t g_lastDrawMs  = 0;

// Whether the camera can see a point, through buildings and the props the
// level says are allowed to block a view.
//
//   buildings  yes. This is the whole feature.
//   vehicles   no. A car that drives between you and a team-mate would blink
//              their name off and on, and a car is not a wall.
//   peds       no. A crowd would hide everyone in it, including each other.
//   objects    yes, but see ignoreSomeObjects. Crates, skips and fences are
//              objects, and standing behind one is standing behind something.
//   dummies    no, which is what the camera's own tests do.
//   ignoreSeeThrough        yes. A name should not be stopped by glass or a
//                           railing you can see the player through.
//   ignoreSomeObjects       yes. It skips temporary objects and anything the
//                           level flagged as not allowed to block a view, so
//                           the props the designers already decided should not
//                           get in the way do not get in the way here either.
//
// One thing worth knowing about where this is called from: it advances
// CWorld::ms_nCurrentScanCode, the same counter CRenderer::ScanSectorList
// uses. That is safe here and only here, because this runs from CHud::Draw and
// the frame's render scan finished before Render2dStuff started. The engine
// advances the same counter from 57 other places anyway.
bool CanCameraSee(const Vec3f &target) {
	void *const camera = Ptr<void>(TheCamera);
	const Vec3f from{Field<float>(camera, offs::POSITION + 0),
	                 Field<float>(camera, offs::POSITION + 4),
	                 Field<float>(camera, offs::POSITION + 8)};

	return Func<LineOfSightFn>(CWorld__GetIsLineOfSightClear)(
	           &from, &target,
	           /*buildings*/ 1, /*vehicles*/ 0, /*peds*/ 0, /*objects*/ 1,
	           /*dummies*/ 0, /*ignoreSeeThrough*/ 1,
	           /*ignoreSomeObjects*/ 1) != 0;
}

// ---- thin wrappers, named after the engine functions they are -------------

void FontStyle(int style) { Func<FontIntFn>(CFont__SetFontStyle)(style); }
void FontScale(float x, float y) { Func<FontScaleFn>(CFont__SetScale)(x, y); }
void FontColor(const Rgba &c) { Func<FontColorFn>(CFont__SetColor)(&c); }
void FontPrint(float x, float y, const uint16_t *s) {
	Func<FontPrintFn>(CFont__PrintString)(x, y, s);
}
// The second argument has to be true. GetStringWidth's loop condition is
// `(*s != ' ' || spaces) && *s != '\0'`, so with it false the measurement
// stops dead at the first space, and the health line is a heart, a space and
// a number. Passing false would have laid every tag out as if the health read
// just "{".
float FontWidth(const uint16_t *s) {
	return Func<FontWidthFn>(CFont__GetStringWidth)(s, 1);
}

// CFont has no reset and no save/restore, so the engine's own convention is
// that every printer sets the whole of it before printing and leaves it
// however it likes afterwards. This follows that convention, with one
// exception, handled by the caller: wrapX. Everything set here is a value the
// game's own HUD sets constantly, but wrapX has to go somewhere no engine
// printer would put it, so the caller puts it back.
//
// Alpha fade is the one that would be easy to miss. SetColor multiplies it
// into whatever colour it's handed, and CHud::Draw leaves it at whatever its
// zone-name fade last worked out, so a tag drawn without setting it would fade
// with the zone name.
//
// Slant is deliberately not touched. CFont::SetSlant has exactly one caller in
// the whole image, CFont::Initialise, which sets it to zero.
void FontStateForTags(float screenWidth) {
	Func<FontVoidFn>(CFont__SetBackgroundOff)();
	Func<FontVoidFn>(CFont__SetBackGroundOnlyTextOff)();
	Func<FontVoidFn>(CFont__SetJustifyOff)();
	Func<FontVoidFn>(CFont__SetCentreOff)();
	Func<FontVoidFn>(CFont__SetRightJustifyOff)();
	Func<FontFloatFn>(CFont__SetRightJustifyWrap)(0.0f);
	Func<FontFloatFn>(CFont__SetCentreSize)(screenWidth);
	// PrintString breaks the line the moment x passes wrapX, and
	// CFont::Initialise leaves it at a flat 640 whatever the resolution is. A
	// tag on the right-hand side of a 1920-wide screen would come out one
	// glyph per line.
	Func<FontFloatFn>(CFont__SetWrapx)(screenWidth * 4.0f);
	Func<FontFloatFn>(CFont__SetAlphaFade)(FONT_ALPHA_OPAQUE);
	Func<FontVoidFn>(CFont__SetPropOn)();
	FontStyle(FONT_HEADING);
}

// CFont strings are uint16 per character (re3 `typedef uint16 wchar`), and
// PrintString reads them a word at a time.
void Widen(const char *src, uint16_t *dst, size_t size) {
	size_t n = 0;
	for (; src != nullptr && src[n] != '\0' && n + 1 < size; ++n)
		dst[n] = static_cast<uint8_t>(src[n]);
	dst[n] = 0;
}

// The HUD's own drop shadow: once in black, then once in colour on top.
void PrintShadowed(float x, float y, const uint16_t *s, const TagColor &c,
                   uint8_t alpha, float shadow) {
	const Rgba black{TAG_SHADOW_COLOR.r, TAG_SHADOW_COLOR.g, TAG_SHADOW_COLOR.b,
	                 alpha};
	FontColor(black);
	FontPrint(x + shadow, y + shadow, s);

	const Rgba ink{c.r, c.g, c.b, alpha};
	FontColor(ink);
	FontPrint(x, y, s);
}

// A remote player's live CPed, without touching the roster. See the header
// comment on why this doesn't call ped.cpp's ResolveRemote.
void *LivePed(const RemotePlayer &player) {
	if (!player.active || player.poolHandle < 0)
		return nullptr;
	void *ped = Func<GetPedFn>(CPools__GetPed)(player.poolHandle);
	if (ped == nullptr)
		return nullptr;
	if (Field<uintptr_t>(ped, offs::VTABLE) != CCivilianPed__vtable)
		return nullptr;
	return ped;
}

// Everything worked out about one tag before anything is drawn, so the whole
// set can be sorted and pulled apart first.
struct Tag {
	uint8_t playerId = 0;
	float   depth   = 0.0f;
	float   centreX = 0.0f;   // over the ped's head, in screen pixels
	float   baseY   = 0.0f;   // the bottom of the tag
	float   scale   = 0.0f;
	uint8_t alpha   = 0;
	uint8_t weapon  = 0;
	bool    icon    = false;
	float   health  = 0.0f;

	// Where a line of sight ray would be aimed for this player, kept so the
	// probe pass doesn't have to go back to the ped for it.
	Vec3f sightTo{};

	uint16_t name[TAG_NAME_MAX + 1]{};
	uint16_t hp[16]{};

	float nameScaleX = 0.0f, nameScaleY = 0.0f;
	float hpScaleX = 0.0f, hpScaleY = 0.0f;
	float nameW = 0.0f, hpW = 0.0f;
	float nameH = 0.0f, hpH = 0.0f;
	float iconW = 0.0f, iconH = 0.0f, iconGap = 0.0f;
	float lineGap = 0.0f;

	TagBox box;
};

bool ShouldDrawAtAll() {
	if (g_client == nullptr || !g_client->IsConnected())
		return false;
	// The player hid the HUD with the second pad's Start.
	if (Global<uint8_t>(CHud__m_Wants_To_Draw_Hud) == 0)
		return false;
	// Widescreen bars are up, which from here is a cutscene.
	if (Global<uint8_t>(TheCamera + CAMERA_WIDESCREEN_ON) != 0)
		return false;
	// The menu is up. See the header comment.
	if (Global<uint8_t>(CMenuManager__m_bMenuActive) != 0)
		return false;
	// The font this draws in is CFont::Sprite[FONT_HEADING], the "font1"
	// texture out of fonts.txd. Without it CFont would queue untextured white
	// blocks rather than letters.
	if (Global<void *>(CFont__Sprite + FONT_HEADING * SIZEOF_SPRITE2D) == nullptr)
		return false;
	return true;
}

void DrawIcon(const Tag &tag) {
	void *sprite = Ptr<void>(CHud__Sprites + tag.weapon * SIZEOF_SPRITE2D);
	if (Field<void *>(sprite, 0) == nullptr)
		return;

	const float left = tag.box.left;
	const float top  = tag.box.top + (tag.box.bottom - tag.box.top - tag.iconH) * 0.5f;
	const Rect  rect{left, top + tag.iconH, left + tag.iconW, top};
	const Rgba  white{255, 255, 255, tag.alpha};

	Func<SpriteDrawFn>(CSprite2d__Draw)(sprite, &rect, &white, HUD_ICON_U0,
	                                    HUD_ICON_V0, HUD_ICON_U1, HUD_ICON_V1,
	                                    HUD_ICON_U2, HUD_ICON_V2, HUD_ICON_U3,
	                                    HUD_ICON_V3);
}

void DrawText(const Tag &tag) {
	const float colH  = tag.nameH + tag.lineGap + tag.hpH;
	const float textX = tag.box.left + (tag.icon ? tag.iconW + tag.iconGap : 0.0f);
	const float textY = tag.box.top + (tag.box.bottom - tag.box.top - colH) * 0.5f;

	float shadow = TAG_SHADOW_PX * tag.scale;
	if (shadow < TAG_SHADOW_MIN_PX)
		shadow = TAG_SHADOW_MIN_PX;

	FontScale(tag.nameScaleX, tag.nameScaleY);
	PrintShadowed(textX, textY, tag.name, TagNameColor(tag.health), tag.alpha,
	              shadow);

	FontScale(tag.hpScaleX, tag.hpScaleY);
	PrintShadowed(textX, textY + tag.nameH + tag.lineGap, tag.hp,
	              TagHealthColor(tag.health), tag.alpha, shadow);
}

void DrawTags() {
	if (!ShouldDrawAtAll())
		return;

	const float screenW = static_cast<float>(Global<int32_t>(RsGlobal__maximumWidth));
	const float screenH = static_cast<float>(Global<int32_t>(RsGlobal__maximumHeight));
	if (!(screenW > 0.0f) || !(screenH > 0.0f))
		return;

	// The HUD works in a 640x448 grid and scales out of it, so everything
	// laid out here does too.
	const float sx = screenW / HUD_REF_WIDTH;
	const float sy = screenH / HUD_REF_HEIGHT;

	const float wrapWas = Global<float>(CFont__Details + FONTDETAILS_WRAPX);
	FontStateForTags(screenW);

	Tag tags[MAX_PLAYERS];
	int count = 0;

	for (uint8_t id = 0; id < MAX_PLAYERS && count < MAX_PLAYERS; ++id) {
		if (id == g_client->LocalPlayerId())
			continue;
		const RemotePlayer &player = g_client->PlayerSlot(id);
		if (!player.active) {
			// An empty slot keeps no opinion about walls, so whoever joins
			// into it next starts visible instead of inheriting the last
			// tenant's fade. A player who is merely off screen keeps theirs:
			// walking back into view from behind a wall should not flash.
			g_sight[id] = Sight{};
			continue;
		}
		if (!player.haveState)
			continue;

		void *ped = LivePed(player);
		if (ped == nullptr)
			continue;   // no ped to label: blip only, docs/roadmap.md §5.3

		// The ped's own position, not the interpolated one off the wire. For
		// somebody sitting in a car the engine puts the ped in the seat every
		// frame and the stream stops writing their position at all, so the ped
		// is the only thing that knows where they are.
		const Vec3f head{Field<float>(ped, offs::POSITION + 0),
		                 Field<float>(ped, offs::POSITION + 4),
		                 Field<float>(ped, offs::POSITION + 8) + TAG_HEAD_Z_M};

		Vec3f screen{};
		float w = 0.0f, h = 0.0f;
		// Behind the camera or past the far clip and this returns false, which
		// is the whole of the "player behind you" case handled by the engine's
		// own arithmetic instead of ours.
		if (!Func<CalcCoorsFn>(CSprite__CalcScreenCoors)(&head, &screen, &w, &h, 1))
			continue;

		const TagPlan plan = PlanTag(screen.z);
		if (!plan.draw)
			continue;

		// Well off the side of the screen. CFont::PrintChar drops glyphs
		// outside the viewport on its own, so the margin only has to be
		// generous enough not to clip a tag that is partly visible.
		if (screen.x < -screenW * 0.5f || screen.x > screenW * 1.5f ||
		    screen.y < -screenH * 0.5f || screen.y > screenH * 1.5f)
			continue;

		Tag &tag   = tags[count];
		tag        = Tag{};
		tag.playerId = id;
		// `head` is the ped's origin plus the tag's own anchor height, so back
		// that off and put the sight height on instead. The ray goes to the top
		// of the ped, the tag hangs a little above it.
		tag.sightTo = Vec3f{head.x, head.y,
		                    head.z - TAG_HEAD_Z_M + TAG_SIGHT_Z_M};
		tag.depth  = screen.z;
		tag.centreX = screen.x;
		tag.baseY  = screen.y - TAG_HEAD_GAP * plan.scale * sy;
		tag.scale  = plan.scale;
		tag.alpha  = plan.alpha;
		tag.health = player.last.health;
		tag.weapon = player.last.weapon;
		tag.icon   = player.last.weapon <= HUD_SPRITE_LAST_WEAPON;

		char name[TAG_NAME_MAX + 1];
		char hp[16];
		TagName(player.nick.c_str(), name, sizeof(name));
		TagHealth(tag.health, hp, sizeof(hp));
		Widen(name, tag.name, TAG_NAME_MAX + 1);
		Widen(hp, tag.hp, 16);

		tag.nameScaleX = TAG_NAME_SCALE_X * plan.scale * sx;
		tag.nameScaleY = TAG_NAME_SCALE_Y * plan.scale * sy;
		tag.hpScaleX   = TAG_HP_SCALE_X * plan.scale * sx;
		tag.hpScaleY   = TAG_HP_SCALE_Y * plan.scale * sy;
		tag.nameH      = FONT_CELL_HEIGHT * tag.nameScaleY;
		tag.hpH        = FONT_CELL_HEIGHT * tag.hpScaleY;
		tag.iconW      = TAG_ICON_SIZE * plan.scale * sx;
		tag.iconH      = TAG_ICON_SIZE * plan.scale * sy;
		tag.iconGap    = TAG_ICON_GAP * plan.scale * sx;
		tag.lineGap    = TAG_LINE_GAP * plan.scale * sy;

		// GetStringWidth answers in screen pixels at whatever scale is set, so
		// it has to be asked once per line.
		FontScale(tag.nameScaleX, tag.nameScaleY);
		tag.nameW = FontWidth(tag.name);
		FontScale(tag.hpScaleX, tag.hpScaleY);
		tag.hpW = FontWidth(tag.hp);

		++count;
	}

	// Put wrapX back whatever happens from here. Nothing else this touched is
	// a value the game's own HUD doesn't set for itself every frame.
	struct RestoreWrap {
		float was;
		~RestoreWrap() { Func<FontFloatFn>(CFont__SetWrapx)(was); }
	} restoreWrap{wrapWas};

	if (count == 0)
		return;

	// ---- can they be seen ------------------------------------------------
	//
	// Everything in `tags` has already survived the free gates, so this is the
	// smallest set worth spending a ray on. One ray goes out, round robin, and
	// everyone else carries last time's answer.
	bool    eligible[MAX_PLAYERS] = {};
	uint8_t probe[MAX_PLAYERS]    = {};
	for (int i = 0; i < count; ++i)
		eligible[tags[i].playerId] = true;

	const int probes =
	    PlanProbes(eligible, MAX_PLAYERS, TAG_PROBE_BUDGET, g_probeCursor, probe);
	for (int p = 0; p < probes; ++p) {
		for (int i = 0; i < count; ++i) {
			if (tags[i].playerId != probe[p])
				continue;
			g_sight[probe[p]].clear = CanCameraSee(tags[i].sightTo);
			break;
		}
	}

	// Time, not frames, so the fade is the same length at 30 FPS as at 60.
	const uint32_t nowMs = Global<uint32_t>(CTimer__m_snTimeInMilliseconds);
	const uint32_t stepMs = nowMs > g_lastDrawMs ? nowMs - g_lastDrawMs : 0;
	g_lastDrawMs          = nowMs;

	int kept = 0;
	for (int i = 0; i < count; ++i) {
		Sight &sight  = g_sight[tags[i].playerId];
		sight.visible = StepVisibility(sight.visible, sight.clear, stepMs);

		tags[i].alpha = TagAlpha(tags[i].alpha, sight.visible);
		if (tags[i].alpha == 0)
			continue;   // fully behind something, but still being probed
		tags[kept++] = tags[i];
	}
	count = kept;
	if (count == 0)
		return;

	// Nearest first, so the player standing next to you is the one whose tag
	// keeps the spot over their head.
	for (int i = 1; i < count; ++i) {
		Tag key = tags[i];
		int j   = i - 1;
		for (; j >= 0 && tags[j].depth > key.depth; --j)
			tags[j + 1] = tags[j];
		tags[j + 1] = key;
	}

	TagBox placed[MAX_PLAYERS];
	int    placedCount = 0;

	for (int i = 0; i < count; ++i) {
		Tag &tag = tags[i];

		const float colW = tag.nameW > tag.hpW ? tag.nameW : tag.hpW;
		const float colH = tag.nameH + tag.lineGap + tag.hpH;
		const float fullW =
		    colW + (tag.icon ? tag.iconW + tag.iconGap : 0.0f);
		const float fullH = colH > tag.iconH ? colH : tag.iconH;

		tag.box.left   = tag.centreX - fullW * 0.5f;
		tag.box.right  = tag.box.left + fullW;
		tag.box.bottom = tag.baseY;
		tag.box.top    = tag.baseY - fullH;

		const float pad  = 2.0f * tag.scale * sy;
		const float lift = TagLift(tag.box, placed, placedCount, pad);
		tag.box.top -= lift;
		tag.box.bottom -= lift;

		placed[placedCount++] = tag.box;
	}

	// Furthest first, so on anything the lift did not fully separate the
	// nearer player still ends up on top.
	for (int i = count - 1; i >= 0; --i) {
		if (tags[i].icon)
			DrawIcon(tags[i]);
		DrawText(tags[i]);
	}
}

void __cdecl HookedHudDraw() {
	// Same containment as the frame hook: a throw escaping into the engine
	// would unwind through frames that know nothing about C++ exceptions.
	try {
		DrawTags();
	} catch (...) {
		// Once, not sixty times a second. Whatever went wrong will still be
		// going wrong next frame and the log is no use full of one line.
		static bool reported = false;
		if (!reported) {
			reported = true;
			Log("nametag: draw threw; tags may be missing from here on");
		}
	}

	g_hud.Original<HudDrawFn>()();
}

} // namespace

bool InstallNametags(const Client &client) {
	g_client = &client;

	for (Sight &s : g_sight)
		s = Sight{};
	g_probeCursor = 0;
	g_lastDrawMs  = 0;

	if (!g_hud.Install("CHud::Draw", reinterpret_cast<void *>(CHud__Draw),
	                   reinterpret_cast<void *>(&HookedHudDraw))) {
		Log("nametag: FAILED to hook CHud::Draw at 0x%08X; remote players will "
		    "not be labelled",
		    CHud__Draw);
		for (const auto &f : HookFailures())
			Log("nametag:   %s: %s", f.name.c_str(), f.reason.c_str());
		g_client = nullptr;
		return false;
	}

	Log("nametag: hooked CHud::Draw at 0x%08X", CHud__Draw);
	return true;
}

void RemoveNametags() {
	if (g_hud.IsInstalled())
		g_hud.Remove();
	g_client = nullptr;
}

bool NametagsInstalled() {
	return g_hud.IsInstalled();
}

} // namespace coopiii::game
