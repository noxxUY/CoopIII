#include "ui/icons.h"

#include "ui/anim.h"

#include <imgui_internal.h>

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace ui {
namespace {

// ---- the shapes -----------------------------------------------------------
//
// One string per icon, shapes separated by ';'. The first letter says what the
// shape is, and the rest is the design's own SVG:
//
//   P <path data>            stroked path
//   F <path data>            filled path
//   C cx cy r                stroked circle
//   D cx cy r                filled circle
//   R x y w h rx             stroked rect
//   B x y w h rx             filled rect
//
// Copied from design/screens/*.dc.html. Do not tidy the numbers: they are the
// design's.
const char *Shapes(Icon icon) {
	switch (icon) {
	case Icon::None:
		return "";
	case Icon::CircleCheck:
		return "C 12 12 9;P M8 12.5l2.7 2.7L16 9.5";
	case Icon::CircleX:
		return "C 12 12 9;P M9 9l6 6M15 9l-6 6";
	case Icon::Info:
		return "C 12 12 9;P M12 11v5M12 8h.01";
	case Icon::TriangleAlert:
		return "P M12 4 2.5 20.5h19zM12 10v4M12 17.5h.01";
	case Icon::Folder:
		return "P M3.5 7A1.5 1.5 0 0 1 5 5.5h4l2 2h8A1.5 1.5 0 0 1 20.5 9v8.5A1.5 "
		       "1.5 0 0 1 19 19H5a1.5 1.5 0 0 1-1.5-1.5z";
	case Icon::Play:
		return "F M7.5 5.5v13L18.5 12z";
	case Icon::Download:
		return "P M12 4v11M7 10.5l5 5 5-5M5 20h14";
	case Icon::RotateCcw:
		return "P M20 12a8 8 0 1 1-8-8c2.2 0 4.3.9 5.9 2.4L20 8.5;P M20 4v4.5h-4.5";
	case Icon::Copy:
		return "R 8 8 13 13 2;P M4.5 15.5A1.5 1.5 0 0 1 3 14V4.5A1.5 1.5 0 0 1 4.5 "
		       "3H14a1.5 1.5 0 0 1 1.5 1.5";
	case Icon::Check:
		return "P M5 12.5l5 5L19 7";
	case Icon::UserX:
		return "P M15 20v-1.5a4 4 0 0 0-4-4H7a4 4 0 0 0-4 4V20;C 9 7.5 3.5;P M17 "
		       "8.5l4 4M21 8.5l-4 4";
	case Icon::Sun:
		return "C 12 12 4;P M12 2v2M12 20v2M4.93 4.93l1.41 1.41M17.66 17.66l1.41 "
		       "1.41M2 12h2M20 12h2M6.34 17.66l-1.41 1.41M19.07 4.93l-1.41 1.41";
	case Icon::Cloud:
		return "P M6.5 18.5a4.5 4.5 0 0 1 .3-9 6 6 0 0 1 11.4 1.9 3.6 3.6 0 0 1-.7 "
		       "7.1z";
	case Icon::CloudRain:
		return "P M6.5 15.5a4.5 4.5 0 0 1 .3-9 6 6 0 0 1 11.4 1.9 3.6 3.6 0 0 1-.7 "
		       "7.1z;P M8.5 18.5 7.5 21M12.5 18.5 11.5 21M16.5 18.5 15.5 21";
	case Icon::CloudFog:
		return "P M6.5 14.5a4.5 4.5 0 0 1 .3-9 6 6 0 0 1 11.4 1.9 3.6 3.6 0 0 1-.7 "
		       "7.1z;P M5 18h14M7 21.5h10";
	case Icon::Image:
		return "R 3 4 18 16 2;C 9 9.5 1.8;P M21 15.5l-5-5L5 20";
	case Icon::ArrowUpRight:
		return "P M7 17 17 7M8 7h9v9";
	case Icon::Cpu:
		return "R 5 5 14 14 2;R 9 9 6 6 1;P M9 2v3M15 2v3M9 19v3M15 19v3M2 9h3M2 "
		       "15h3M19 9h3M19 15h3";
	case Icon::Users:
		return "C 9 8 3.5;P M2.5 20v-1a5 5 0 0 1 5-5h3a5 5 0 0 1 5 5v1;P M16 4.6a3.5 "
		       "3.5 0 0 1 0 6.8M21.5 20v-1a5 5 0 0 0-3.5-4.8";
	case Icon::Layers:
		return "P M12 3 2.5 8 12 13l9.5-5z;P M2.5 12.5 12 17.5l9.5-5;P M2.5 16.5 12 "
		       "21.5l9.5-5";
	case Icon::ShieldCheck:
		return "P M12 3 5 6v5.5c0 4.3 3 7.9 7 9.5 4-1.6 7-5.2 7-9.5V6z;P M9 12l2 2 4-4";
	case Icon::Gear:
		return "P M19.13 11.00 L21.76 11.15 L21.76 12.85 L19.13 13.00 L17.75 16.33 "
		       "L19.51 18.30 L18.30 19.51 L16.33 17.75 L13.00 19.13 L12.85 21.76 "
		       "L11.15 21.76 L11.00 19.13 L7.67 17.75 L5.70 19.51 L4.49 18.30 L6.25 "
		       "16.33 L4.87 13.00 L2.24 12.85 L2.24 11.15 L4.87 11.00 L6.25 7.67 "
		       "L4.49 5.70 L5.70 4.49 L7.67 6.25 L11.00 4.87 L11.15 2.24 L12.85 2.24 "
		       "L13.00 4.87 L16.33 6.25 L18.30 4.49 L19.51 5.70 L17.75 7.67 Z;C 12 12 3";
	case Icon::Stop:
		return "B 5 5 14 14 2";
	case Icon::Minimize:
		return "P M6 12h12";
	case Icon::Maximize:
		return "R 6.5 6.5 11 11 1.5";
	case Icon::Close:
		return "P M6.5 6.5l11 11M17.5 6.5l-11 11";
	case Icon::ChevronDown:
		return "P M6 9.5l6 6 6-6";
	case Icon::Plus:
		return "P M12 5v14M5 12h14";
	case Icon::Minus:
		return "P M5 12h14";
	}
	return "";
}

// ---- a very small SVG path reader ----------------------------------------
//
// Enough for the shapes above: M m L l H h V v C c A a Z z, plus the implicit
// line-to that follows a move-to. Curves and arcs are flattened; at 16 to 24 px
// a handful of segments is already past what the screen can show.

struct Reader {
	const char *p;

	void Space() {
		while (*p == ' ' || *p == ',' || *p == '\t' || *p == '\n' || *p == '\r')
			++p;
	}

	bool More() {
		Space();
		return *p != '\0';
	}

	bool AtNumber() {
		Space();
		return (*p >= '0' && *p <= '9') || *p == '-' || *p == '+' || *p == '.';
	}

	float Number() {
		Space();
		char       *end = nullptr;
		const float v   = std::strtof(p, &end);
		p               = end ? end : p;
		return v;
	}

	char Command() {
		Space();
		return *p ? *p++ : '\0';
	}
};

constexpr int   kCurveSegments = 12;
constexpr float kPi            = 3.14159265358979323846f;

void FlattenCubic(std::vector<ImVec2> &out, ImVec2 p0, ImVec2 p1, ImVec2 p2, ImVec2 p3) {
	for (int i = 1; i <= kCurveSegments; ++i) {
		const float t = static_cast<float>(i) / kCurveSegments;
		const float u = 1.0f - t;
		const float a = u * u * u, b = 3 * u * u * t, c = 3 * u * t * t, d = t * t * t;
		out.push_back(ImVec2(a * p0.x + b * p1.x + c * p2.x + d * p3.x,
		                     a * p0.y + b * p1.y + c * p2.y + d * p3.y));
	}
}

// SVG elliptical arc, endpoint parameterisation (the "a rx ry rot laf sf x y"
// form), converted to a centre and swept. Straight out of the SVG spec's
// implementation notes.
void FlattenArc(std::vector<ImVec2> &out, ImVec2 from, float rx, float ry, float rotDeg,
                bool largeArc, bool sweep, ImVec2 to) {
	if (rx == 0.0f || ry == 0.0f) {
		out.push_back(to);
		return;
	}
	rx = std::fabs(rx);
	ry = std::fabs(ry);

	const float phi  = rotDeg * kPi / 180.0f;
	const float cosP = std::cos(phi), sinP = std::sin(phi);

	const float dx2 = (from.x - to.x) * 0.5f;
	const float dy2 = (from.y - to.y) * 0.5f;
	const float x1  = cosP * dx2 + sinP * dy2;
	const float y1  = -sinP * dx2 + cosP * dy2;

	// Scale the radii up if they are too small to reach, as the spec requires.
	const float lambda = (x1 * x1) / (rx * rx) + (y1 * y1) / (ry * ry);
	if (lambda > 1.0f) {
		const float s = std::sqrt(lambda);
		rx *= s;
		ry *= s;
	}

	const float rxSq = rx * rx, rySq = ry * ry;
	float       num  = rxSq * rySq - rxSq * y1 * y1 - rySq * x1 * x1;
	const float den  = rxSq * y1 * y1 + rySq * x1 * x1;
	if (num < 0.0f)
		num = 0.0f;
	float coef = den > 0.0f ? std::sqrt(num / den) : 0.0f;
	if (largeArc == sweep)
		coef = -coef;

	const float cx1 = coef * rx * y1 / ry;
	const float cy1 = -coef * ry * x1 / rx;
	const ImVec2 centre(cosP * cx1 - sinP * cy1 + (from.x + to.x) * 0.5f,
	                    sinP * cx1 + cosP * cy1 + (from.y + to.y) * 0.5f);

	auto angle = [](float ux, float uy, float vx, float vy) {
		const float dot  = ux * vx + uy * vy;
		const float len  = std::sqrt((ux * ux + uy * uy) * (vx * vx + vy * vy));
		float       a    = len > 0.0f ? std::acos(ImClamp(dot / len, -1.0f, 1.0f)) : 0.0f;
		if (ux * vy - uy * vx < 0.0f)
			a = -a;
		return a;
	};

	const float ux    = (x1 - cx1) / rx, uy = (y1 - cy1) / ry;
	const float vx    = (-x1 - cx1) / rx, vy = (-y1 - cy1) / ry;
	const float start = angle(1.0f, 0.0f, ux, uy);
	float       sweepA = angle(ux, uy, vx, vy);
	if (!sweep && sweepA > 0.0f)
		sweepA -= 2.0f * kPi;
	else if (sweep && sweepA < 0.0f)
		sweepA += 2.0f * kPi;

	const int steps = ImMax(2, static_cast<int>(std::fabs(sweepA) / (kPi / 8.0f)) + 1);
	for (int i = 1; i <= steps; ++i) {
		const float a  = start + sweepA * (static_cast<float>(i) / steps);
		const float px = rx * std::cos(a), py = ry * std::sin(a);
		out.push_back(ImVec2(centre.x + cosP * px - sinP * py,
		                     centre.y + sinP * px + cosP * py));
	}
}

struct SubPath {
	std::vector<ImVec2> points;
	bool                closed = false;
};

void ParsePath(const char *d, std::vector<SubPath> &out) {
	Reader  r{d};
	ImVec2  cur(0, 0), start(0, 0);
	ImVec2  lastCtrl(0, 0);
	char    prev = '\0';
	SubPath sp;

	auto flush = [&] {
		if (sp.points.size() > 1)
			out.push_back(sp);
		sp.points.clear();
		sp.closed = false;
	};

	while (r.More()) {
		char cmd;
		if (r.AtNumber()) {
			// A repeated command: after an M the implicit one is L.
			cmd = (prev == 'M') ? 'L' : (prev == 'm') ? 'l' : prev;
			if (cmd == '\0')
				break;
		} else {
			cmd = r.Command();
		}
		prev = cmd;

		const bool rel = cmd >= 'a' && cmd <= 'z';
		switch (cmd | 0x20) {
		case 'm': {
			const float x = r.Number(), y = r.Number();
			flush();
			cur   = rel ? ImVec2(cur.x + x, cur.y + y) : ImVec2(x, y);
			start = cur;
			sp.points.push_back(cur);
			break;
		}
		case 'l': {
			const float x = r.Number(), y = r.Number();
			cur = rel ? ImVec2(cur.x + x, cur.y + y) : ImVec2(x, y);
			sp.points.push_back(cur);
			break;
		}
		case 'h': {
			const float x = r.Number();
			cur = rel ? ImVec2(cur.x + x, cur.y) : ImVec2(x, cur.y);
			sp.points.push_back(cur);
			break;
		}
		case 'v': {
			const float y = r.Number();
			cur = rel ? ImVec2(cur.x, cur.y + y) : ImVec2(cur.x, y);
			sp.points.push_back(cur);
			break;
		}
		case 'c': {
			const float x1 = r.Number(), y1 = r.Number();
			const float x2 = r.Number(), y2 = r.Number();
			const float x = r.Number(), y = r.Number();
			const ImVec2 c1 = rel ? ImVec2(cur.x + x1, cur.y + y1) : ImVec2(x1, y1);
			const ImVec2 c2 = rel ? ImVec2(cur.x + x2, cur.y + y2) : ImVec2(x2, y2);
			const ImVec2 to = rel ? ImVec2(cur.x + x, cur.y + y) : ImVec2(x, y);
			if (sp.points.empty())
				sp.points.push_back(cur);
			FlattenCubic(sp.points, cur, c1, c2, to);
			lastCtrl = c2;
			cur      = to;
			break;
		}
		case 's': {
			const float x2 = r.Number(), y2 = r.Number();
			const float x = r.Number(), y = r.Number();
			const ImVec2 c1 = ImVec2(2 * cur.x - lastCtrl.x, 2 * cur.y - lastCtrl.y);
			const ImVec2 c2 = rel ? ImVec2(cur.x + x2, cur.y + y2) : ImVec2(x2, y2);
			const ImVec2 to = rel ? ImVec2(cur.x + x, cur.y + y) : ImVec2(x, y);
			if (sp.points.empty())
				sp.points.push_back(cur);
			FlattenCubic(sp.points, cur, c1, c2, to);
			lastCtrl = c2;
			cur      = to;
			break;
		}
		case 'a': {
			const float rx = r.Number(), ry = r.Number(), rot = r.Number();
			const bool  laf = r.Number() != 0.0f;
			const bool  sf  = r.Number() != 0.0f;
			const float x = r.Number(), y = r.Number();
			const ImVec2 to = rel ? ImVec2(cur.x + x, cur.y + y) : ImVec2(x, y);
			if (sp.points.empty())
				sp.points.push_back(cur);
			FlattenArc(sp.points, cur, rx, ry, rot, laf, sf, to);
			cur = to;
			break;
		}
		case 'z':
			sp.closed = true;
			flush();
			cur = start;
			break;
		default:
			// Unknown command: stop rather than spin.
			flush();
			return;
		}
	}
	flush();
}

struct Painter {
	ImDrawList *draw;
	ImVec2      origin;
	float       scale;
	ImU32       colour;
	float       stroke;

	ImVec2 At(float x, float y) const {
		return ImVec2(origin.x + x * scale, origin.y + y * scale);
	}

	void Stroke(const SubPath &sp) const {
		if (sp.points.size() < 2)
			return;
		std::vector<ImVec2> pts;
		pts.reserve(sp.points.size());
		for (const ImVec2 &p : sp.points)
			pts.push_back(At(p.x, p.y));

		const float  r     = stroke * 0.5f;
		const size_t count = pts.size();

		// Turn angle at each interior vertex, to decide how this path has to be
		// stroked. ImDrawList only miters, and a miter at a sharp corner shoots
		// well past where the design's rounded join stops - the layers icon's
		// apex overhangs its 24 grid by nearly 2 px that way.
		auto sharpAt = [&](size_t i) {
			const ImVec2 &prev = pts[(i + count - 1) % count];
			const ImVec2 &here = pts[i % count];
			const ImVec2 &next = pts[(i + 1) % count];
			const ImVec2  a(here.x - prev.x, here.y - prev.y);
			const ImVec2  b(next.x - here.x, next.y - here.y);
			const float   la = std::sqrt(a.x * a.x + a.y * a.y);
			const float   lb = std::sqrt(b.x * b.x + b.y * b.y);
			if (la <= 0.0f || lb <= 0.0f)
				return false;
			// Turns gentler than about 30 degrees miter to nothing worth fixing.
			return (a.x * b.x + a.y * b.y) / (la * lb) < 0.87f;
		};

		bool         sharp = false;
		const size_t first = sp.closed ? 0 : 1;
		const size_t last  = sp.closed ? count : count - 1;
		for (size_t i = first; i < last && !sharp; ++i)
			sharp = sharpAt(i);

		if (!sharp || stroke <= 1.0f) {
			// A smooth path: one polyline, whose joins are gentle enough that a
			// miter and a round join draw the same pixels.
			draw->AddPolyline(pts.data(), static_cast<int>(count), colour,
			                  sp.closed ? ImDrawFlags_Closed : 0, stroke);
		} else {
			// stroke-linejoin: round, built the way a round join is defined -
			// each segment on its own, with a disc over every corner.
			const size_t segments = sp.closed ? count : count - 1;
			for (size_t i = 0; i < segments; ++i)
				draw->AddLine(pts[i], pts[(i + 1) % count], colour, stroke);
			for (size_t i = first; i < last; ++i)
				draw->AddCircleFilled(pts[i % count], r, colour, 12);
		}

		// stroke-linecap: round. ImDrawList has no cap style either, and at this
		// size a butt cap on the tick inside a circle-check is visible.
		if (!sp.closed && stroke > 1.0f) {
			draw->AddCircleFilled(pts.front(), r, colour, 10);
			draw->AddCircleFilled(pts.back(), r, colour, 10);
		}
	}

	void Fill(const std::vector<SubPath> &paths) const {
		for (const SubPath &sp : paths) {
			if (sp.points.size() < 3)
				continue;
			std::vector<ImVec2> pts;
			pts.reserve(sp.points.size());
			for (const ImVec2 &p : sp.points)
				pts.push_back(At(p.x, p.y));
			draw->AddConvexPolyFilled(pts.data(), static_cast<int>(pts.size()), colour);
		}
	}
};

} // namespace

void DrawIcon(ImDrawList *draw, Icon icon, ImVec2 topLeft, float size, ImU32 colour,
              float stroke, float rotation) {
	const char *shapes = Shapes(icon);
	if (!shapes || !*shapes)
		return;

	// Drawn square and upright, then turned: the paths are the design's own and
	// nothing here should be re-deriving them at an angle.
	const DrawGroup group(draw);

	Painter painter{draw, topLeft, size / 24.0f, colour, stroke * (size / 24.0f)};
	if (painter.stroke < 1.0f)
		painter.stroke = 1.0f;

	const char *s = shapes;
	while (*s) {
		const char  kind = *s++;
		const char *end  = std::strchr(s, ';');
		const size_t len = end ? static_cast<size_t>(end - s) : std::strlen(s);

		switch (kind) {
		case 'P':
		case 'F': {
			std::string          d(s, len);
			std::vector<SubPath> paths;
			ParsePath(d.c_str(), paths);
			if (kind == 'F') {
				painter.Fill(paths);
			} else {
				for (const SubPath &sp : paths)
					painter.Stroke(sp);
			}
			break;
		}
		case 'C':
		case 'D': {
			Reader      r{s};
			const float cx = r.Number(), cy = r.Number(), rad = r.Number();
			const ImVec2 c = painter.At(cx, cy);
			if (kind == 'D')
				draw->AddCircleFilled(c, rad * painter.scale, colour, 0);
			else
				draw->AddCircle(c, rad * painter.scale, colour, 0, painter.stroke);
			break;
		}
		case 'R':
		case 'B': {
			Reader      r{s};
			const float x = r.Number(), y = r.Number();
			const float w = r.Number(), h = r.Number(), rad = r.Number();
			const ImVec2 a = painter.At(x, y);
			const ImVec2 b = painter.At(x + w, y + h);
			if (kind == 'B')
				draw->AddRectFilled(a, b, colour, rad * painter.scale);
			else
				draw->AddRect(a, b, colour, rad * painter.scale, 0, painter.stroke);
			break;
		}
		default:
			break;
		}

		if (!end)
			break;
		s = end + 1;
	}

	group.Rotate(ImVec2(topLeft.x + size * 0.5f, topLeft.y + size * 0.5f), rotation);
}

void IconItem(Icon icon, float size, ImU32 colour, float stroke) {
	const ImVec2 pos = ImGui::GetCursorScreenPos();
	DrawIcon(ImGui::GetWindowDrawList(), icon, pos, size, colour, stroke);
	ImGui::Dummy(ImVec2(size, size));
}

} // namespace ui
