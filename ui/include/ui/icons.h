// The icon set - design/DESIGN.md §4.
//
// Stroke icons on a 24 grid, stroke width 2 (1.8 for the title bar), round
// caps and joins, drawn in the current text colour.
//
// These are not a font. The design draws them as SVG paths and the same paths
// are kept here verbatim, rendered onto an ImDrawList by a small path reader.
// A font would mean shipping 900 kB of Lucide for the twenty glyphs used here,
// and it would leave the two custom shapes - the server's gear and the stop
// square - without a home.
#pragma once

#include <imgui.h>

namespace ui {

enum class Icon {
	None,   // a button the design draws without one

	CircleCheck,
	CircleX,
	Info,
	TriangleAlert,
	Folder,
	Play,
	Download,
	RotateCcw,
	Copy,
	Check,
	UserX,
	Sun,
	Cloud,
	CloudRain,
	CloudFog,
	Image,
	ArrowUpRight,
	Cpu,
	Users,
	Layers,
	ShieldCheck,
	Gear,      // custom: the rules card
	Stop,      // custom: the stop square
	Minimize,
	Maximize,
	Close,
	ChevronDown,
	Plus,
	Minus,
};

// Draws `icon` into a `size` x `size` box with its top-left at `topLeft`.
// `stroke` is in 24-grid units, so it scales with the icon the way the design's
// SVG does. `rotation` is radians about the box's centre - the one thing here
// that moves, for an icon that means "working".
void DrawIcon(ImDrawList *draw, Icon icon, ImVec2 topLeft, float size, ImU32 colour,
              float stroke = 2.0f, float rotation = 0.0f);

// Same, in the current window at the cursor, advancing the layout by `size`.
// Uses the current text colour unless one is given.
void IconItem(Icon icon, float size, ImU32 colour, float stroke = 2.0f);

} // namespace ui
