// The city map background - design/DESIGN.md §5, the signature element.
//
// An abstract Liberty City: a grid of blocks rotated -16 degrees, with a
// dotted route between two player dots. It appears behind the launcher's left
// column, the installer's left column and the server's header.
//
// The tile is the design's own (ui/src/citytile.inc, lifted out of
// design/screens/Main.dc.html) rather than a port of
// design/reference/citymap_texture.py. The generator draws its blocks from
// Python's Mersenne Twister, so a C++ port of it would lay out a different
// city - faithful to the algorithm and not to the design.
#pragma once

#include <imgui.h>

namespace ui {

struct Theme;

// Where the two player dots sit, in the tile's own space, and which way the
// route runs between them. The design puts one vertical leg and one horizontal
// leg along street centre lines; both screens use the same shape at a
// different x.
struct MapRoute {
	float x0 = 297.5f;   // the vertical leg, and the first dot
	float y0 = 194.5f;
	float y1 = 247.5f;   // the corner
	float x1 = 394.5f;   // the horizontal leg, and the second dot
};

// A fade over the texture, so it never sits behind text at full strength.
// The design draws these as a linear gradient in bg.window (or bg.card) with
// two or three stops; `from`/`to` are fractions of the box.
struct MapFade {
	bool  vertical = true;
	float from     = 0.35f;   // fully transparent at or before this
	float to       = 0.92f;   // fully opaque at or after this
	ImU32 colour   = 0;       // 0 means "the theme's bg.window"
};

class CityMap {
public:
	// `seconds` is wall time; the map drifts one tile every `driftSeconds`
	// and the route's dashes march 14 px every `marchSeconds`. The app scale
	// is 320 s and 3.2 s (DESIGN §5); the landing uses faster numbers.
	static constexpr float kAppDriftSeconds  = 320.0f;
	static constexpr float kAppMarchSeconds  = 3.2f;
	static constexpr float kAppPulseSeconds  = 5.5f;
	static constexpr float kAppPulseScale    = 1.22f;

	// Draws the blocks, then the route, clipped to the given box.
	// Pass `animate = false` where the OS has asked for no animation.
	static void Draw(ImDrawList *draw, ImVec2 topLeft, ImVec2 size, const Theme &theme,
	                 const MapRoute &route, float seconds, bool animate);

	// A gradient the width or height of the box, in `fade.colour`. Drawn after
	// Draw() and before the text that sits on top.
	static void DrawFade(ImDrawList *draw, ImVec2 topLeft, ImVec2 size,
	                     const Theme &theme, const MapFade &fade);
};

} // namespace ui
