// Colour tokens, straight out of design/DESIGN.md §1.
//
// One struct with every token, filled twice: Dark() and Light(). Nothing in
// the widgets writes a colour of its own - if a screen needs a colour that
// isn't here, the token table is what's wrong, not the call site.
#pragma once

#include <imgui.h>

#include <cstdint>

namespace ui {

struct Theme {
	// ---- surfaces ---------------------------------------------------------
	ImU32 bgPage;       // behind the window
	ImU32 bgTitlebar;   // custom title bar
	ImU32 bgWindow;     // window background, text inputs, gradient fades
	ImU32 bgPanel;      // right-hand panel, dialogs
	ImU32 bgCard;       // cards, lists, address box
	ImU32 bgConsole;    // server console, install details
	ImU32 bgSubtle;     // small filled buttons; city map blocks
	ImU32 mapBlock2;    // city map accent blocks
	ImU32 mapLot;       // city map empty lots (stroke only)
	ImU32 bgBadge;      // slot badges, feature icon tiles
	ImU32 bgDisabled;   // disabled primary button
	ImU32 bgSelected;   // selected segment in a segmented control

	// ---- text -------------------------------------------------------------
	ImU32 textPrimary;
	ImU32 textButton;     // secondary button label
	ImU32 textSecondary;  // body copy, labels
	ImU32 textTertiary;   // captions, hints, meta
	ImU32 textMuted;      // console timestamps, disabled text

	// ---- action -----------------------------------------------------------
	ImU32 actionBg;   // primary button, progress fill, switch on, player dot
	ImU32 actionFg;   // text and icons on actionBg

	// ---- lines ------------------------------------------------------------
	ImU32 border;          // card borders, dividers
	ImU32 borderControl;   // inputs, outline buttons
	ImU32 borderTitlebar;  // title bar bottom border
	ImU32 borderStrong;    // chips, dashed empty slots, highlighted card
	ImU32 borderDisabled;  // disabled button, kick button

	// ---- status -----------------------------------------------------------
	ImU32 mapRoute;     // dotted route between the two player dots
	ImU32 statusOk;
	ImU32 statusWarn;
	ImU32 statusFail;
	ImU32 brandRed;     // logo red: second player dot, eyebrow squares, stop
	ImU32 okPillBg;
	ImU32 okPillBorder;

	// ---- overlays ---------------------------------------------------------
	ImU32 scrim;         // behind a dialog
	bool  isLight;

	static const Theme &Dark();
	static const Theme &Light();
	static const Theme &For(bool light) { return light ? Light() : Dark(); }

	// Every token of `a` mixed toward `b`. What the theme switch in the title
	// bar cross-fades through, so the whole window changes over a quarter of a
	// second instead of blinking. `isLight` flips at the halfway point, which
	// is where the hover states should start going the other way.
	static Theme Blend(const Theme &a, const Theme &b, float t);
};

// Whether Windows is set to a light app theme:
// HKCU\...\Themes\Personalize\AppsUseLightTheme, 0 = dark. Anything we cannot
// read means dark, which is this design's home ground.
bool WindowsPrefersLight();

// Whether the OS wants animation at all
// (SystemParametersInfo SPI_GETCLIENTAREAANIMATION). False means every moving
// thing in the UI holds still - design/DESIGN.md §5.
bool SystemWantsAnimation();

// Helpers. ImGui colours are ABGR in an ImU32, which is easy to get backwards;
// these take the design's own #RRGGBB.
constexpr ImU32 Rgb(uint32_t hex, float alpha = 1.0f) {
	return IM_COL32((hex >> 16) & 0xFF, (hex >> 8) & 0xFF, hex & 0xFF,
	                static_cast<int>(alpha * 255.0f + 0.5f));
}

// Same colour, different alpha. For fades and halos, which the design draws as
// one token at a lower opacity rather than as a colour of its own.
ImU32 WithAlpha(ImU32 colour, float alpha);

// Straight mix, used only by the gradient fades over the city map.
ImU32 Mix(ImU32 a, ImU32 b, float t);

} // namespace ui
