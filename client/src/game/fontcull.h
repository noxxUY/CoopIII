// CFont::PrintChar drops a glyph whose top is at or past the screen WIDTH on
// the y axis (addresses.h, CFont__PrintChar). In a window taller than it is
// wide that is everything below y = width: nametags over anybody in the lower
// part of the screen, the chat line in a tall enough window, and the game's
// own subtitles and help text.
//
// fontcull.cpp fixes the compare itself, once, if the bytes are still retail.
// Where it can't, the layout code keeps text above the line instead, so this
// header also says where the line is.
#pragma once

#include "addresses.h"

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace coopiii::game {

// What PrintChar's cull block (0x00500C30..0x00500C95) holds right now.
enum class FontCullBytes : uint8_t {
	Retail,    // the four tests as shipped, y against the width
	Height,    // the same with the fourth test reading the height
	Foreign,   // anything else: another mod has been here
};

inline FontCullBytes ClassifyFontCull(const uint8_t *block) {
	constexpr size_t len = sizeof(PRINTCHAR_CULL_RETAIL);
	constexpr size_t at  = CFont__PrintChar_CullY - CFont__PrintChar;
	constexpr size_t fix = sizeof(PRINTCHAR_CULL_Y_HEIGHT);
	if (std::memcmp(block, PRINTCHAR_CULL_RETAIL, len) == 0)
		return FontCullBytes::Retail;
	if (std::memcmp(block, PRINTCHAR_CULL_RETAIL, at) == 0 &&
	    std::memcmp(block + at, PRINTCHAR_CULL_Y_HEIGHT, fix) == 0 &&
	    std::memcmp(block + at + fix, PRINTCHAR_CULL_RETAIL + at + fix, len - at - fix) == 0)
		return FontCullBytes::Height;
	return FontCullBytes::Foreign;
}

// First byte of the block that is not retail's, or the block's length.
inline size_t FirstForeignByte(const uint8_t *block) {
	size_t i = 0;
	while (i < sizeof(PRINTCHAR_CULL_RETAIL) && block[i] == PRINTCHAR_CULL_RETAIL[i])
		++i;
	return i;
}

// The lowest a glyph's top can be and still print. Fixed, that is the bottom
// of the screen. Not fixed, it is the width, or the bottom if that comes
// first.
inline float TextCullLine(float screenW, float screenH, bool fixed) {
	if (fixed || screenH < screenW)
		return screenH;
	return screenW;
}

constexpr float TEXT_CULL_CLEARANCE_PX = 2.0f;

// How far up text whose lowest glyph top is `lowestTop` has to go to be
// printed. Nothing for text that already prints, and nothing for text that is
// off the bottom of the screen anyway: only what the cull line would lose.
inline float LiftAboveCull(float lowestTop, float cullY, float screenH) {
	const float limit = cullY - TEXT_CULL_CLEARANCE_PX;
	if (!(lowestTop > limit) || !(lowestTop < screenH))
		return 0.0f;
	return lowestTop - limit;
}

// Game thread only, where no glyph is being printed while the bytes change.
// Tries once; every later call returns at once.
void FixFontCull();
// TextCullLine for this screen, fixed if FixFontCull managed it.
float CurrentTextCullLine(float screenW, float screenH);

} // namespace coopiii::game
