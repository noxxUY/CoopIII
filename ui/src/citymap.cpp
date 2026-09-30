#include "ui/citymap.h"

#include "ui/theme.h"

#include <imgui_internal.h>

#include <cmath>

namespace ui {
namespace {

constexpr float kTile       = 900.0f;   // the tile is seamless at 900 x 900
constexpr float kAngleDeg   = -16.0f;
constexpr float kBlockRound = 2.0f;

struct TileRect {
	float x, y, w, h;
	int   kind;   // 0 block, 1 accent block, 2 empty lot
};

const TileRect kTile900[] = {
#include "citytile.inc"
};

constexpr int kTileCount = static_cast<int>(sizeof(kTile900) / sizeof(kTile900[0]));

struct Rot {
	float cos, sin;

	ImVec2 Apply(float x, float y) const {
		return ImVec2(x * cos - y * sin, x * sin + y * cos);
	}
	ImVec2 Undo(float x, float y) const {
		return ImVec2(x * cos + y * sin, -x * sin + y * cos);
	}
};

Rot Rotation(float degrees) {
	const float r = degrees * 3.14159265358979323846f / 180.0f;
	return {std::cos(r), std::sin(r)};
}

// Rotates every vertex added since `first` about `pivot`. The blocks are laid
// out unrotated and turned in one pass, which keeps the rounded corners the
// design asks for - ImDrawList has no rotated rounded rect.
void RotateVertices(ImDrawList *draw, int first, ImVec2 pivot, const Rot &rot) {
	ImDrawVert *v   = draw->VtxBuffer.Data + first;
	ImDrawVert *end = draw->VtxBuffer.Data + draw->VtxBuffer.Size;
	for (; v < end; ++v) {
		const float x = v->pos.x - pivot.x;
		const float y = v->pos.y - pivot.y;
		v->pos.x      = pivot.x + x * rot.cos - y * rot.sin;
		v->pos.y      = pivot.y + x * rot.sin + y * rot.cos;
	}
}

float Wrap(float value, float period) {
	const float m = std::fmod(value, period);
	return m < 0.0f ? m + period : m;
}

// A dashed polyline: 2 on, 5 off, round caps, marching by `offset`. The design
// draws it with stroke-dasharray="2 5" and animates stroke-dashoffset.
void DashedLine(ImDrawList *draw, ImVec2 a, ImVec2 b, ImU32 colour, float width,
                float dash, float gap, float offset) {
	const float dx  = b.x - a.x, dy = b.y - a.y;
	const float len = std::sqrt(dx * dx + dy * dy);
	if (len <= 0.0f)
		return;
	const float ux = dx / len, uy = dy / len;
	const float period = dash + gap;

	for (float t = -Wrap(offset, period); t < len; t += period) {
		const float s0 = t < 0.0f ? 0.0f : t;
		const float s1 = t + dash > len ? len : t + dash;
		if (s1 <= s0)
			continue;
		draw->AddLine(ImVec2(a.x + ux * s0, a.y + uy * s0),
		              ImVec2(a.x + ux * s1, a.y + uy * s1), colour, width);
	}
}

} // namespace

void CityMap::Draw(ImDrawList *draw, ImVec2 topLeft, ImVec2 size, const Theme &theme,
                   const MapRoute &route, float seconds, bool animate) {
	if (size.x <= 0.0f || size.y <= 0.0f)
		return;

	const Rot rot = Rotation(kAngleDeg);

	// One tile "up" the rotated grid per kAppDriftSeconds, then it loops -
	// the tile is seamless, so the loop is invisible.
	const float phase = animate ? Wrap(seconds / kAppDriftSeconds, 1.0f) : 0.0f;
	const ImVec2 drift = rot.Apply(0.0f, -kTile * phase);

	draw->PushClipRect(topLeft, ImVec2(topLeft.x + size.x, topLeft.y + size.y), true);

	// Which tiles can reach the box: take the box's corners back into tile
	// space and cover their bounding range.
	float minX = 1e9f, minY = 1e9f, maxX = -1e9f, maxY = -1e9f;
	const ImVec2 corners[4] = {ImVec2(0, 0), ImVec2(size.x, 0), ImVec2(0, size.y), size};
	for (const ImVec2 &c : corners) {
		const ImVec2 t = rot.Undo(c.x - drift.x, c.y - drift.y);
		minX = ImMin(minX, t.x);
		maxX = ImMax(maxX, t.x);
		minY = ImMin(minY, t.y);
		maxY = ImMax(maxY, t.y);
	}

	const int i0 = static_cast<int>(std::floor(minX / kTile)) - 1;
	const int i1 = static_cast<int>(std::floor(maxX / kTile)) + 1;
	const int j0 = static_cast<int>(std::floor(minY / kTile)) - 1;
	const int j1 = static_cast<int>(std::floor(maxY / kTile)) + 1;

	const ImVec2 pivot(topLeft.x + drift.x, topLeft.y + drift.y);
	const int    vtxStart = draw->VtxBuffer.Size;

	for (int j = j0; j <= j1; ++j) {
		for (int i = i0; i <= i1; ++i) {
			const float ox = pivot.x + i * kTile;
			const float oy = pivot.y + j * kTile;
			for (int k = 0; k < kTileCount; ++k) {
				const TileRect &r = kTile900[k];
				const ImVec2    a(ox + r.x, oy + r.y);
				const ImVec2    b(a.x + r.w, a.y + r.h);
				if (r.kind == 2)
					draw->AddRect(a, b, theme.mapLot, kBlockRound, 0, 1.0f);
				else
					draw->AddRectFilled(a, b, r.kind == 1 ? theme.mapBlock2 : theme.bgSubtle,
					                    kBlockRound);
			}
		}
	}

	// The route and its dots ride the same rotated grid, so they go in the
	// same vertex range and get turned with everything else.
	const float march = animate ? Wrap(seconds / kAppMarchSeconds, 1.0f) * 14.0f : 0.0f;
	const float pulse =
	    animate ? 1.0f + (kAppPulseScale - 1.0f) *
	                         0.5f * (1.0f - std::cos(seconds / kAppPulseSeconds * 6.2831853f))
	            : 1.0f;

	for (int j = j0; j <= j1; ++j) {
		const float oy = pivot.y + j * kTile;
		for (int i = i0; i <= i1; ++i) {
			const float ox = pivot.x + i * kTile;

			const ImVec2 first(ox + route.x0, oy + route.y0);
			const ImVec2 bend(ox + route.x0, oy + route.y1);
			const ImVec2 second(ox + route.x1, oy + route.y1);

			DashedLine(draw, first, bend, theme.mapRoute, 2.0f, 2.0f, 5.0f, march);
			DashedLine(draw, bend, second, theme.mapRoute, 2.0f, 2.0f, 5.0f, march);

			draw->AddCircleFilled(first, 13.0f * pulse, WithAlpha(theme.actionBg, 0.10f), 20);
			draw->AddCircleFilled(first, 4.5f, theme.actionBg, 16);
			draw->AddCircleFilled(second, 13.0f * pulse, WithAlpha(theme.brandRed, 0.16f), 20);
			draw->AddCircleFilled(second, 4.5f, theme.brandRed, 16);
		}
	}

	RotateVertices(draw, vtxStart, pivot, rot);
	draw->PopClipRect();
}

void CityMap::DrawFade(ImDrawList *draw, ImVec2 topLeft, ImVec2 size, const Theme &theme,
                       const MapFade &fade) {
	const ImU32 solid = fade.colour ? fade.colour : theme.bgWindow;
	const ImU32 clear = WithAlpha(solid, 0.0f);

	const float span = fade.vertical ? size.y : size.x;
	const float a    = span * fade.from;
	const float b    = span * fade.to;
	if (b <= a)
		return;

	const ImVec2 bottomRight(topLeft.x + size.x, topLeft.y + size.y);

	if (fade.vertical) {
		draw->AddRectFilledMultiColor(ImVec2(topLeft.x, topLeft.y + a),
		                              ImVec2(bottomRight.x, topLeft.y + b), clear, clear,
		                              solid, solid);
		if (b < span)
			draw->AddRectFilled(ImVec2(topLeft.x, topLeft.y + b), bottomRight, solid);
	} else {
		draw->AddRectFilledMultiColor(ImVec2(topLeft.x + a, topLeft.y),
		                              ImVec2(topLeft.x + b, bottomRight.y), solid, clear,
		                              clear, solid);
		if (a > 0.0f)
			draw->AddRectFilled(topLeft, ImVec2(topLeft.x + a, bottomRight.y), solid);
	}
}

} // namespace ui
