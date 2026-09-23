// Drawing remote players on the minimap. radar.h has the design and the
// reasoning; this is the half that touches game memory.
//
// ---------------------------------------------------------------------------
// The hook, and why it is on DrawBlips
// ---------------------------------------------------------------------------
//
// CRadar::DrawBlips is detoured and the original is called first, so the
// arrows land on a finished radar: the map, the game's own blips, the compass
// and the local player's own arrow are all already there.
//
// CHud::Draw would also have worked in principle and it is the wrong place in
// practice, for three reasons and only the third is about plumbing.
//
//   - DrawBlips sets six render states at the top of itself - z-write and
//     z-test off, vertex alpha on, src-alpha/inv-src-alpha blending, fog off
//     - and never puts them back. Drawing from inside it means drawing in
//     exactly the state the engine set up for drawing blips. Drawing after
//     CHud::Draw returns means drawing in whatever state the last thing the
//     HUD did happened to leave behind.
//   - DrawBlips is the thing that knows whether the radar is on screen. It
//     is called from inside a conditional in CHud::Draw and it has its own
//     two gates on top of that. A radar overlay that hangs off the HUD has to
//     re-derive all of that; one that hangs off the radar inherits it.
//   - and CHud::Draw is already detoured by nametag.cpp. MinHook allows one
//     hook per target, and that detour draws its tags *before* the original
//     so the local player's HUD stays on top of them - so there is not even a
//     post-original moment to borrow.
//
// The old version of this file had no hook at all, because keeping a table
// right needs no hook. Drawing does.
//
// ---------------------------------------------------------------------------
// Nothing here changes a ped
// ---------------------------------------------------------------------------
//
// Same rule as nametag.cpp. This resolves each player's ped read-only and
// does not call ped.cpp's ResolveRemote, because that clears the handle and
// re-arms the spawn when a ped has gone - correct for the network path, and
// not something a drawing pass gets to decide. If the ped is not there this
// frame the arrow comes off the wire instead, which is the one case the old
// blip table could not cover at all.
//
// ---------------------------------------------------------------------------
// Failing loudly
// ---------------------------------------------------------------------------
//
// The install refuses rather than guesses, in the same spirit as the blip
// table check it replaces. Before the detour goes in it checks that the seven
// bytes at CRadar::DrawBlips are the prologue the disassembly says they are,
// and that CRadar::RadarSprites[4] really is CRadar::CentreSprite - which is
// one pointer compare that catches the image having moved, another mod having
// rebuilt the sprite table, and this file's own addresses being wrong, all at
// once. Either failing means CoopIII says so in the log and draws nothing,
// rather than hooking a function that is not DrawBlips.
#include "radar.h"

#include "client.h"
#include "hook/hook.h"
#include "log.h"

#include <cstdint>
#include <cstring>

namespace coopiii::game {

namespace {

// CVector2D, and CRGBA. Both are what the engine's own signatures take.
struct Vec2f {
	float x, y;
};
struct Rgba {
	uint8_t r, g, b, a;
};

using DrawBlipsFn   = void(__cdecl *)();
using GetPedFn      = void *(__cdecl *)(int32_t);
using TransformFn   = void(__cdecl *)(Vec2f *, const Vec2f *);
using LimitPointFn  = float(__cdecl *)(Vec2f *);
using BlipAlphaFn   = uint8_t(__cdecl *)(float);
using TraceColourFn = uint32_t(__cdecl *)(uint32_t, uint32_t);
using SpriteQuadFn  = void(__thiscall *)(void *, float, float, float, float, float,
                                         float, float, float, const Rgba *);

Detour        g_drawBlips;
const Client *g_client = nullptr;

// The two reciprocals SCREEN_SCALE_X/Y are built from, read out of the image
// once at install. Cached rather than read per arrow because they are
// constants of the build; re-read per *frame* would be defensible, per
// player is just sixteen loads nobody asked for.
float g_recipRefWidth  = 0.0f;
float g_recipRefHeight = 0.0f;

// Said once each, never per frame.
bool g_saidFirstArrow  = false;
bool g_saidNoTexture   = false;
bool g_saidBadRange    = false;

// The retail prologue of CRadar::DrawBlips (0x004A42F0):
//
//     53              push ebx
//     56              push esi
//     57              push edi
//     55              push ebp
//     83 C4 80        add  esp,-80h
//
// Seven bytes, which is more than the five MinHook needs to place a jump, so
// this is exactly the span the detour is about to overwrite.
constexpr uint8_t DRAW_BLIPS_PROLOGUE[] = {0x53, 0x56, 0x57, 0x55, 0x83, 0xC4, 0x80};

// ---- thin wrappers, named after the engine functions they are -------------

void ToRadarSpace(Vec2f &out, const Vec2f &in) {
	Func<TransformFn>(CRadar__TransformRealWorldPointToRadarSpace)(&out, &in);
}

float LimitRadarPoint(Vec2f &point) {
	return Func<LimitPointFn>(CRadar__LimitRadarPoint)(&point);
}

uint8_t CalculateBlipAlpha(float dist) {
	return Func<BlipAlphaFn>(CRadar__CalculateBlipAlpha)(dist);
}

void ToScreenSpace(Vec2f &out, const Vec2f &in) {
	Func<TransformFn>(CRadar__TransformRadarPointToScreenSpace)(&out, &in);
}

// The second argument is m_bDim, and the inversion is easy to get backwards:
// a *set* m_bDim selects the lighter of the two colours. SetEntityBlip leaves
// it at 1, so every script blip in the game is the lighter pair, and a player
// has to be too or they would be the only dark green thing on the radar.
uint32_t GetRadarTraceColour(uint32_t colour) {
	return Func<TraceColourFn>(CRadar__GetRadarTraceColour)(colour, 1);
}

// ---- where somebody is, and which way they are facing ---------------------

// A remote player's live CPed, read-only. The vtable check is nametag.cpp's,
// and it is what turns a recycled pool slot into a null rather than into an
// arrow following a pedestrian around.
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

// Exactly the test DrawBlips makes before deciding to draw a BLIP_CHAR at the
// car's position instead of the ped's: `cmp byte [eax+314h],0 / je` then
// `mov edi,[eax+310h] / test edi,edi`.
void *VehicleOf(void *ped) {
	if (!Field<bool>(ped, offs::PED_IN_VEHICLE))
		return nullptr;
	return Field<void *>(ped, offs::PED_MY_VEHICLE);
}

// CPlaceable::GetForward().Heading(), which is FindPlayerHeading's own
// arithmetic: `fld [edx+14h] / fchs / fld [edx+18h] / fpatan`, i.e.
// Atan2(-forward.x, forward.y) over the entity's matrix.
//
// It is the same quantity the wire carries. CPed::m_fRotationCur is the angle
// CPlaceable::SetHeading builds the matrix from, and SetRotateZOnly writes
// forward = (-sin a, cos a), so Atan2(-fx, fy) is a again. That is what lets
// the two sources below be used interchangeably without a conversion.
float EntityHeading(void *entity) {
	const float fx = Field<float>(entity, offs::MATRIX_FWD + 0);
	const float fy = Field<float>(entity, offs::MATRIX_FWD + 4);
	return std::atan2(-fx, fy);
}

// Everything one arrow needs, resolved before anything is drawn.
struct Arrow {
	Vec2f world{};
	float heading   = 0.0f;
	bool  inVehicle = false;
	bool  fromPed   = false;   // for the log line, and nothing else
};

// Whichever of the two sources knows where this player is.
//
// The engine first, when it has an answer. A live ped - or the car it is
// sitting in, which is what DrawBlips itself redirects to - is the truth this
// machine is already rendering, and reading it means the arrow can never
// disagree with the ped standing under the nametag.
//
// The wire otherwise. A player whose ped has not streamed in, or who is
// outside whatever streaming radius CoopIII eventually grows, still has a
// position and a heading arriving twenty-five times a second, and an arrow
// needs nothing else. docs/roadmap.md §5.3.
bool ResolveArrow(const RemotePlayer &player, Arrow &out) {
	if (void *ped = LivePed(player)) {
		out.fromPed        = true;
		void *const entity = VehicleOf(ped);
		if (entity != nullptr) {
			out.inVehicle = true;
			out.world.x   = Field<float>(entity, offs::POSITION + 0);
			out.world.y   = Field<float>(entity, offs::POSITION + 4);
			out.heading   = EntityHeading(entity);
			return true;
		}
		out.world.x = Field<float>(ped, offs::POSITION + 0);
		out.world.y = Field<float>(ped, offs::POSITION + 4);
		out.heading = EntityHeading(ped);
		return true;
	}

	// No ped. `last` rather than the interpolation buffer, on purpose:
	// InterpBuffer::SampleDelayed drives a playback clock and ped.cpp already
	// advances it once a frame, so calling it from the draw would move that
	// clock twice per frame in a way ped.cpp cannot see. A radar pixel is
	// worth a hundred metres of world at this range; the newest snapshot is
	// more than close enough and it costs nothing.
	if (player.haveState) {
		out.world.x = player.last.pos.x;
		out.world.y = player.last.pos.y;
		out.heading = player.last.heading;
		return true;
	}
	if (player.haveSeedPose) {
		out.world.x = player.seedPose.pos.x;
		out.world.y = player.seedPose.pos.y;
		out.heading = player.seedPose.heading;
		return true;
	}
	return false;
}

// ---- the draw -------------------------------------------------------------

// The two gates DrawBlips itself opens with. The original has already run and
// has already returned early if either of these is set, in which case it set
// no render states either - so this is not belt and braces, it is the
// difference between drawing into a state the engine prepared and drawing
// into whatever was there.
bool RadarIsOnScreen() {
	if (Global<uint8_t>(TheCamera + CAMERA_WIDESCREEN_ON) != 0)
		return false;   // widescreen bars: from here, a cutscene
	return Global<uint8_t>(CHud__m_Wants_To_Draw_Hud) != 0;
}

void DrawArrows() {
	if (g_client == nullptr || !g_client->IsConnected())
		return;
	if (!RadarIsOnScreen())
		return;

	// CRadar::LoadTextures runs at game start and CRadar::RemoveRadarSections
	// throws them away again, so between a game load and the next load this
	// is a CSprite2d with no texture. Drawing it would queue untextured white
	// quads over the radar.
	void *const centre = Ptr<void>(CRadar__CentreSprite);
	if (Field<void *>(centre, 0) == nullptr) {
		if (!g_saidNoTexture) {
			g_saidNoTexture = true;
			Log("radar: CRadar::CentreSprite has no texture loaded, so there is "
			    "nothing to draw a player arrow with. This is normal between a "
			    "game load and the next; reported once");
		}
		return;
	}

	const int screenW = Global<int32_t>(RsGlobal__maximumWidth);
	const int screenH = Global<int32_t>(RsGlobal__maximumHeight);
	const float halfX = RadarSpriteHalf(screenW, g_recipRefWidth);
	const float halfY = RadarSpriteHalf(screenH, g_recipRefHeight);
	// Zero means the game's own arrow has vanished into the truncation too.
	// See RadarSpriteHalf.
	if (!(halfX > 0.0f) || !(halfY > 0.0f))
		return;

	const float range = Global<float>(CRadar__m_radarRange);
	if (!RadarRangeUsable(range)) {
		if (!g_saidBadRange) {
			g_saidBadRange = true;
			Log("radar: CRadar::m_radarRange reads %f, which is not a radar "
			    "range, so remote players cannot be placed on the minimap this "
			    "frame; reported once",
			    range);
		}
		return;
	}

	// Which way the camera has the radar turned, asked of the engine's own
	// transform rather than of TheCamera. radar.h::RadarCameraHeading has the
	// derivation; the short version is that the point one radar range due
	// north of the radar origin lands on exactly (sin, cos) of the angle.
	const Vec2f &origin = Global<Vec2f>(CRadar__vec2DRadarOrigin);
	const Vec2f  north{origin.x, origin.y + range};
	Vec2f        sincos{};
	ToRadarSpace(sincos, north);
	const float cameraHeading = RadarCameraHeading(sincos.x, sincos.y);

	const uint8_t localId = g_client->LocalPlayerId();
	for (uint8_t id = 0; id < MAX_PLAYERS; ++id) {
		if (id == localId)
			continue;   // you are the centre sprite, drawn by the engine

		const RemotePlayer &player = g_client->PlayerSlot(id);
		if (!ArrowWanted(player.active, player.haveState || player.haveSeedPose))
			continue;

		Arrow arrow;
		if (!ResolveArrow(player, arrow))
			continue;

		// The engine's own four steps, in the engine's own order.
		Vec2f       radarPoint{};
		ToRadarSpace(radarPoint, arrow.world);
		const float dist  = LimitRadarPoint(radarPoint);
		const uint8_t alpha = CalculateBlipAlpha(dist);
		Vec2f       screen{};
		ToScreenSpace(screen, radarPoint);

		const ArrowQuad quad = MakeArrowQuad(screen.x, screen.y,
		                                     ArrowAngle(arrow.heading, cameraHeading),
		                                     halfX, halfY);

		const ArrowRgb rgb =
		    UnpackTraceColour(GetRadarTraceColour(ArrowTraceColour(arrow.inVehicle)));
		const Rgba colour{rgb.r, rgb.g, rgb.b, alpha};

		const int *o = ARROW_DRAW_ORDER;
		Func<SpriteQuadFn>(CSprite2d__DrawFourCorners)(
		    centre, quad.x[o[0]], quad.y[o[0]], quad.x[o[1]], quad.y[o[1]],
		    quad.x[o[2]], quad.y[o[2]], quad.x[o[3]], quad.y[o[3]], &colour);

		if (!g_saidFirstArrow) {
			g_saidFirstArrow = true;
			Log("radar: player %u (\"%s\") is an arrow on the minimap - %s, %s, "
			    "%.0fx%.0f px, alpha %u",
			    id, player.nick.c_str(),
			    arrow.inVehicle ? "red (in a vehicle)" : "green (on foot)",
			    arrow.fromPed ? "from their ped" : "from the wire",
			    halfX * 2.0f, halfY * 2.0f, unsigned(alpha));
		}
	}
}

void __cdecl HookedDrawBlips() {
	// The original first: the map, the game's blips, the compass and the local
	// player's own arrow all belong under ours.
	g_drawBlips.Original<DrawBlipsFn>()();

	// Same containment as the frame hook: a throw escaping into the engine
	// would unwind through frames that know nothing about C++ exceptions.
	try {
		DrawArrows();
	} catch (...) {
		static bool reported = false;
		if (!reported) {
			reported = true;
			Log("radar: the arrow draw threw; remote players may be missing from "
			    "the minimap from here on");
		}
	}
}

} // namespace

// ---- installing -----------------------------------------------------------

bool InstallRadarArrows(const Client &client) {
	g_client         = nullptr;
	g_saidFirstArrow = false;
	g_saidNoTexture  = false;
	g_saidBadRange   = false;

	// Is the function about to be detoured the one this file was written
	// against? verify.cpp has already refused to load against any other image,
	// so a mismatch here means something patched DrawBlips at runtime - and
	// hooking a function whose first seven bytes are somebody else's jump is
	// how two mods quietly break each other.
	if (std::memcmp(reinterpret_cast<const void *>(CRadar__DrawBlips),
	                DRAW_BLIPS_PROLOGUE, sizeof(DRAW_BLIPS_PROLOGUE)) != 0) {
		const uint8_t *at = reinterpret_cast<const uint8_t *>(CRadar__DrawBlips);
		Log("radar: NOT drawing players on the minimap. CRadar::DrawBlips at "
		    "0x%08X starts %02X %02X %02X %02X %02X %02X %02X, not the prologue "
		    "this was built against - something else has already patched it, and "
		    "hooking over that would break both",
		    unsigned(CRadar__DrawBlips), at[0], at[1], at[2], at[3], at[4], at[5],
		    at[6]);
		return false;
	}

	// One pointer compare that checks three things at once: the sprite table
	// is where addresses.h says, CentreSprite is where addresses.h says, and
	// entry 4 is still the one the table shipped with.
	void *const inTable =
	    Global<void *>(CRadar__RadarSprites + RADAR_SPRITE_CENTRE * sizeof(void *));
	if (inTable != reinterpret_cast<void *>(CRadar__CentreSprite)) {
		Log("radar: NOT drawing players on the minimap. "
		    "CRadar::RadarSprites[%u] is 0x%08X, not CRadar::CentreSprite "
		    "(0x%08X) - the sprite table has been moved or rebuilt, and the "
		    "arrow would be drawn with whatever is there instead",
		    unsigned(RADAR_SPRITE_CENTRE), unsigned(uintptr_t(inTable)),
		    unsigned(CRadar__CentreSprite));
		return false;
	}

	// The HUD reciprocals, read rather than compiled in, so a mod that
	// rescales the radar by patching them takes our arrow with it. radar.h
	// has the reasoning and why this file answers the Widescreen Fix question
	// the opposite way to nametag.h.
	g_recipRefWidth  = Global<float>(RADAR_RECIP_REF_WIDTH);
	g_recipRefHeight = Global<float>(RADAR_RECIP_REF_HEIGHT);
	if (!(g_recipRefWidth > 0.0f) || !(g_recipRefHeight > 0.0f)) {
		Log("radar: NOT drawing players on the minimap. The HUD scale "
		    "reciprocals at 0x%08X and 0x%08X read %f and %f, which cannot be "
		    "1/640 and 1/448 or anything a mod would have replaced them with",
		    unsigned(RADAR_RECIP_REF_WIDTH), unsigned(RADAR_RECIP_REF_HEIGHT),
		    g_recipRefWidth, g_recipRefHeight);
		return false;
	}

	if (!g_drawBlips.Install("CRadar::DrawBlips",
	                         reinterpret_cast<void *>(CRadar__DrawBlips),
	                         reinterpret_cast<void *>(&HookedDrawBlips))) {
		Log("radar: FAILED to hook CRadar::DrawBlips at 0x%08X; remote players "
		    "will not be on the minimap",
		    unsigned(CRadar__DrawBlips));
		for (const auto &f : HookFailures())
			Log("radar:   %s: %s", f.name.c_str(), f.reason.c_str());
		return false;
	}

	g_client = &client;

	const float stockW = 1.0f / HUD_REF_WIDTH;
	const float stockH = 1.0f / HUD_REF_HEIGHT;
	Log("radar: remote players will be arrows on the minimap. Hooked "
	    "CRadar::DrawBlips at 0x%08X, drawing CRadar::CentreSprite (0x%08X) "
	    "through CSprite2d::Draw",
	    unsigned(CRadar__DrawBlips), unsigned(CRadar__CentreSprite));
	if (g_recipRefWidth != stockW || g_recipRefHeight != stockH) {
		Log("radar: the HUD scale reciprocals are %g and %g, not the stock %g "
		    "and %g - something has rescaled the radar, and the player arrows "
		    "are following it rather than the built-in numbers",
		    g_recipRefWidth, g_recipRefHeight, stockW, stockH);
	}
	return true;
}

void RemoveRadarArrows() {
	if (g_drawBlips.IsInstalled())
		g_drawBlips.Remove();
	g_client = nullptr;
}

bool RadarArrowsInstalled() {
	return g_drawBlips.IsInstalled();
}

} // namespace coopiii::game
