// The two families the design uses - design/DESIGN.md §2.
//
// Archivo for everything except data, Fragment Mono for data. Dear ImGui
// cannot use a variable font, so these are the static instances the spec asks
// for: Regular, Medium, SemiBold and Bold at width 100, plus ExtraBold at
// width 125 for headings and primary buttons.
//
// Dear ImGui 1.92 rasterises glyphs on demand, so one face covers every size:
// push the face and the size you want and the atlas catches up.
#pragma once

#include <imgui.h>

namespace ui {

struct Fonts {
	ImFont *regular  = nullptr;   // Archivo Regular, 400
	ImFont *medium   = nullptr;   // Archivo Medium, 500
	ImFont *semibold = nullptr;   // Archivo SemiBold, 600
	ImFont *bold     = nullptr;   // Archivo Bold, 700
	ImFont *heavy    = nullptr;   // Archivo Expanded ExtraBold, 800 at width 125
	ImFont *mono     = nullptr;   // Fragment Mono Regular

	// Loads all six into the current ImGui context's atlas. Call once, after
	// the context exists and before the first frame.
	static void Load();
	static const Fonts &Get();
};

// The type roles in design/DESIGN.md §2, so a screen says what a piece of text
// *is* rather than repeating a face and a size.
enum class Type {
	WindowHeading,   // 28/34, 800, width 125, -0.01em
	DialogHeading,   // 22/28, 800, width 125
	SectionTitle,    // 15/20, 600
	Body,            // 14/20
	BodySmall,       // 13/18
	FieldLabel,      // 13/18, 600
	Hint,            // 12/16
	MetaLabel,       // 12/16, 600 - the small label over a value
	ButtonPrimary,   // 17, 800, width 125
	ButtonSmall,     // 14, 600
	MonoSmall,       // 12, Fragment Mono
	Mono,            // 13, Fragment Mono
	MonoLarge,       // 16, Fragment Mono
	Console,         // 12.5/20, Fragment Mono
};

struct TypeStyle {
	ImFont *font;
	// The design's size, in CSS pixels. Layout and line heights use this.
	float   size;
	// What to hand Dear ImGui to get glyphs that size.
	//
	// The two do not agree about what a font size is. CSS scales the em box:
	// 14px means unitsPerEm maps to 14. Dear ImGui hands the size to
	// stbtt_ScaleForPixelHeight, which maps ascender minus descender instead.
	// Archivo's is 1.088 em and Fragment Mono's is 1.2, so pushing the
	// design's number straight in draws every screen about 8% small - which is
	// most visible where a paragraph wraps a word earlier than the design does
	// and everything below it moves up.
	float   pixelSize;
	float   lineHeight;
	float   letterSpacing;   // in px at `size`; only the headings use it
};

// (ascender - descender) / unitsPerEm, read out of the embedded fonts:
// Archivo is 878 and -210 over 1000, Fragment Mono 950 and -250.
constexpr float kArchivoEmScale = 1.088f;
constexpr float kMonoEmScale    = 1.200f;

// The size to hand Dear ImGui for a design size in a given family.
inline float PixelSizeFor(float cssSize, bool mono) {
	return cssSize * (mono ? kMonoEmScale : kArchivoEmScale);
}

TypeStyle StyleOf(Type role);

// Pushes a role for the duration of the scope.
class ScopedType {
public:
	explicit ScopedType(Type role);
	ScopedType(ImFont *font, float size);
	~ScopedType();

	ScopedType(const ScopedType &)            = delete;
	ScopedType &operator=(const ScopedType &) = delete;
};

} // namespace ui
