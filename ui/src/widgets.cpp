#include "ui/widgets.h"

#include "ui/anim.h"
#include "ui/app.h"

#include <imgui_internal.h>

#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <unordered_map>

namespace ui {
namespace {

ImDrawList *Draw() { return ImGui::GetWindowDrawList(); }

// Where the glyphs go inside a line box.
//
// The screens are CSS: a run of text sits in a box `lineHeight` tall with the
// glyphs centred in it (half-leading above and below). ImGui instead puts the
// top of the font's own box at the position given. Without this every heading
// and label lands a few pixels high of where the design draws it - 3 px on the
// 28/34 window heading, which is visible next to the screen it came from.
float LineTop(const TypeStyle &style) {
	const ImFontBaked *baked = style.font ? style.font->GetFontBaked(style.pixelSize) : nullptr;
	if (!baked)
		return 0.0f;
	const float fontHeight = baked->Ascent - baked->Descent;
	return (style.lineHeight - fontHeight) * 0.5f;
}

// Text with letter-spacing, which ImGui has no notion of. Only the two
// headings and the primary button ask for it, and only by a fraction of a
// pixel per glyph - but over a 28 px heading it adds up to a few pixels of
// width, and the design's widths are the thing being matched.
float DrawSpaced(ImVec2 pos, const TypeStyle &style, ImU32 colour, const char *text) {
	ImDrawList *dl = Draw();
	pos.y += LineTop(style);
	if (style.letterSpacing == 0.0f) {
		dl->AddText(style.font, style.pixelSize, pos, colour, text);
		return ImGui::CalcTextSize(text).x;
	}

	float       x   = pos.x;
	const char *p   = text;
	const char *end = text + std::strlen(text);
	while (p < end) {
		unsigned int c  = 0;
		const int    n  = ImTextCharFromUtf8(&c, p, end);
		if (n == 0)
			break;
		const char *next = p + n;
		char        one[8];
		std::memcpy(one, p, static_cast<size_t>(n));
		one[n] = '\0';

		dl->AddText(style.font, style.pixelSize, ImVec2(x, pos.y), colour, one);
		ImFontBaked *baked = style.font->GetFontBaked(style.pixelSize);
		const ImFontGlyph *glyph = baked ? baked->FindGlyph(static_cast<ImWchar>(c)) : nullptr;
		x += (glyph ? glyph->AdvanceX : style.size * 0.5f) + style.letterSpacing;
		p = next;
	}
	return x - pos.x;
}

float SpacedWidth(const TypeStyle &style, const char *text) {
	ImGui::PushFont(style.font, style.pixelSize);
	float w = ImGui::CalcTextSize(text).x;
	ImGui::PopFont();
	if (style.letterSpacing != 0.0f) {
		int glyphs = 0;
		for (const char *p = text; *p; ++p)
			if ((*p & 0xC0) != 0x80)
				++glyphs;
		w += style.letterSpacing * glyphs;
	}
	return w;
}

ImU32 StatusColour(StatusKind kind, const Theme &theme) {
	switch (kind) {
	case StatusKind::Ok:
	case StatusKind::Done:    return theme.statusOk;
	case StatusKind::Fail:    return theme.statusFail;
	case StatusKind::Warn:    return theme.statusWarn;
	case StatusKind::Info:    return theme.textSecondary;
	case StatusKind::Pending: return theme.textTertiary;
	}
	return theme.textSecondary;
}

Icon StatusIcon(StatusKind kind) {
	switch (kind) {
	case StatusKind::Ok:
	case StatusKind::Done:    return Icon::CircleCheck;
	case StatusKind::Fail:    return Icon::CircleX;
	case StatusKind::Warn:    return Icon::TriangleAlert;
	case StatusKind::Info:    return Icon::Info;
	case StatusKind::Pending: return Icon::Info;
	}
	return Icon::Info;
}

// A hit target at an absolute position, so the design's own coordinates can be
// used without fighting ImGui's cursor.
bool Hit(const char *id, Rect box, bool *hovered = nullptr, bool *held = nullptr) {
	ImGui::SetCursorScreenPos(box.pos);
	const bool pressed = ImGui::InvisibleButton(
	    id, ImVec2(ImMax(box.size.x, 1.0f), ImMax(box.size.y, 1.0f)));
	if (hovered)
		*hovered = ImGui::IsItemHovered();
	if (held)
		*held = ImGui::IsItemActive();
	return pressed;
}

// 0 to 1 every `seconds`, free-running. For the things that loop rather than
// respond: a pulse, a sheen, a spinner.
float Cycle(float seconds) {
	const float t = static_cast<float>(ImGui::GetTime());
	return seconds > 0.0f ? std::fmod(t, seconds) / seconds : 0.0f;
}

} // namespace

// ---- pointing at things ---------------------------------------------------

float   Touched::Press() const { return ImSaturate((t - 0.5f) * 2.0f); }
ImGuiID Touched::Also(unsigned n) const { return key ^ (0x9E3779B9u * (n + 1)); }

Touched Hotspot(const char *id, Rect box) {
	Touched f;
	// The id is taken before the hit test rather than after it, so a widget
	// drawn while disabled - which submits no button - still has somewhere to
	// keep what it looks like.
	f.key     = ImGui::GetID(id);
	f.clicked = Hit(id, box, &f.hovered, &f.held);
	f.t       = Pointer(f.key, f.hovered, f.held);
	return f;
}

// The design has no hover states written down, so everything uses the same
// restrained one: a slightly brighter surface, never a colour change. Hovered
// is 7% toward white (black in the light theme) and held 14% - the numbers
// this had before any of it moved.
ImU32 Lift(ImU32 base, const Theme &theme, float amount) {
	if (amount <= 0.0f)
		return base;
	return Mix(base, theme.isLight ? IM_COL32(0, 0, 0, 255) : IM_COL32(255, 255, 255, 255),
	           amount * 0.14f);
}

// ---- text -----------------------------------------------------------------

float Text(ImVec2 pos, Type role, ImU32 colour, const char *text) {
	return DrawSpaced(pos, StyleOf(role), colour, text);
}

float TextF(ImVec2 pos, Type role, ImU32 colour, const char *fmt, ...) {
	char    buf[512];
	va_list args;
	va_start(args, fmt);
	std::vsnprintf(buf, sizeof(buf), fmt, args);
	va_end(args);
	return Text(pos, role, colour, buf);
}

float TextRight(float rightEdge, float y, Type role, ImU32 colour, const char *text) {
	const TypeStyle style = StyleOf(role);
	const float     w     = SpacedWidth(style, text);
	DrawSpaced(ImVec2(rightEdge - w, y), style, colour, text);
	return w;
}

ImVec2 MeasureText(Type role, const char *text) {
	const TypeStyle style = StyleOf(role);
	return ImVec2(SpacedWidth(style, text), style.lineHeight);
}

ImVec2 MeasureWrapped(Type role, float width, const char *text) {
	const TypeStyle style = StyleOf(role);
	ImGui::PushFont(style.font, style.pixelSize);
	const ImVec2 size = ImGui::CalcTextSize(text, nullptr, false, width);
	ImGui::PopFont();
	// ImGui measures in line boxes of its own; the design gives a line height,
	// and that is what the rest of the layout is spaced against.
	const int lines = ImMax(1, static_cast<int>(size.y / ImGui::GetTextLineHeight() + 0.5f));
	return ImVec2(size.x, lines * style.lineHeight);
}

float TextWrapped(ImVec2 pos, float width, Type role, ImU32 colour, const char *text) {
	const TypeStyle style = StyleOf(role);
	ImGui::PushFont(style.font, style.pixelSize);
	// Draw line by line so the design's line height is what separates them,
	// not the font's own.
	const char *p     = text;
	const char *end   = text + std::strlen(text);
	const float top   = LineTop(style);
	float       y     = pos.y;
	while (p < end) {
		const char *stop = ImGui::GetFont()->CalcWordWrapPosition(style.pixelSize, p, end, width);
		if (stop == p)
			stop = p + 1;
		Draw()->AddText(style.font, style.pixelSize, ImVec2(pos.x, y + top), colour, p, stop);
		y += style.lineHeight;
		p = stop;
		while (p < end && (*p == ' ' || *p == '\n'))
			++p;
	}
	ImGui::PopFont();
	return y - pos.y;
}

float TextMiddle(ImVec2 pos, float height, Type role, ImU32 colour, const char *text) {
	TypeStyle style = StyleOf(role);
	// Centre the design's line box in the box given, then let DrawSpaced put
	// the glyphs inside that line box.
	const float y = pos.y + (height - style.lineHeight) * 0.5f;
	return DrawSpaced(ImVec2(pos.x, y), style, colour, text);
}

// ---- title bar ------------------------------------------------------------

TitleBarResult TitleBar(App &app, const char *name, const char *version, bool withMaximise,
                        bool withThemeSwitch) {
	const Theme &theme  = app.CurrentTheme();
	const float  height = 40.0f;
	const float  width  = ImGui::GetIO().DisplaySize.x;

	Draw()->AddRectFilled(ImVec2(0, 0), ImVec2(width, height), theme.bgTitlebar);
	Draw()->AddLine(ImVec2(0, height - 0.5f), ImVec2(width, height - 0.5f),
	                theme.borderTitlebar, 1.0f);

	// Left: the logo at 18 px, then the app's name.
	const float logoWidth = Logo(ImVec2(14.0f, (height - 18.0f) * 0.5f), 18.0f);
	TextMiddle(ImVec2(14.0f + logoWidth + 10.0f, 0.0f), height, Type::BodySmall,
	           theme.textSecondary, name);

	TitleBarResult result;

	// Right: the window buttons, 46 x 39, then the version to their left.
	const float buttonW = 46.0f, buttonH = 39.0f;
	int         buttons = withMaximise ? 3 : 2;
	float       x       = width - buttonW * buttons;

	auto windowButton = [&](const char *id, Icon icon) {
		const Rect box{ImVec2(x, 0.0f), ImVec2(buttonW, buttonH)};
		const Touched f = Hotspot(id, box);

		// Pointer() tops out at 0.5 for a hover; these fills are either there
		// or not, so hovering is the whole way in.
		const float on     = ImMin(1.0f, f.t * 2.0f);
		const bool  danger = std::strcmp(id, "##close") == 0;
		if (on > 0.0f)
			Draw()->AddRectFilled(box.pos, box.Max(),
			                      danger ? Rgb(0xC42B1C, on * (f.held ? 1.0f : 0.9f))
			                             : WithAlpha(Lift(theme.bgTitlebar, theme, 1.0f), on));

		DrawIcon(Draw(), icon, ImVec2(x + (buttonW - 16.0f) * 0.5f, (buttonH - 16.0f) * 0.5f),
		         16.0f, danger ? Mix(theme.textSecondary, Rgb(0xFFFFFF), on) : theme.textSecondary,
		         1.8f);
		x += buttonW;
		return f.clicked;
	};

	float rightOfContent = width - buttonW * buttons;

	if (withThemeSwitch) {
		const float switchW = 36.0f;
		rightOfContent -= switchW + 4.0f;
		const Rect box{ImVec2(rightOfContent, (height - 30.0f) * 0.5f), ImVec2(switchW, 30.0f)};
		const Touched f = Hotspot("##theme", box);
		if (f.clicked)
			result.theme = true;
		if (f.t > 0.0f)
			Draw()->AddRectFilled(box.pos, box.Max(),
			                      WithAlpha(Lift(theme.bgTitlebar, theme, 1.0f),
			                                ImMin(1.0f, f.t * 2.0f)),
			                      radius::kInput);

		// The two icons turn through each other on the same clock the window's
		// colours cross-fade on, so the switch and what it switches arrive
		// together.
		const float   mix = Animate(f.Also(1), app.IsLight() ? 1.0f : 0.0f, 11.0f);
		const ImVec2  at(box.pos.x + (switchW - 16.0f) * 0.5f, box.pos.y + 7.0f);
		if (mix < 1.0f)
			DrawIcon(Draw(), Icon::Sun, at, 16.0f, WithAlpha(theme.textSecondary, 1.0f - mix),
			         1.8f, mix * 1.4f);
		if (mix > 0.0f)
			DrawIcon(Draw(), Icon::Cloud, at, 16.0f, WithAlpha(theme.textSecondary, mix), 1.8f,
			         (mix - 1.0f) * 1.4f);
		ImGui::SetItemTooltip(app.IsLight() ? "Switch to the dark theme"
		                                    : "Switch to the light theme");
	}

	if (version && *version) {
		const float w = MeasureText(Type::Hint, version).x;
		TextMiddle(ImVec2(rightOfContent - w - 8.0f, 0.0f), height, Type::Hint,
		           theme.textTertiary, version);
		rightOfContent -= w + 8.0f;
	}

	result.minimise = windowButton("##min", Icon::Minimize);
	if (withMaximise)
		result.maximise = windowButton("##max", Icon::Maximize);
	result.close = windowButton("##close", Icon::Close);

	// Everything left of the buttons drags the window.
	app.SetDragRegion(height, rightOfContent);
	return result;
}

// ---- buttons --------------------------------------------------------------

bool PrimaryButton(const char *id, Rect box, const char *label, Icon icon, bool enabled,
                   const Theme &theme, Type role) {
	Touched f;
	if (enabled) {
		f = Hotspot(id, box);
	} else {
		// Still claim the space, so nothing behind it takes the click, and
		// still hold the id: a button that is about to come alive has to have
		// something to fade from.
		f.key = ImGui::GetID(id);
		f.t   = Pointer(f.key, false, false);
		ImGui::SetCursorScreenPos(box.pos);
		ImGui::Dummy(box.size);
	}

	// The launcher's start button spends its life going from grey to live and
	// back as the checks pass and fail, and that is worth seeing happen.
	const float on = Animate(f.Also(1), enabled ? 1.0f : 0.0f, 12.0f);

	const ImU32 fill = Mix(theme.bgDisabled, Lift(theme.actionBg, theme, f.t), on);
	const ImU32 text = Mix(theme.textMuted, theme.actionFg, on);

	// Pressing sinks it by a pixel and a half. The hit box does not move, so
	// the pointer never falls off the edge of what it is holding.
	const Rect drawn = box.Inset(f.Press() * 1.5f);
	Draw()->AddRectFilled(drawn.pos, drawn.Max(), fill, radius::kPrimary);
	if (on < 1.0f)
		Draw()->AddRect(drawn.pos, drawn.Max(), WithAlpha(theme.borderDisabled, 1.0f - on),
		                radius::kPrimary, 0, 1.0f);

	const TypeStyle style    = StyleOf(role);
	const bool      hasIcon  = icon != Icon::None;
	const float     iconSize = 18.0f;
	const float     gap      = 12.0f;
	const float     textW    = label ? SpacedWidth(style, label) : 0.0f;
	const float     total    = textW + (hasIcon ? iconSize + gap : 0.0f);
	float           x        = box.pos.x + (box.size.x - total) * 0.5f;
	const float     y        = box.pos.y + (box.size.y - style.lineHeight) * 0.5f;

	const DrawGroup content(Draw());
	if (hasIcon) {
		DrawIcon(Draw(), icon, ImVec2(x, box.pos.y + (box.size.y - iconSize) * 0.5f), iconSize,
		         text);
		x += iconSize + gap;
	}
	if (label)
		DrawSpaced(ImVec2(x, y), style, text, label);
	content.Scale(box.Centre(), 1.0f - f.Press() * 0.02f);

	if (enabled && f.hovered)
		ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
	return f.clicked;
}

bool OutlineButton(const char *id, Rect box, const char *label, Icon icon, const Theme &theme,
                   bool filled, Type role, ImU32 iconColour, float iconTurn) {
	const Touched f     = Hotspot(id, box);
	const Rect  drawn = box.Inset(f.Press() * 1.0f);
	const ImU32 base  = filled ? theme.bgSubtle : theme.bgWindow;

	if (filled)
		Draw()->AddRectFilled(drawn.pos, drawn.Max(), Lift(base, theme, f.t), radius::kInput);
	else if (f.t > 0.0f)
		Draw()->AddRectFilled(drawn.pos, drawn.Max(),
		                      WithAlpha(Lift(base, theme, f.t), ImMin(1.0f, f.t * 2.0f)),
		                      radius::kInput);
	// The border comes up with the pointer, which is the whole hover state on
	// a button the design draws as an outline and nothing else.
	Draw()->AddRect(drawn.pos, drawn.Max(),
	                Mix(theme.borderControl, theme.borderStrong, ImMin(1.0f, f.t * 2.0f)),
	                radius::kInput, 0, 1.0f);

	const TypeStyle style    = StyleOf(role);
	const bool      hasIcon  = icon != Icon::None;
	const float     iconSize = 16.0f;
	const float     gap      = 8.0f;
	const float     textW    = label ? SpacedWidth(style, label) : 0.0f;
	const float     total    = textW + (hasIcon ? iconSize + (label ? gap : 0.0f) : 0.0f);
	float           x        = box.pos.x + (box.size.x - total) * 0.5f;
	const float     y        = box.pos.y + (box.size.y - style.lineHeight) * 0.5f;

	const DrawGroup content(Draw());
	if (hasIcon) {
		DrawIcon(Draw(), icon, ImVec2(x, box.pos.y + (box.size.y - iconSize) * 0.5f), iconSize,
		         iconColour ? iconColour : theme.textButton, 2.0f, iconTurn);
		x += iconSize + gap;
	}
	if (label)
		DrawSpaced(ImVec2(x, y), style, theme.textButton, label);
	content.Scale(box.Centre(), 1.0f - f.Press() * 0.03f);

	if (f.hovered)
		ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
	return f.clicked;
}

bool IconButton(const char *id, Rect box, Icon icon, const Theme &theme, ImU32 colour,
                float iconSize, bool bordered, ImU32 fill, ImU32 borderColour) {
	const Touched f = Hotspot(id, box);

	if (fill)
		Draw()->AddRectFilled(box.pos, box.Max(), Lift(fill, theme, f.t), radius::kInput);
	else if (f.t > 0.0f)
		Draw()->AddRectFilled(box.pos, box.Max(),
		                      WithAlpha(Lift(theme.bgWindow, theme, f.t), ImMin(1.0f, f.t * 2.0f)),
		                      radius::kInput);
	if (bordered)
		Draw()->AddRect(box.pos, box.Max(),
		                Mix(borderColour ? borderColour : theme.borderControl, theme.borderStrong,
		                    ImMin(1.0f, f.t * 2.0f)),
		                radius::kInput, 0, 1.0f);

	const DrawGroup glyph(Draw());
	DrawIcon(Draw(), icon,
	         ImVec2(box.pos.x + (box.size.x - iconSize) * 0.5f,
	                box.pos.y + (box.size.y - iconSize) * 0.5f),
	         iconSize, colour);
	glyph.Scale(box.Centre(), 1.0f - f.Press() * 0.10f);

	if (f.hovered)
		ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
	return f.clicked;
}

bool LinkText(const char *id, ImVec2 pos, Type role, ImU32 colour, const char *label) {
	const ImVec2 size = MeasureText(role, label);
	const Rect   box{pos, size};
	const Touched f = Hotspot(id, box);
	Text(pos, role, colour, label);

	// The underline is drawn from the left rather than faded in: it reads as
	// the pointer running along the word.
	const float on = ImMin(1.0f, f.t * 2.0f);
	if (on > 0.0f) {
		const float y = pos.y + size.y - 2.0f;
		Draw()->AddLine(ImVec2(pos.x, y), ImVec2(pos.x + size.x * EaseOut(on), y), colour, 1.0f);
		ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
	}
	return f.clicked;
}

// ---- input ----------------------------------------------------------------

bool TextInput(const char *id, Rect box, char *buffer, size_t bufferSize, const Theme &theme,
               const char *placeholder, ImGuiInputTextFlags flags) {
	Draw()->AddRectFilled(box.pos, box.Max(), theme.bgWindow, radius::kInput);

	ImGui::SetCursorScreenPos(ImVec2(box.pos.x + 14.0f, box.pos.y));
	ImGui::PushItemWidth(box.size.x - 28.0f);
	ImGui::PushStyleColor(ImGuiCol_FrameBg, IM_COL32(0, 0, 0, 0));
	ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, IM_COL32(0, 0, 0, 0));
	ImGui::PushStyleColor(ImGuiCol_FrameBgActive, IM_COL32(0, 0, 0, 0));
	ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(theme.textPrimary));
	ImGui::PushStyleColor(ImGuiCol_TextSelectedBg,
	                      ImGui::ColorConvertU32ToFloat4(WithAlpha(theme.actionBg, 0.30f)));
	ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,
	                    ImVec2(0.0f, (box.size.y - 15.0f) * 0.5f - 1.0f));
	ImGui::PushFont(Fonts::Get().regular, PixelSizeFor(15.0f, false));

	const bool changed = ImGui::InputTextWithHint(id, placeholder ? placeholder : "", buffer,
	                                              bufferSize, flags);
	const bool active = ImGui::IsItemActive();

	ImGui::PopFont();
	ImGui::PopStyleVar();
	ImGui::PopStyleColor(5);
	ImGui::PopItemWidth();

	// Focus arrives rather than snapping, and brings a one-pixel halo with it
	// so a field being typed into is obvious without a colour the design
	// does not have.
	const float focus = Animate(MotionKey(id), active ? 1.0f : 0.0f, 18.0f);
	if (focus > 0.0f)
		Draw()->AddRect(ImVec2(box.pos.x - 1.0f, box.pos.y - 1.0f),
		                ImVec2(box.Max().x + 1.0f, box.Max().y + 1.0f),
		                WithAlpha(theme.actionBg, focus * 0.22f), radius::kInput + 1.0f, 0, 2.0f);
	Draw()->AddRect(box.pos, box.Max(), Mix(theme.borderControl, theme.actionBg, focus),
	                radius::kInput, 0, 1.0f);
	return changed;
}

FieldResult Field(const char *id, ImVec2 pos, float width, const char *label,
                  const char *rightNote, char *buffer, size_t bufferSize, const Theme &theme,
                  float inputHeight, ImGuiInputTextFlags flags) {
	FieldResult result;
	if (label)
		Text(pos, Type::FieldLabel, theme.textSecondary, label);
	if (rightNote)
		TextRight(pos.x + width, pos.y, Type::Hint, theme.textTertiary, rightNote);

	// 8 px between the label and the field - the launcher's form spacing.
	result.inputBox = Rect{ImVec2(pos.x, pos.y + (label ? 18.0f + 8.0f : 0.0f)),
	                       ImVec2(width, inputHeight)};
	result.changed  = TextInput(id, result.inputBox, buffer, bufferSize, theme, nullptr, flags);
	return result;
}

bool Checkbox(const char *id, ImVec2 pos, bool *value, const char *label, const Theme &theme) {
	const float side = 18.0f;
	const ImVec2 labelSize = label ? MeasureText(Type::Body, label) : ImVec2(0, 0);
	const Rect   box{pos, ImVec2(side + (label ? 10.0f + labelSize.x : 0.0f), side)};

	const Touched f = Hotspot(id, box);
	if (f.clicked)
		*value = !*value;

	const float on   = Animate(f.Also(1), *value ? 1.0f : 0.0f, 20.0f);
	const Rect  mark{pos, ImVec2(side, side)};

	if (on < 1.0f)
		Draw()->AddRect(mark.pos, mark.Max(),
		                WithAlpha(Mix(theme.borderControl, theme.borderStrong,
		                              ImMin(1.0f, f.t * 2.0f)),
		                          1.0f - on),
		                4.0f, 0, 1.5f);
	if (on > 0.0f) {
		// The box fills from its middle and the tick lands just past full
		// size, which is the whole difference between a checkbox and a
		// checkbox worth clicking twice.
		const DrawGroup ticked(Draw());
		Draw()->AddRectFilled(mark.pos, mark.Max(), Lift(theme.actionBg, theme, f.t * 0.5f), 4.0f);
		DrawIcon(Draw(), Icon::Check, ImVec2(pos.x + 2.0f, pos.y + 2.0f), side - 4.0f,
		         theme.actionFg, 2.6f);
		ticked.Fade(on);
		ticked.Scale(mark.Centre(), 0.55f + 0.45f * EaseBack(on));
	}

	if (label)
		Text(ImVec2(pos.x + side + 10.0f, pos.y + (side - 14.0f) * 0.5f - 1.0f), Type::Body,
		     theme.textPrimary, label);
	if (f.hovered)
		ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
	return f.clicked;
}

bool Switch(const char *id, ImVec2 pos, bool *value, const Theme &theme) {
	const Rect box{pos, ImVec2(48.0f, 28.0f)};
	const Touched f = Hotspot(id, box);
	if (f.clicked)
		*value = !*value;

	const float on = Animate(f.Also(1), *value ? 1.0f : 0.0f, 18.0f);
	const float r  = box.size.y * 0.5f;

	Draw()->AddRectFilled(box.pos, box.Max(),
	                      Mix(Lift(theme.bgSubtle, theme, f.t), theme.actionBg, on), r);
	if (on < 1.0f)
		Draw()->AddRect(box.pos, box.Max(), WithAlpha(theme.borderControl, 1.0f - on), r, 0, 1.0f);

	// The knob stretches as it travels and settles back into a circle, the
	// way a physical switch has some give in it.
	const float knobR   = r - 4.0f;
	const float travel  = EaseInOut(on);
	const float cx      = box.pos.x + r + (box.size.x - r * 2.0f) * travel;
	const float stretch = std::sin(travel * 3.14159265f) * 3.0f;
	const ImU32 knob    = Mix(theme.textTertiary, theme.actionFg, on);
	Draw()->AddRectFilled(ImVec2(cx - knobR - stretch, box.pos.y + r - knobR),
	                      ImVec2(cx + knobR + stretch, box.pos.y + r + knobR), knob, knobR);

	if (f.hovered)
		ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
	return f.clicked;
}

bool Segmented(const char *id, Rect box, const char *const *options, int count, int *selected,
               const Theme &theme) {
	Draw()->AddRectFilled(box.pos, box.Max(), theme.bgWindow, radius::kInput);

	const float inset   = 3.0f;
	const float innerW  = box.size.x - inset * 2.0f;
	const float segW    = innerW / static_cast<float>(count);
	const float segH    = box.size.y - inset * 2.0f;
	bool        changed = false;

	// The selection slides to where it was sent. Drawn first, so the labels
	// sit on top of it.
	const float where = Animate(MotionKey(id), static_cast<float>(*selected), 22.0f);
	Draw()->AddRectFilled(ImVec2(box.pos.x + inset + segW * where, box.pos.y + inset),
	                      ImVec2(box.pos.x + inset + segW * (where + 1.0f), box.pos.y + inset + segH),
	                      theme.bgSelected, radius::kInput - 1.0f);

	for (int i = 0; i < count; ++i) {
		const Rect seg{ImVec2(box.pos.x + inset + segW * i, box.pos.y + inset),
		               ImVec2(segW, segH)};
		char       segId[64];
		std::snprintf(segId, sizeof(segId), "%s##seg%d", id, i);

		const Touched f = Hotspot(segId, seg);
		if (f.clicked && *selected != i) {
			*selected = i;
			changed   = true;
		}

		// How much of the selection is over this segment, which is also how
		// far its label has come up to textPrimary.
		const float here = ImSaturate(1.0f - std::fabs(where - static_cast<float>(i)));
		if (here < 0.5f && f.t > 0.0f)
			Draw()->AddRectFilled(seg.pos, seg.Max(),
			                      WithAlpha(Lift(theme.bgWindow, theme, f.t),
			                                ImMin(1.0f, f.t * 2.0f) * (1.0f - here)),
			                      radius::kInput - 1.0f);

		const TypeStyle style = StyleOf(Type::ButtonSmall);
		const float     w     = SpacedWidth(style, options[i]);
		DrawSpaced(ImVec2(seg.pos.x + (segW - w) * 0.5f,
		                  seg.pos.y + (segH - style.lineHeight) * 0.5f),
		           style, Mix(theme.textSecondary, theme.textPrimary, here), options[i]);
		if (f.hovered)
			ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
	}
	return changed;
}

float SegmentedFlat(const char *id, float rightEdge, float centreY,
                    const char *const *options, int count, int *selected, const Theme &theme,
                    bool *changed) {
	// 13/18 at 600, the weight the design gives these.
	const TypeStyle style = StyleOf(Type::FieldLabel);
	const float     height = 32.0f;
	const float     gap    = 4.0f;
	const float     padX   = 12.0f;

	float total = 0.0f;
	for (int i = 0; i < count; ++i)
		total += SpacedWidth(style, options[i]) + padX * 2.0f + (i ? gap : 0.0f);

	const float y = centreY - height * 0.5f;
	if (changed)
		*changed = false;

	// These segments are each as wide as their own word, so the selection
	// cannot just slide along a fixed step: it grows and shrinks on the way.
	float xs[8], ws[8];
	float x = rightEdge - total;
	for (int i = 0; i < count && i < 8; ++i) {
		ws[i] = SpacedWidth(style, options[i]) + padX * 2.0f;
		xs[i] = x;
		x += ws[i] + gap;
	}

	const float where = Animate(MotionKey(id), static_cast<float>(*selected), 22.0f);
	const int   from  = ImClamp(static_cast<int>(where), 0, count - 1);
	const int   to    = ImClamp(from + 1, 0, count - 1);
	const float part  = where - static_cast<float>(from);
	const float selX  = ImLerp(xs[from], xs[to], part);
	const float selW  = ImLerp(ws[from], ws[to], part);
	Draw()->AddRectFilled(ImVec2(selX, y), ImVec2(selX + selW, y + height), theme.bgSelected,
	                      radius::kInput);

	for (int i = 0; i < count && i < 8; ++i) {
		const Rect seg{ImVec2(xs[i], y), ImVec2(ws[i], height)};

		char segId[64];
		std::snprintf(segId, sizeof(segId), "%s##flat%d", id, i);
		const Touched f = Hotspot(segId, seg);
		if (f.clicked && *selected != i) {
			*selected = i;
			if (changed)
				*changed = true;
		}

		const float here = ImSaturate(1.0f - std::fabs(where - static_cast<float>(i)));
		if (here < 0.5f && f.t > 0.0f)
			Draw()->AddRectFilled(seg.pos, seg.Max(),
			                      WithAlpha(Lift(theme.bgConsole, theme, f.t),
			                                ImMin(1.0f, f.t * 2.0f) * (1.0f - here)),
			                      radius::kInput);

		DrawSpaced(ImVec2(xs[i] + padX, y + (height - style.lineHeight) * 0.5f), style,
		           Mix(theme.textSecondary, theme.actionBg, here), options[i]);
		if (f.hovered)
			ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
	}
	return total;
}

// ---- surfaces -------------------------------------------------------------

void Card(Rect box, const Theme &theme, float rounding, ImU32 fill, ImU32 border) {
	Draw()->AddRectFilled(box.pos, box.Max(), fill ? fill : theme.bgCard, rounding);
	Draw()->AddRect(box.pos, box.Max(), border ? border : theme.border, rounding, 0, 1.0f);
}

void Chip(ImVec2 pos, const char *label, const Theme &theme) {
	const TypeStyle style = StyleOf(Type::Hint);
	const float     w     = SpacedWidth(style, label) + 18.0f;
	const float     h     = 22.0f;
	Draw()->AddRect(pos, ImVec2(pos.x + w, pos.y + h), theme.borderStrong, h * 0.5f, 0, 1.0f);
	DrawSpaced(ImVec2(pos.x + 9.0f, pos.y + (h - style.lineHeight) * 0.5f), style,
	           theme.textSecondary, label);
}

float Pill(ImVec2 pos, const char *label, ImU32 colour, ImU32 fill, ImU32 border, bool pulse) {
	const TypeStyle style = StyleOf(Type::BodySmall);
	const float     textW = SpacedWidth(style, label);
	const float     h     = 26.0f;
	const float     w     = 10.0f + 8.0f + 8.0f + textW + 11.0f;

	Draw()->AddRectFilled(pos, ImVec2(pos.x + w, pos.y + h), fill, h * 0.5f);
	Draw()->AddRect(pos, ImVec2(pos.x + w, pos.y + h), border, h * 0.5f, 0, 1.0f);

	// A halo off the dot, the same idea as the city map's player halos in
	// DESIGN §5 and at about the same speed. It is the only thing in the
	// server's header saying the process is alive rather than hung.
	const ImVec2 dot(pos.x + 10.0f + 4.0f, pos.y + h * 0.5f);
	if (pulse && Ambient()) {
		const float cycle = Cycle(2.6f);
		Draw()->AddCircleFilled(dot, 4.0f + 7.0f * cycle,
		                        WithAlpha(colour, 0.28f * (1.0f - cycle)), 20);
	}
	Draw()->AddCircleFilled(dot, 4.0f, colour, 16);

	DrawSpaced(ImVec2(pos.x + 10.0f + 8.0f + 8.0f, pos.y + (h - style.lineHeight) * 0.5f),
	           style, colour, label);
	return w;
}

void ProgressBar(Rect box, float fraction, const Theme &theme, const char *id) {
	const float r = box.size.y * 0.5f;
	Draw()->AddRectFilled(box.pos, box.Max(), theme.bgSubtle, r);

	// The install finishes a component at a time, so without this the bar
	// jumps in twelfths. Eased, it reads as progress rather than as steps.
	const float shown = Animate(MotionKey(id), ImClamp(fraction, 0.0f, 1.0f), 9.0f);
	const float w     = box.size.x * shown;
	if (w <= 0.5f)
		return;

	const float right = box.pos.x + ImMax(w, box.size.y);
	Draw()->AddRectFilled(box.pos, ImVec2(right, box.Max().y), theme.actionBg, r);

	// A sheen crossing the filled part while there is still work left: the
	// difference between a bar that is waiting on something and one that has
	// stopped.
	if (shown < 0.999f && Ambient()) {
		const float at    = box.pos.x - 40.0f + (right - box.pos.x + 80.0f) * Cycle(1.8f);
		const ImU32 clear = WithAlpha(theme.bgWindow, 0.0f);
		const ImU32 lit   = WithAlpha(theme.bgWindow, 0.34f);
		Draw()->PushClipRect(box.pos, ImVec2(right, box.Max().y), true);
		Draw()->AddRectFilledMultiColor(ImVec2(at - 34.0f, box.pos.y), ImVec2(at, box.Max().y),
		                                clear, lit, lit, clear);
		Draw()->AddRectFilledMultiColor(ImVec2(at, box.pos.y), ImVec2(at + 34.0f, box.Max().y),
		                                lit, clear, clear, lit);
		Draw()->PopClipRect();
	}
}

// ---- rows -----------------------------------------------------------------

float StatusRow(Rect box, StatusKind kind, const char *title, const char *detail,
                const char *meta, const Theme &theme) {
	const ImU32 colour = StatusColour(kind, theme);
	const float iconSize = 18.0f;
	const float left     = box.pos.x + 14.0f;
	const float textX    = left + iconSize + 12.0f;

	const float height = detail ? 63.0f : 44.0f;
	const float iconY  = box.pos.y + (detail ? 14.0f : (height - iconSize) * 0.5f);
	DrawIcon(Draw(), StatusIcon(kind), ImVec2(left, iconY), iconSize, colour);

	float metaW = 0.0f;
	if (meta) {
		metaW = MeasureText(Type::Hint, meta).x;
		TextRight(box.Max().x - 14.0f, box.pos.y + (height - 16.0f) * 0.5f, Type::Hint,
		          theme.textTertiary, meta);
	}

	if (detail) {
		Text(ImVec2(textX, box.pos.y + 11.0f), Type::Body, theme.textPrimary, title);
		TextWrapped(ImVec2(textX, box.pos.y + 31.0f), box.size.x - (textX - box.pos.x) - 14.0f,
		            Type::BodySmall, colour, detail);
	} else {
		TextMiddle(ImVec2(textX, box.pos.y), height, Type::Body, theme.textPrimary, title);
	}
	return height;
}

void Stepper(ImVec2 pos, const char *const *steps, int count, int current, const Theme &theme) {
	const float   badge = 28.0f;
	const float   gap   = 34.0f;
	const ImGuiID key   = MotionKey("##stepper");
	auto          also  = [key](unsigned n) { return key ^ (0x9E3779B9u * (n + 1)); };

	for (int i = 0; i < count; ++i) {
		const float y = pos.y + i * (badge + gap);
		const Rect  b{ImVec2(pos.x, y), ImVec2(badge, badge)};

		// Two numbers per step rather than two booleans, so a step going from
		// pending to current to done crosses rather than cuts.
		const float done    = Animate(also(i * 2u), i < current ? 1.0f : 0.0f, 12.0f);
		const float present = Animate(also(i * 2u + 1u), i == current ? 1.0f : 0.0f, 12.0f);

		const TypeStyle style = StyleOf(Type::FieldLabel);
		char            n[8];
		std::snprintf(n, sizeof(n), "%d", i + 1);
		const float w = SpacedWidth(style, n);

		if (done < 1.0f) {
			Draw()->AddRectFilled(b.pos, b.Max(), WithAlpha(theme.actionBg, present),
			                      radius::kInput);
			if (present < 1.0f)
				Draw()->AddRect(b.pos, b.Max(),
				                WithAlpha(theme.borderControl, (1.0f - present) * (1.0f - done)),
				                radius::kInput, 0, 1.0f);
			DrawSpaced(ImVec2(b.pos.x + (badge - w) * 0.5f,
			                  b.pos.y + (badge - style.lineHeight) * 0.5f),
			           style, WithAlpha(Mix(theme.textTertiary, theme.actionFg, present),
			                            1.0f - done),
			           n);
		}
		if (done > 0.0f) {
			const DrawGroup tick(Draw());
			Draw()->AddRectFilled(b.pos, b.Max(), theme.statusOk, radius::kInput);
			DrawIcon(Draw(), Icon::Check, ImVec2(b.pos.x + 5.0f, b.pos.y + 5.0f), badge - 10.0f,
			         theme.isLight ? Rgb(0xFFFFFF) : theme.bgCard, 2.6f);
			tick.Fade(done);
			tick.Scale(b.Centre(), 0.7f + 0.3f * EaseBack(done));
		}

		TextMiddle(ImVec2(pos.x + badge + 14.0f, y), badge, Type::Body,
		           Mix(Mix(theme.textTertiary, theme.textSecondary, done), theme.textPrimary,
		               present),
		           steps[i]);

		// The connector fills downward as the step above is finished.
		if (i + 1 < count) {
			const ImVec2 top(pos.x + badge * 0.5f, y + badge + 6.0f);
			const ImVec2 end(pos.x + badge * 0.5f, y + badge + gap - 6.0f);
			Draw()->AddLine(top, end, theme.border, 2.0f);
			if (done > 0.0f)
				Draw()->AddLine(top, ImVec2(end.x, top.y + (end.y - top.y) * done),
				                theme.statusOk, 2.0f);
		}
	}
}

void Console(Rect box, const std::vector<ConsoleLine> &lines, const Theme &theme,
             bool scrollToBottom, const char *id) {
	const TypeStyle style = StyleOf(Type::Console);

	// What arrived since last frame, so it can be faded up from under the
	// bottom edge instead of simply being there. The count has to be kept
	// here: the vector is rebuilt every frame from whatever the filter lets
	// through, and it has no memory of its own.
	struct Arrivals {
		size_t count = 0;
		int    fresh = 0;
	};
	static std::unordered_map<ImGuiID, Arrivals> arrivals;

	const ImGuiID key  = MotionKey(id);
	Arrivals     &seen = arrivals[key];
	if (lines.size() > seen.count) {
		seen.fresh = static_cast<int>(lines.size() - seen.count);
		Restart(key);
	}
	if (lines.size() != seen.count) {
		if (lines.size() < seen.count)
			seen.fresh = 0;
		seen.count = lines.size();
	}
	const float arrival = EaseOut(Since(key) / 0.3f);
	const int   firstNew =
	    arrival < 1.0f ? static_cast<int>(lines.size()) - seen.fresh : static_cast<int>(lines.size());

	ImGui::SetCursorScreenPos(box.pos);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16.0f, 12.0f));
	ImGui::PushStyleColor(ImGuiCol_ChildBg, IM_COL32(0, 0, 0, 0));
	ImGui::PushStyleColor(ImGuiCol_ScrollbarBg, IM_COL32(0, 0, 0, 0));
	ImGui::PushStyleColor(ImGuiCol_ScrollbarGrab,
	                      ImGui::ColorConvertU32ToFloat4(theme.borderControl));
	ImGui::PushStyleColor(ImGuiCol_ScrollbarGrabHovered,
	                      ImGui::ColorConvertU32ToFloat4(theme.borderStrong));
	// AlwaysUseWindowPadding: a child with no border gets no padding by
	// default, and the design's console has 16 px of it.
	ImGui::BeginChild(id, box.size, ImGuiChildFlags_AlwaysUseWindowPadding,
	                  ImGuiWindowFlags_NoSavedSettings);

	// A 9-character tag column, the way the design sets it.
	ImGui::PushFont(style.font, style.pixelSize);
	const float tagWidth = ImGui::CalcTextSize("123456789").x;
	const float timeWidth = ImGui::CalcTextSize("00:00:00").x;
	ImGui::PopFont();

	const float originX = ImGui::GetCursorScreenPos().x;
	float       y       = ImGui::GetCursorScreenPos().y;

	for (size_t i = 0; i < lines.size(); ++i) {
		const ConsoleLine &line  = lines[i];
		const bool         fresh = static_cast<int>(i) >= firstNew;

		const DrawGroup arriving(Draw());
		float           x = originX;
		Draw()->AddText(style.font, style.pixelSize, ImVec2(x, y), theme.textMuted,
		                line.time.c_str());
		x += timeWidth + 12.0f;
		Draw()->AddText(style.font, style.pixelSize, ImVec2(x, y),
		                line.tagColour ? line.tagColour : theme.textTertiary, line.tag.c_str());
		x += tagWidth + 12.0f;
		Draw()->AddText(style.font, style.pixelSize, ImVec2(x, y),
		                line.textColour ? line.textColour : theme.textSecondary,
		                line.text.c_str());
		if (fresh) {
			arriving.Fade(arrival);
			arriving.Move(ImVec2(0.0f, (1.0f - arrival) * style.lineHeight * 0.7f * Travel()));
		}
		y += style.lineHeight;
	}

	// Claim the height so the child scrolls.
	ImGui::Dummy(ImVec2(1.0f, lines.size() * style.lineHeight));
	if (scrollToBottom)
		ImGui::SetScrollHereY(1.0f);

	ImGui::EndChild();
	ImGui::PopStyleColor(4);
	ImGui::PopStyleVar();
}

// ---- dialog ---------------------------------------------------------------

Rect DialogScrim(ImVec2 screenSize, ImVec2 dialogSize, const Theme &theme, float t,
                 float topInset) {
	Draw()->AddRectFilled(ImVec2(0, topInset), screenSize, WithAlpha(theme.scrim, ImSaturate(t)));
	return Rect{ImVec2((screenSize.x - dialogSize.x) * 0.5f,
	                   topInset + (screenSize.y - topInset - dialogSize.y) * 0.5f),
	            dialogSize};
}

Dialog::Dialog(ImVec2 screenSize, ImVec2 dialogSize, const Theme &theme, float t, float topInset)
    : t(ImSaturate(t)) {
	// The scrim belongs to the screen behind, so it goes in that window's draw
	// list and renders under everything below.
	box = DialogScrim(screenSize, dialogSize, theme, this->t, topInset);

	// The dialog then gets an ImGui window of its own. Not for a frame or a
	// background - it draws both itself - but because ImGui works out what the
	// mouse is over one window at a time, and it stops at the first window
	// under the pointer (imgui.cpp, ItemHoverable: `if (g.HoveredWindow !=
	// window) return false`). The server's console is an ImGui child window
	// sitting under the right-hand half of the options dialog, so without a
	// window of its own every control over that half - Save and Cancel
	// included - is drawn and never reachable.
	ImGui::SetNextWindowPos(box.pos);
	ImGui::SetNextWindowSize(box.size);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
	ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
	ImGui::Begin("##dialog", nullptr,
	             ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
	                 ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
	                 ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoSavedSettings |
	                 ImGuiWindowFlags_NoBackground);
	// The screens behind are all drawn in a window with NoBringToFrontOnFocus,
	// so this one is above them; the focus is for the first frame, before any
	// of that has been worked out.
	if (ImGui::IsWindowAppearing())
		ImGui::SetWindowFocus();

	group = DrawGroup(ImGui::GetWindowDrawList());
}

void Dialog::End() const {
	// Hit testing already happened at the dialog's resting place, so a click
	// on the first frame lands where the eye expects it to by the second.
	group.Fade(t);
	group.Move(ImVec2(0.0f, (1.0f - EaseOut(t)) * 16.0f * Travel()));
	group.Scale(box.Centre(), 1.0f - (1.0f - EaseOut(t)) * 0.025f * Travel());

	ImGui::End();
	ImGui::PopStyleVar(2);
}

} // namespace ui
