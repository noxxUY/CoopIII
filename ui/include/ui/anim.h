// Motion.
//
// The screens in design/screens/ are still images, so none of this is written
// down there the way a colour is. What is written down is the rule, in
// design/DESIGN.md §5: motion is subtle, and it stops when the OS asks it to
// (SystemParametersInfo SPI_GETCLIENTAREAANIMATION).
//
// That switch governs two different things, and they are kept apart here.
// Ambient motion runs on its own and never stops - the city map's drift, the
// pulse on the server's Running dot, the sheen on a progress bar, a spinner.
// That is what §5 is about and it goes off when Windows asks. A transition
// happens because somebody clicked: a hover coming up, a switch travelling, a
// dialog arriving. Those are the interface answering, they are over in a fifth
// of a second, and turning them off would not make the UI calmer, only
// abrupt - so they stay. What reduced motion takes off them is the distance
// they travel, which is Travel() below.
//
// Two kinds of thing live here. Values that remember: a number keyed by a
// widget's id that walks toward whatever it is given, frame-rate
// independently, so a call site can ask for "1 when hovered" and get a fade
// for free. And transforms over vertices already drawn, because ImGui has no
// notion of a group's opacity - a dialog is a few hundred vertices by the time
// it exists, and fading it in means going back over them.
#pragma once

#include <imgui.h>

namespace ui {

// Called by App once a frame, before the frame callback. `ambient` is
// App::Animates().
void BeginMotionFrame(float deltaSeconds, bool ambient);

// Seconds the last frame took, clamped: a frame lost to a folder picker or a
// window drag must not teleport everything that was mid-flight.
float Delta();

// Whether motion that runs on its own is wanted. False when the OS asked for
// none; skip drawing the moving thing entirely.
bool Ambient();

// How far a transition may slide: 1 normally, 0 when the OS asked for no
// animation. Multiply any offset by it, and a reduced-motion machine gets the
// cross-fade with none of the travel.
float Travel();

// Overrides what BeginMotionFrame was told, for the rest of this frame. Only
// ui-gallery uses it, so the ambient motion can be looked at on a machine
// whose OS has asked for none. The three shipped apps never call it.
void ForceAmbient(bool on);

// ---- curves ---------------------------------------------------------------

float EaseOut(float t);     // cubic: quick, then settling
float EaseInOut(float t);
float EaseBack(float t);    // overshoots a little, for something arriving

// One step toward `target`, frame-rate independent. `speed` is e-foldings a
// second: 30 is a snap, 14 a glide, 5 a drift. Settles exactly, so a value
// that has arrived stops costing anything.
float Approach(float current, float target, float speed);

// ---- values that remember -------------------------------------------------
//
// Keyed either by a widget's own ImGui id, which is what a hover state uses,
// or by a name, which is what anything without a widget uses - a dialog being
// open, which screen the installer is on. Calling one twice in a frame with
// the same key returns the same number rather than stepping it twice.
//
// The two kinds of key cannot collide. A name is hashed into a namespace of
// its own, so Appear("##options") is not the button whose id is "##options" -
// which it was, once, and the dialog was then only on screen while the pointer
// sat on the button that opened it.

// The id a name hashes to, for a caller that needs to hold one.
ImGuiID MotionKey(const char *text);

float Animate(ImGuiID key, float target, float speed = 14.0f);
float Animate(const char *key, float target, float speed = 14.0f);

// 0 closed, 1 open, eased both ways. Exactly 0 means "gone", so a caller can
// skip drawing a dialog that has finished closing.
float Appear(ImGuiID key, bool open, float speed = 16.0f);
float Appear(const char *key, bool open, float speed = 16.0f);

// Hover and press as one number: 0 at rest, 0.5 hovered, 1 held. Pressing
// reads as instant, letting go settles.
float Pointer(ImGuiID key, bool hovered, bool held);

// Seconds since the key was first seen, or since Restart(). For one-shots -
// a spin, a flash, a "copied" tick.
float Since(ImGuiID key);
float Since(const char *key);
void  Restart(ImGuiID key);
void  Restart(const char *key);

// ---- transforming what was already drawn ----------------------------------

// Marks where a draw list is, then moves, scales or fades everything added
// after that. Hit testing is unaffected, which is what makes it safe for an
// entrance: the widget is already where it will end up, and only the pixels
// are still arriving.
//
// One catch, and it is the only one: a group cannot span ImGui::BeginChild.
// A child window draws into a list of its own, and this holds one list.
struct DrawGroup {
	explicit DrawGroup(ImDrawList *list);

	void Fade(float alpha) const;
	void Move(ImVec2 by) const;
	void Scale(ImVec2 around, float by) const;
	void Rotate(ImVec2 around, float radians) const;

	ImDrawList *draw = nullptr;
	int         at   = 0;
};

} // namespace ui
