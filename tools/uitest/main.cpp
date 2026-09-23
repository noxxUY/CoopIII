// Tests for the ui library that need neither a window nor a GPU.
//
// Three things worth failing a build over:
//
//   1. the colour tokens still meet WCAG 4.5:1, which design/DESIGN.md §1
//      says to keep that way
//   2. the city map's drift loops seamlessly, which is the whole reason the
//      tile is 900 x 900
//   3. every icon's path data parses and stays inside its 24 grid
#include "ui/anim.h"
#include "ui/citymap.h"
#include "ui/fonts.h"
#include "ui/icons.h"
#include "ui/theme.h"

#include <imgui.h>
#include <imgui_internal.h>

#include <cmath>
#include <cstring>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

using namespace ui;

namespace {

int g_failures = 0;

void Check(bool ok, const std::string &what) {
	std::printf("  [%s] %s\n", ok ? " ok " : "FAIL", what.c_str());
	if (!ok)
		++g_failures;
}

// ---- contrast -------------------------------------------------------------

float Channel(ImU32 colour, int shift) {
	const float v = ((colour >> shift) & 0xFF) / 255.0f;
	return v <= 0.04045f ? v / 12.92f : std::pow((v + 0.055f) / 1.055f, 2.4f);
}

float Luminance(ImU32 colour) {
	return 0.2126f * Channel(colour, IM_COL32_R_SHIFT) +
	       0.7152f * Channel(colour, IM_COL32_G_SHIFT) +
	       0.0722f * Channel(colour, IM_COL32_B_SHIFT);
}

float Contrast(ImU32 a, ImU32 b) {
	const float la = Luminance(a), lb = Luminance(b);
	const float hi = la > lb ? la : lb;
	const float lo = la > lb ? lb : la;
	return (hi + 0.05f) / (lo + 0.05f);
}

struct Pair {
	const char *name;
	ImU32 Theme::*text;
	ImU32 Theme::*background;
	float          least;
};

void CheckContrast(const Theme &theme, const char *label) {
	// The pairings the screens actually use. 4.5:1 for text;
	// icons and dots that carry meaning are held to 3:1, the WCAG bar for
	// non-text.
	const Pair pairs[] = {
	    {"text.primary on bg.window", &Theme::textPrimary, &Theme::bgWindow, 4.5f},
	    {"text.primary on bg.panel", &Theme::textPrimary, &Theme::bgPanel, 4.5f},
	    {"text.primary on bg.card", &Theme::textPrimary, &Theme::bgCard, 4.5f},
	    {"text.secondary on bg.window", &Theme::textSecondary, &Theme::bgWindow, 4.5f},
	    {"text.secondary on bg.panel", &Theme::textSecondary, &Theme::bgPanel, 4.5f},
	    {"text.secondary on bg.card", &Theme::textSecondary, &Theme::bgCard, 4.5f},
	    {"text.tertiary on bg.window", &Theme::textTertiary, &Theme::bgWindow, 4.5f},
	    {"text.tertiary on bg.card", &Theme::textTertiary, &Theme::bgCard, 4.5f},
	    {"text.button on bg.window", &Theme::textButton, &Theme::bgWindow, 4.5f},
	    {"text.muted on bg.console", &Theme::textMuted, &Theme::bgConsole, 4.5f},
	    {"action.fg on action.bg", &Theme::actionFg, &Theme::actionBg, 4.5f},
	    {"status.ok on bg.card", &Theme::statusOk, &Theme::bgCard, 4.5f},
	    {"status.warn on bg.card", &Theme::statusWarn, &Theme::bgCard, 4.5f},
	    {"status.fail on bg.card", &Theme::statusFail, &Theme::bgCard, 4.5f},
	    {"status.ok on ok.pill.bg", &Theme::statusOk, &Theme::okPillBg, 4.5f},
	    {"text.primary on bg.console", &Theme::textPrimary, &Theme::bgConsole, 4.5f},
	    {"text.secondary on bg.console", &Theme::textSecondary, &Theme::bgConsole, 4.5f},
	    {"border.control against bg.window", &Theme::borderControl, &Theme::bgWindow, 1.3f},
	};

	std::printf("%s theme\n", label);
	for (const Pair &p : pairs) {
		const float ratio = Contrast(theme.*p.text, theme.*p.background);
		char        line[160];
		std::snprintf(line, sizeof(line), "%-36s %.2f:1 (needs %.1f)", p.name, ratio, p.least);
		Check(ratio >= p.least, line);
	}
}

// ---- the city map ---------------------------------------------------------

// A frame with no backend, so a draw list is fully set up. Returns the
// vertices the callback produced.
std::vector<ImVec2> Capture(const std::function<void(ImDrawList *)> &body) {
	ImGui::NewFrame();
	ImDrawList *draw  = ImGui::GetBackgroundDrawList();
	const int   first = draw->VtxBuffer.Size;
	body(draw);

	std::vector<ImVec2> out;
	out.reserve(static_cast<size_t>(draw->VtxBuffer.Size - first));
	for (int i = first; i < draw->VtxBuffer.Size; ++i)
		out.push_back(draw->VtxBuffer.Data[i].pos);

	ImGui::EndFrame();
	return out;
}

std::vector<ImVec2> MapVertices(float seconds, bool animate = true) {
	return Capture([&](ImDrawList *draw) {
		CityMap::Draw(draw, ImVec2(0, 0), ImVec2(440, 400), Theme::Dark(), MapRoute{}, seconds,
		              animate);
	});
}

void CheckSeamlessLoop() {
	std::printf("city map\n");

	// The drift loops every 320 s, the dashes every 3.2 s and the halo every
	// 5.5 s, so the whole map only repeats exactly at their common multiple.
	constexpr float kWholeCycle = 3520.0f;   // 11 x 320, 1100 x 3.2, 640 x 5.5

	const std::vector<ImVec2> atStart      = MapVertices(0.0f);
	const std::vector<ImVec2> afterOneLoop = MapVertices(kWholeCycle);

	Check(!atStart.empty(), "the map draws something at all");
	Check(atStart.size() == afterOneLoop.size(),
	      "a whole cycle produces the same geometry (vertex count)");

	if (atStart.size() == afterOneLoop.size()) {
		float worst = 0.0f;
		for (size_t i = 0; i < atStart.size(); ++i) {
			worst = ImMax(worst, std::fabs(atStart[i].x - afterOneLoop[i].x));
			worst = ImMax(worst, std::fabs(atStart[i].y - afterOneLoop[i].y));
		}
		char line[128];
		std::snprintf(line, sizeof(line),
		              "a whole cycle lands back where it started (worst %.4f px)", worst);
		// The loop is what makes the movement endless; a tile that does not
		// land back on itself would show a seam every 320 seconds.
		Check(worst < 0.05f, line);
	}

	// A second in, it has moved: 900 px over 320 s is 2.8 px per second. One
	// second keeps the same tiles on screen, so the vertices still line up.
	const std::vector<ImVec2> afterASecond = MapVertices(1.0f);
	float                     moved        = 0.0f;
	bool                      comparable   = afterASecond.size() == atStart.size();
	// Only the blocks, which are laid down first. The halo pulses on its own
	// 5.5 s cycle and would swamp a 2.7 px drift.
	const size_t blocks = ImMin<size_t>(atStart.size(), 2000);
	if (comparable)
		for (size_t i = 0; i < blocks; ++i)
			moved = ImMax(moved, std::fabs(atStart[i].y - afterASecond[i].y));
	char line[128];
	std::snprintf(line, sizeof(line), "a second in it has drifted (%.3f px, expected 2.704)",
	              moved);
	Check(comparable && moved > 2.60f && moved < 2.80f, line);

	// Motion off means nothing moves at all.
	const std::vector<ImVec2> still = MapVertices(1234.0f, false);
	bool                      same  = still.size() == atStart.size();
	if (same)
		for (size_t i = 0; i < still.size(); ++i)
			if (std::fabs(still[i].x - atStart[i].x) > 0.01f ||
			    std::fabs(still[i].y - atStart[i].y) > 0.01f) {
				same = false;
				break;
			}
	Check(same, "with animation off the map is exactly where it starts");
}

// ---- icons ----------------------------------------------------------------

void CheckIcons() {
	std::printf("icons\n");

	const Icon all[] = {
	    Icon::CircleCheck, Icon::CircleX,    Icon::Info,     Icon::TriangleAlert,
	    Icon::Folder,      Icon::Play,       Icon::Download, Icon::RotateCcw,
	    Icon::Copy,        Icon::Check,      Icon::UserX,    Icon::Sun,
	    Icon::Cloud,       Icon::CloudRain,  Icon::CloudFog, Icon::Image,
	    Icon::ArrowUpRight,Icon::Cpu,        Icon::Users,    Icon::Layers,
	    Icon::ShieldCheck, Icon::Gear,       Icon::Stop,     Icon::Minimize,
	    Icon::Maximize,    Icon::Close,      Icon::ChevronDown, Icon::Plus,
	    Icon::Minus,
	};
	const char *names[] = {
	    "circle-check", "circle-x",  "info",       "triangle-alert", "folder",
	    "play",         "download",  "rotate-ccw", "copy",           "check",
	    "user-x",       "sun",       "cloud",      "cloud-rain",     "cloud-fog",
	    "image",        "arrow-up-right", "cpu",   "users",          "layers",
	    "shield-check", "gear",      "stop",       "minimize",       "maximize",
	    "close",        "chevron-down",   "plus",  "minus",
	};

	const float size = 24.0f;
	for (size_t i = 0; i < sizeof(all) / sizeof(all[0]); ++i) {
		const std::vector<ImVec2> pts = Capture([&](ImDrawList *draw) {
			DrawIcon(draw, all[i], ImVec2(0, 0), size, IM_COL32_WHITE, 2.0f);
		});

		const bool drewSomething = !pts.empty();
		float      minX = 1e9f, minY = 1e9f, maxX = -1e9f, maxY = -1e9f;
		for (const ImVec2 &p : pts) {
			minX = ImMin(minX, p.x); maxX = ImMax(maxX, p.x);
			minY = ImMin(minY, p.y); maxY = ImMax(maxY, p.y);
		}
		// A 2 px stroke straddles the path, so a shape touching the grid's
		// edge legitimately reaches 1 px outside it.
		const bool inGrid = drewSomething && minX >= -1.6f && minY >= -1.6f &&
		                    maxX <= size + 1.6f && maxY <= size + 1.6f;
		char line[128];
		std::snprintf(line, sizeof(line), "%-16s drawn, inside the 24 grid (%.1f..%.1f)",
		              names[i], minX, maxX);
		Check(drewSomething && inGrid, line);
	}
}

// ---- tokens ---------------------------------------------------------------

void CheckTokens() {
	std::printf("tokens\n");
	const Theme &dark  = Theme::Dark();
	const Theme &light = Theme::Light();

	// Every token is opaque except the scrim, which is not.
	ImU32 Theme::*const opaque[] = {
	    &Theme::bgPage,     &Theme::bgTitlebar,  &Theme::bgWindow,    &Theme::bgPanel,
	    &Theme::bgCard,     &Theme::bgConsole,   &Theme::bgSubtle,    &Theme::mapBlock2,
	    &Theme::mapLot,     &Theme::bgBadge,     &Theme::bgDisabled,  &Theme::bgSelected,
	    &Theme::textPrimary,&Theme::textButton,  &Theme::textSecondary,&Theme::textTertiary,
	    &Theme::textMuted,  &Theme::actionBg,    &Theme::actionFg,    &Theme::border,
	    &Theme::borderControl, &Theme::borderTitlebar, &Theme::borderStrong,
	    &Theme::borderDisabled, &Theme::mapRoute, &Theme::statusOk,   &Theme::statusWarn,
	    &Theme::statusFail, &Theme::brandRed,    &Theme::okPillBg,    &Theme::okPillBorder,
	};

	bool allOpaque = true, allDiffer = true;
	for (ImU32 Theme::*const token : opaque) {
		if (((dark.*token) >> IM_COL32_A_SHIFT) != 0xFF ||
		    ((light.*token) >> IM_COL32_A_SHIFT) != 0xFF)
			allOpaque = false;
	}
	Check(allOpaque, "every surface and text token is opaque in both themes");

	// brand.red is the one token that is the same in both, on purpose.
	int same = 0;
	for (ImU32 Theme::*const token : opaque)
		if ((dark.*token) == (light.*token))
			++same;
	Check(same == 1 && dark.brandRed == light.brandRed,
	      "only brand.red is shared between the two themes");
	(void)allDiffer;

	Check(((dark.scrim >> IM_COL32_A_SHIFT) & 0xFF) == 189, "dark scrim is 74% opaque");
	Check(((light.scrim >> IM_COL32_A_SHIFT) & 0xFF) == 97, "light scrim is 38% opaque");
	Check(dark.isLight == false && light.isLight == true, "each theme knows which it is");
}

// ---- motion ---------------------------------------------------------------

void CheckMotion() {
	std::printf("motion\n");

	// The curves all start at 0 and end at 1, and only EaseBack is allowed
	// outside that range in between.
	Check(EaseOut(0.0f) == 0.0f && EaseOut(1.0f) == 1.0f, "EaseOut spans 0 to 1");
	Check(EaseInOut(0.0f) == 0.0f && EaseInOut(1.0f) == 1.0f, "EaseInOut spans 0 to 1");
	Check(std::fabs(EaseInOut(0.5f) - 0.5f) < 0.001f, "EaseInOut is symmetric about the middle");
	Check(EaseBack(0.0f) == 0.0f && EaseBack(1.0f) == 1.0f, "EaseBack spans 0 to 1");

	float overshoot = 0.0f;
	for (int i = 0; i <= 100; ++i)
		overshoot = ImMax(overshoot, EaseBack(i / 100.0f));
	Check(overshoot > 1.0f && overshoot < 1.12f, "EaseBack overshoots, but only by a tenth");

	// Anything clamped past its ends stays there, because Since()/duration
	// hands these numbers well over 1 every time something has finished.
	Check(EaseOut(4.0f) == 1.0f && EaseOut(-1.0f) == 0.0f, "EaseOut clamps");
	Check(EaseBack(4.0f) == 1.0f && EaseBack(-1.0f) == 0.0f, "EaseBack clamps");

	// Approach has to be frame-rate independent: the same wall time has to
	// cover the same ground whether it arrives as 30 frames or 120. That is
	// the whole reason it is exponential and not a fixed step.
	BeginMotionFrame(1.0f / 30.0f, true);
	float slow = 0.0f;
	for (int i = 0; i < 30; ++i)
		slow = Approach(slow, 1.0f, 8.0f);

	BeginMotionFrame(1.0f / 120.0f, true);
	float fast = 0.0f;
	for (int i = 0; i < 120; ++i)
		fast = Approach(fast, 1.0f, 8.0f);

	char line[128];
	std::snprintf(line, sizeof(line), "one second of Approach lands in the same place (%.4f vs %.4f)",
	              slow, fast);
	Check(std::fabs(slow - fast) < 0.01f, line);

	// And it has to settle exactly, or Appear() never reaches 0 and a closed
	// dialog is drawn for ever at an alpha nobody can see.
	BeginMotionFrame(1.0f / 60.0f, true);
	float settling = 1.0f;
	int   frames   = 0;
	while (settling != 0.0f && frames < 600) {
		settling = Approach(settling, 0.0f, 16.0f);
		++frames;
	}
	std::snprintf(line, sizeof(line), "Approach settles exactly, after %d frames", frames);
	Check(settling == 0.0f && frames < 120, line);

	// With the OS asking for no animation, motion that runs on its own is off
	// (DESIGN §5) and a transition keeps its cross-fade but loses every pixel
	// of travel.
	BeginMotionFrame(1.0f / 60.0f, false);
	Check(!Ambient(), "animation off turns the ambient motion off");
	Check(Travel() == 0.0f, "animation off leaves a transition nowhere to slide");
	const float eased = Approach(0.0f, 1.0f, 8.0f);
	Check(eased > 0.0f && eased < 1.0f,
	      "animation off still lets a click's own answer fade rather than snap");

	BeginMotionFrame(1.0f / 60.0f, true);
	Check(Ambient() && Travel() == 1.0f, "and it all comes back when the OS allows it");

	// A frame lost to a folder picker is a gap, not a long frame.
	BeginMotionFrame(3.0f, true);
	Check(Delta() <= 1.0f / 15.0f + 0.0001f, "a stalled frame is clamped, so nothing teleports");
	BeginMotionFrame(1.0f / 60.0f, true);

	// A name and a widget id are different keys even when they read the same.
	// They were not once, and the server's options dialog was then only on
	// screen while the pointer sat on the button that opened it: the button
	// wrote its hover state first, and the frame guard handed the dialog that
	// number instead of its own.
	const ImGuiID widget = ImHashStr("##options", 0, 0);
	Animate(widget, 0.0f, 30.0f);                            // the button: not hovered
	const float dialog = Appear("##options", true, 30.0f);   // the dialog: open
	Check(MotionKey("##options") != widget && dialog == 1.0f,
	      "a name and a widget id that spell the same do not share an entry");
}

void CheckThemeBlend() {
	std::printf("theme blend\n");
	const Theme &dark  = Theme::Dark();
	const Theme &light = Theme::Light();

	const Theme at0 = Theme::Blend(dark, light, 0.0f);
	const Theme at1 = Theme::Blend(dark, light, 1.0f);
	Check(at0.bgWindow == dark.bgWindow && at0.textPrimary == dark.textPrimary &&
	          at0.isLight == false,
	      "a blend of 0 is the dark theme");
	Check(at1.bgWindow == light.bgWindow && at1.textPrimary == light.textPrimary &&
	          at1.isLight == true,
	      "a blend of 1 is the light theme");

	// Halfway is genuinely between, on every token - the whole point being
	// that a token added to the struct is carried without anyone remembering
	// to update the blend.
	const Theme half = Theme::Blend(dark, light, 0.5f);
	bool        between = true;
	ImU32 Theme::*const sample[] = {&Theme::bgWindow,   &Theme::bgCard,     &Theme::bgPanel,
	                                &Theme::textPrimary, &Theme::actionBg,  &Theme::border,
	                                &Theme::statusOk,    &Theme::okPillBg,  &Theme::mapRoute};
	for (ImU32 Theme::*const token : sample) {
		const int a = (dark.*token) & 0xFF;
		const int b = (light.*token) & 0xFF;
		const int m = (half.*token) & 0xFF;
		if (m < ImMin(a, b) || m > ImMax(a, b))
			between = false;
	}
	Check(between, "halfway is between the two on every token sampled");
	Check(half.scrim != dark.scrim && half.scrim != light.scrim, "the scrim blends too");
}

} // namespace

int main() {
	// Unbuffered, so a crash still shows how far it got.
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	IMGUI_CHECKVERSION();
	ImGui::CreateContext();
	ImGuiIO &io = ImGui::GetIO();
	// 1.92 rasterises glyphs on demand and expects the renderer to say it can
	// take new textures. Nothing here renders, but the atlas still has to
	// believe it can grow or it refuses to bake a glyph.
	io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
	io.IniFilename = nullptr;   // a test has no layout worth remembering
	io.LogFilename = nullptr;
	io.DisplaySize = ImVec2(1040, 700);
	io.Fonts->AddFontDefault();
	io.Fonts->Build();

	// A frame with no backend attached is enough to get a working draw list,
	// which is what makes any of this testable without a GPU.
	io.DeltaTime = 1.0f / 60.0f;

	// The fonts have to measure the same as the ones the screens were drawn
	// with, or every wrapped paragraph breaks in a different place and the
	// layout under it moves.
	Fonts::Load();
	ImGui::NewFrame();
	std::printf("type widths\n");
	struct Want { Type role; const char *text; float browser; };
	const Want wants[] = {
	    {Type::Body, "CoopIII goes in with the Essential Pack by default. Untick anything you already manage yourself.", 586.77f},
	    {Type::Body, "Co-op only runs when GTA III starts from here.", 283.44f},
	    {Type::Body, "The quick brown fox jumps over the lazy dog", 270.75f},
	    {Type::WindowHeading, "Join a server", 221.05f},
	    {Type::SectionTitle, "Pre-launch checks", 127.73f},
	    {Type::FieldLabel, "Your name", 63.22f},
	    {Type::Console, "123456789", 69.53f},
	    {Type::Mono, "192.168.1.40:2001", 136.58f},
	    {Type::MonoLarge, "192.168.1.40:2001", 168.11f},
	};
	for (const Want &want : wants) {
	    const TypeStyle style = StyleOf(want.role);
	    ImGui::PushFont(style.font, style.pixelSize);
	    const float got = ImGui::CalcTextSize(want.text).x;
	    ImGui::PopFont();
	    // Dear ImGui rounds each glyph's advance to a whole pixel, so the error
	    // is bounded by half a pixel per glyph however long the run is. That is
	    // the bound to hold it to - not a percentage, which would be strict on
	    // a long string and meaningless on a short one.
	    const int   glyphs = static_cast<int>(std::strlen(want.text));
	    const float slack  = 0.5f * glyphs + 1.0f;
	    const float delta  = std::fabs(got - want.browser);
	    char        line[220];
	    std::snprintf(line, sizeof(line), "%-34.34s %.1f vs %.1f (%+.1f%%, %.1f of %.1f allowed)",
	                  want.text, got, want.browser,
	                  100.0f * (got - want.browser) / want.browser, delta, slack);
	    Check(delta <= slack, line);
	}
	ImGui::EndFrame();

	CheckTokens();
	CheckContrast(Theme::Dark(), "dark");
	CheckContrast(Theme::Light(), "light");
	CheckSeamlessLoop();
	CheckIcons();
	CheckMotion();
	CheckThemeBlend();

	ImGui::DestroyContext();

	std::printf("\n%s\n", g_failures == 0 ? "all ui checks passed"
	                                      : "ui checks FAILED");
	return g_failures == 0 ? 0 : 1;
}
