#include "ui/anim.h"

#include <imgui_internal.h>

#include <cmath>
#include <iterator>
#include <unordered_map>

namespace ui {
namespace {

// One entry per key. `frame` is what stops a value stepping twice when two
// call sites ask for it in the same frame, and what lets old keys be swept:
// a screen the installer has left behind should not keep paying for its
// hover states.
struct Value {
	float value     = 0.0f;
	float startedAt = 0.0f;
	int   frame     = -1;
	bool  seeded    = false;
};

std::unordered_map<ImGuiID, Value> g_values;

float g_delta   = 1.0f / 60.0f;
float g_time    = 0.0f;
bool  g_ambient = true;
int   g_frame   = 0;

// String keys get a namespace of their own rather than ImGui's.
//
// Without this, Appear("##options") and the hover state of the button whose id
// is "##options" are the same entry - and the frame guard below then hands the
// second caller whatever the first one wrote, silently. That is not a
// hypothetical: it is what made the server's options dialog visible only while
// the pointer sat on the button that opened it. A widget's own animation is
// keyed by its ImGui id and belongs in ImGui's namespace; anything keyed by a
// name of its own belongs here, and the two can no longer meet.
constexpr ImGuiID kStringSeed = 0xC003111Du;

Value &Entry(ImGuiID key) {
	Value &v = g_values[key];
	if (!v.seeded) {
		v.seeded    = true;
		v.startedAt = g_time;
	}
	return v;
}

void Sweep() {
	if (g_frame % 600 != 0 || g_values.size() < 256)
		return;
	for (auto it = g_values.begin(); it != g_values.end();)
		it = (g_frame - it->second.frame > 600) ? g_values.erase(it) : std::next(it);
}

} // namespace

void BeginMotionFrame(float deltaSeconds, bool ambient) {
	// A frame lost to a folder picker, a window drag or a stalled download is
	// not a long frame, it is a gap. Treat anything past a fifteenth of a
	// second as one frame's worth so nothing mid-flight teleports.
	g_delta   = deltaSeconds > 0.0f ? (deltaSeconds < 1.0f / 15.0f ? deltaSeconds : 1.0f / 15.0f)
	                                : 1.0f / 60.0f;
	g_time += g_delta;
	g_ambient = ambient;
	++g_frame;
	Sweep();
}

float Delta() { return g_delta; }
bool  Ambient() { return g_ambient; }
float Travel() { return g_ambient ? 1.0f : 0.0f; }
void  ForceAmbient(bool on) { g_ambient = on; }

// ---- curves ---------------------------------------------------------------

float EaseOut(float t) {
	if (t <= 0.0f)
		return 0.0f;
	if (t >= 1.0f)
		return 1.0f;
	const float u = 1.0f - t;
	return 1.0f - u * u * u;
}

float EaseInOut(float t) {
	if (t <= 0.0f)
		return 0.0f;
	if (t >= 1.0f)
		return 1.0f;
	return t < 0.5f ? 4.0f * t * t * t : 1.0f - std::pow(-2.0f * t + 2.0f, 3.0f) * 0.5f;
}

float EaseBack(float t) {
	// Exact at the ends rather than a rounding error away from them: a tick
	// that settles at 0.99999994 of its size is a tick that never settles.
	if (t <= 0.0f)
		return 0.0f;
	if (t >= 1.0f)
		return 1.0f;
	// A tenth of the distance past the target and back. Any more and a tick
	// appearing starts to look like a toy.
	constexpr float k = 1.10158f;
	const float     u = t - 1.0f;
	return 1.0f + u * u * ((k + 1.0f) * u + k);
}

float Approach(float current, float target, float speed) {
	// Not gated on the OS switch: this is what a transition is made of, and a
	// transition is the interface answering a click.
	if (speed <= 0.0f)
		return target;
	const float k   = 1.0f - std::exp(-speed * g_delta);
	float       out = current + (target - current) * k;
	// Settle exactly. Without this a value crawls at target for ever, Appear()
	// never reaches 0, and a closed dialog is drawn every frame at an alpha
	// nobody can see.
	if (std::fabs(target - out) < 0.0008f)
		out = target;
	return out;
}

// ---- values that remember -------------------------------------------------

ImGuiID MotionKey(const char *text) { return ImHashStr(text, 0, kStringSeed); }

float Animate(ImGuiID key, float target, float speed) {
	Value &v = Entry(key);
	if (v.frame == g_frame)
		return v.value;      // already stepped this frame by another call site
	if (v.frame < 0)
		v.value = target;    // first sight of it: start where it belongs
	else
		v.value = Approach(v.value, target, speed);
	v.frame = g_frame;
	return v.value;
}

float Animate(const char *key, float target, float speed) {
	return Animate(MotionKey(key), target, speed);
}

float Appear(ImGuiID key, bool open, float speed) {
	return Animate(key, open ? 1.0f : 0.0f, speed);
}

float Appear(const char *key, bool open, float speed) {
	return Appear(MotionKey(key), open, speed);
}

float Pointer(ImGuiID key, bool hovered, bool held) {
	const float target = held ? 1.0f : (hovered ? 0.5f : 0.0f);
	return Animate(key, target, held ? 34.0f : 16.0f);
}

float Since(ImGuiID key) {
	Value &v = Entry(key);
	v.frame  = g_frame;
	return g_time - v.startedAt;
}

float Since(const char *key) { return Since(MotionKey(key)); }

void Restart(ImGuiID key) {
	Value &v    = Entry(key);
	v.startedAt = g_time;
	v.frame     = g_frame;
}

void Restart(const char *key) { Restart(MotionKey(key)); }

// ---- transforming what was already drawn ----------------------------------

DrawGroup::DrawGroup(ImDrawList *list) : draw(list), at(list ? list->VtxBuffer.Size : 0) {}

void DrawGroup::Fade(float alpha) const {
	if (!draw || alpha >= 1.0f)
		return;
	const float       a   = ImClamp(alpha, 0.0f, 1.0f);
	ImDrawVert       *v   = draw->VtxBuffer.Data + at;
	ImDrawVert *const end = draw->VtxBuffer.Data + draw->VtxBuffer.Size;
	for (; v < end; ++v) {
		const ImU32 was = (v->col >> IM_COL32_A_SHIFT) & 0xFF;
		const ImU32 now = static_cast<ImU32>(was * a + 0.5f);
		v->col = (v->col & ~(0xFFu << IM_COL32_A_SHIFT)) | (now << IM_COL32_A_SHIFT);
	}
}

void DrawGroup::Move(ImVec2 by) const {
	if (!draw || (by.x == 0.0f && by.y == 0.0f))
		return;
	ImDrawVert       *v   = draw->VtxBuffer.Data + at;
	ImDrawVert *const end = draw->VtxBuffer.Data + draw->VtxBuffer.Size;
	for (; v < end; ++v) {
		v->pos.x += by.x;
		v->pos.y += by.y;
	}
}

void DrawGroup::Rotate(ImVec2 around, float radians) const {
	if (!draw || radians == 0.0f)
		return;
	const float       c   = std::cos(radians);
	const float       s   = std::sin(radians);
	ImDrawVert       *v   = draw->VtxBuffer.Data + at;
	ImDrawVert *const end = draw->VtxBuffer.Data + draw->VtxBuffer.Size;
	for (; v < end; ++v) {
		const float x = v->pos.x - around.x;
		const float y = v->pos.y - around.y;
		v->pos.x      = around.x + x * c - y * s;
		v->pos.y      = around.y + x * s + y * c;
	}
}

void DrawGroup::Scale(ImVec2 around, float by) const {
	if (!draw || by == 1.0f)
		return;
	ImDrawVert       *v   = draw->VtxBuffer.Data + at;
	ImDrawVert *const end = draw->VtxBuffer.Data + draw->VtxBuffer.Size;
	for (; v < end; ++v) {
		v->pos.x = around.x + (v->pos.x - around.x) * by;
		v->pos.y = around.y + (v->pos.y - around.y) * by;
	}
}

} // namespace ui
