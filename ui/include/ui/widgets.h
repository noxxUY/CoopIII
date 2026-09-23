// The components in design/DESIGN.md §6.
//
// Every one takes an explicit position and size in design pixels, because the
// screens in design/screens/ are explicit about both and a layout engine's
// idea of "close enough" is exactly what this has to avoid. The design's own
// numbers go in at the call site and come out on screen.
#pragma once

#include "ui/anim.h"
#include "ui/fonts.h"
#include "ui/icons.h"
#include "ui/theme.h"

#include <imgui.h>

#include <string>
#include <vector>

namespace ui {

class App;

// ---- shape ----------------------------------------------------------------
//
// Radii from DESIGN §3: inputs and small buttons 6, primary buttons 8, cards
// and lists 10, dialogs 12, pills fully round.
namespace radius {
constexpr float kInput   = 6.0f;
constexpr float kPrimary = 8.0f;
constexpr float kCard    = 10.0f;
constexpr float kDialog  = 12.0f;
} // namespace radius

inline ImVec2 Add(ImVec2 a, ImVec2 b) { return ImVec2(a.x + b.x, a.y + b.y); }
inline ImVec2 Sub(ImVec2 a, ImVec2 b) { return ImVec2(a.x - b.x, a.y - b.y); }

struct Rect {
	ImVec2 pos;
	ImVec2 size;

	ImVec2 Max() const { return ImVec2(pos.x + size.x, pos.y + size.y); }
	ImVec2 Centre() const { return ImVec2(pos.x + size.x * 0.5f, pos.y + size.y * 0.5f); }
	Rect   Inset(float by) const {
        return {ImVec2(pos.x + by, pos.y + by), ImVec2(size.x - by * 2, size.y - by * 2)};
	}
	bool Contains(ImVec2 p) const {
		return p.x >= pos.x && p.y >= pos.y && p.x < pos.x + size.x && p.y < pos.y + size.y;
	}
};

// ---- text -----------------------------------------------------------------

// Draws one line with its top-left at `pos`. Returns how wide it came out, so
// a caller can put something after it.
float Text(ImVec2 pos, Type role, ImU32 colour, const char *text);
float TextF(ImVec2 pos, Type role, ImU32 colour, const char *fmt, ...);

// Right-aligned: `rightEdge` is where the text ends.
float TextRight(float rightEdge, float y, Type role, ImU32 colour, const char *text);

// Wraps to `width`, returns the height used.
float TextWrapped(ImVec2 pos, float width, Type role, ImU32 colour, const char *text);

ImVec2 MeasureText(Type role, const char *text);
ImVec2 MeasureWrapped(Type role, float width, const char *text);

// A line of text vertically centred in a box of `height`, which is how the
// design lines labels up with 44 and 46 px controls.
float TextMiddle(ImVec2 pos, float height, Type role, ImU32 colour, const char *text);

// ---- pointing at things ---------------------------------------------------

// A hit target at an absolute position, with the number that goes with it: 0
// at rest, 0.5 hovered, 1 held, moving between them over a few frames rather
// than snapping. The widgets below all use it; it is here because a screen
// that draws a surface of its own - a component tile, a rule, a list row -
// needs exactly the same thing.
struct Touched {
	bool    clicked = false;
	bool    hovered = false;
	bool    held    = false;
	float   t       = 0.0f;
	ImGuiID key     = 0;

	// How far into being pressed, 0 to 1. What a button sinks by.
	float Press() const;
	// A second (third, fourth) animated value belonging to the same widget.
	ImGuiID Also(unsigned n) const;
};

Touched Hotspot(const char *id, Rect box);

// The one hover state in the whole UI: a slightly brighter surface, never a
// colour change. `amount` is a Touched::t, so at rest it is the base colour,
// hovered 7% toward white (black in the light theme) and held 14%.
ImU32 Lift(ImU32 base, const Theme &theme, float amount);

// ---- title bar ------------------------------------------------------------

struct TitleBarResult {
	bool minimise = false;
	bool maximise = false;
	bool close    = false;
	bool theme    = false;   // the theme switch, when one is shown
};

// The 40 px custom title bar: logo, app name, version, then the window
// buttons at 46 x 39. Sets the app's drag region on the way out.
TitleBarResult TitleBar(App &app, const char *name, const char *version,
                        bool withMaximise, bool withThemeSwitch = true);

// ---- buttons --------------------------------------------------------------

// Primary: action.bg filled, action.fg label, radius 8. Disabled uses
// bg.disabled on border.disabled with text.muted, the way LauncherBlocked
// draws it.
bool PrimaryButton(const char *id, Rect box, const char *label, Icon icon, bool enabled,
                   const Theme &theme, Type role = Type::ButtonPrimary);

// Outline: transparent on border.control, text.button label.
bool OutlineButton(const char *id, Rect box, const char *label, Icon icon,
                   const Theme &theme, bool filled = false,
                   Type role = Type::ButtonSmall, ImU32 iconColour = 0,
                   float iconTurn = 0.0f);

// Square icon button, 36 px in the design.
bool IconButton(const char *id, Rect box, Icon icon, const Theme &theme, ImU32 colour,
                float iconSize = 16.0f, bool bordered = true, ImU32 fill = 0,
                ImU32 borderColour = 0);

// A label that behaves like a link: no chrome, underline on hover.
bool LinkText(const char *id, ImVec2 pos, Type role, ImU32 colour, const char *label);

// ---- input ----------------------------------------------------------------

// Text input with the design's frame: 46 px tall, radius 6, bg.window inside
// border.control. `label` and `hint` are drawn by the caller; this is the
// field itself.
bool TextInput(const char *id, Rect box, char *buffer, size_t bufferSize,
               const Theme &theme, const char *placeholder = nullptr,
               ImGuiInputTextFlags flags = 0);

// Field label on the left, an optional counter or note on the right, then the
// input under it. Returns the input's rect so a caller can put a Browse button
// beside it.
struct FieldResult {
	Rect inputBox;
	bool changed = false;
};
FieldResult Field(const char *id, ImVec2 pos, float width, const char *label,
                  const char *rightNote, char *buffer, size_t bufferSize,
                  const Theme &theme, float inputHeight = 46.0f,
                  ImGuiInputTextFlags flags = 0);

bool Checkbox(const char *id, ImVec2 pos, bool *value, const char *label,
              const Theme &theme);

// 48 x 28, filled with action.bg when on.
bool Switch(const char *id, ImVec2 pos, bool *value, const Theme &theme);

// Group on bg.window with a 3 px inset; the selected segment is bg.selected.
// That is the options dialog's control.
bool Segmented(const char *id, Rect box, const char *const *options, int count,
               int *selected, const Theme &theme);

// The console header's version: no group behind it, each label sized to its
// own text, and only the selected one has a background. Right-aligned, ending
// at `rightEdge`; `changed` says whether the selection moved.
float SegmentedFlat(const char *id, float rightEdge, float centreY,
                    const char *const *options, int count, int *selected,
                    const Theme &theme, bool *changed = nullptr);

// ---- surfaces -------------------------------------------------------------

void Card(Rect box, const Theme &theme, float rounding = radius::kCard,
          ImU32 fill = 0, ImU32 border = 0);

// "Required" and friends: a small outlined label.
void Chip(ImVec2 pos, const char *label, const Theme &theme);

// "Running": a fully round pill with a dot. `pulse` gives the dot a halo that
// breathes, which is the server window saying it is still there.
float Pill(ImVec2 pos, const char *label, ImU32 colour, ImU32 fill, ImU32 border,
           bool pulse = false);

// 8 px bar, fill in action.bg. The fill eases toward `fraction` and carries a
// sheen while it is short of full, so `id` has to be unique per bar.
void ProgressBar(Rect box, float fraction, const Theme &theme, const char *id = "##progress");

// ---- rows -----------------------------------------------------------------

enum class StatusKind { Ok, Fail, Warn, Info, Pending, Done };

// Icon, title, an optional detail under it and an optional right-hand meta.
// Returns the height used, which is 44 with no detail.
float StatusRow(Rect box, StatusKind kind, const char *title, const char *detail,
                const char *meta, const Theme &theme);

// The installer's left rail: 28 px badges, green when done, action.bg for the
// current one, outlined when still to come.
void Stepper(ImVec2 pos, const char *const *steps, int count, int current,
             const Theme &theme);

// One console line: timestamp, a 9-character tag column, then the message.
struct ConsoleLine {
	std::string time;
	std::string tag;
	std::string text;
	ImU32       tagColour  = 0;
	ImU32       textColour = 0;
};

// Draws as many lines as fit, newest at the bottom, clipped to the box.
// Whatever is new since the last frame fades up from under the edge, so `id`
// has to be unique per console.
void Console(Rect box, const std::vector<ConsoleLine> &lines, const Theme &theme,
             bool scrollToBottom, const char *id = "##console");

// ---- dialog ---------------------------------------------------------------

// Dims everything behind, then returns the box the dialog should be drawn in.
// Call Card() on it yourself, so the dialog keeps its own padding. `t` is how
// far open it is, from Appear(); `topInset` keeps the scrim off the title bar,
// which is where the design draws its edge.
Rect DialogScrim(ImVec2 screenSize, ImVec2 dialogSize, const Theme &theme, float t = 1.0f,
                 float topInset = 0.0f);

// The same, as something that also arrives and leaves. Draw the dialog between
// the two calls:
//
//     const float t = Appear("##options", open);
//     if (t > 0.0f) {
//         Dialog dialog(screen, size, theme, t, kTitleBar);
//         ... Card(dialog.box, ...) and everything in it ...
//         dialog.End();
//     }
//
// The box is where the dialog ends up, not where it is mid-flight: clicks land
// correctly from the first frame while the pixels are still on their way.
// Because it holds one draw list, nothing between the two calls may open an
// ImGui child window.
//
// It opens an ImGui window of its own, which is what puts its controls in
// front of whatever is behind them - see the comment on the constructor. It
// does not make the screen behind inert, because that would take the title bar
// with it: wrap the body in ImGui::BeginDisabled() for that.
struct Dialog {
	Dialog(ImVec2 screenSize, ImVec2 dialogSize, const Theme &theme, float t,
	       float topInset = 0.0f);
	void End() const;

	Rect      box;
	float     t = 1.0f;
	DrawGroup group{nullptr};
};

// ---- images ---------------------------------------------------------------

// The CoopIII logo mark, drawn `height` px tall with its top-left at `pos`.
// Returns the width it took.
float Logo(ImVec2 pos, float height);
// Same, sized by width instead - the launcher's left column asks for 216 px.
float LogoWidth(ImVec2 pos, float width);
// Uploads the embedded logo. Called once by App, safe to call again.
void LoadImages(void *d3dDevice);

} // namespace ui
