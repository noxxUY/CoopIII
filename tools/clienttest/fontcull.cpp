// CFont::PrintChar's y test: which bytes game/fontcull.cpp will rewrite and
// which it will leave, where text is laid out either way, and - when a copy
// of the retail exe is handed over - the bytes themselves.

#include "game/fontcull.h"
#include "game/nametag.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace coopiii;
using namespace coopiii::game;

namespace {

int g_cullFailures = 0;

void Check(bool cond, const char *what) {
	std::printf("  [%s] %s\n", cond ? "ok" : "FAIL", what);
	if (!cond)
		++g_cullFailures;
}

constexpr size_t BLOCK = sizeof(PRINTCHAR_CULL_RETAIL);
constexpr size_t AT    = CFont__PrintChar_CullY - CFont__PrintChar;
constexpr size_t FIX   = sizeof(PRINTCHAR_CULL_Y_HEIGHT);

std::vector<uint8_t> Retail() {
	return std::vector<uint8_t>(PRINTCHAR_CULL_RETAIL, PRINTCHAR_CULL_RETAIL + BLOCK);
}

std::vector<uint8_t> Fixed() {
	std::vector<uint8_t> b = Retail();
	std::memcpy(&b[AT], PRINTCHAR_CULL_Y_HEIGHT, FIX);
	return b;
}

void TestWhatGetsRewritten() {
	std::printf("\nwhat gets rewritten\n");
	Check(ClassifyFontCull(Retail().data()) == FontCullBytes::Retail, "retail's block is retail");
	Check(ClassifyFontCull(Fixed().data()) == FontCullBytes::Height,
	      "with the eight bytes in, it reads as already done, so a second go does nothing");

	Check(AT == 0x4F && PRINTCHAR_CULL_RETAIL[AT] == 0x89 && PRINTCHAR_CULL_RETAIL[AT + 4] == 0xDB &&
	          PRINTCHAR_CULL_RETAIL[AT + 8] == 0xD8 && PRINTCHAR_CULL_RETAIL[AT + 11] == 0x54,
	      "the eight bytes are the store and the fild, right before the fcomp of y");
	Check(PRINTCHAR_CULL_Y_HEIGHT[0] == 0xDB && PRINTCHAR_CULL_Y_HEIGHT[1] == 0x05 &&
	          PRINTCHAR_CULL_Y_HEIGHT[6] == 0x66 && PRINTCHAR_CULL_Y_HEIGHT[7] == 0x90,
	      "and become a fild of an absolute dword and a two-byte nop");

	// Somebody else's hand on it, anywhere in the block.
	std::vector<uint8_t> entry = Retail();
	entry[0]                   = 0xE9;
	Check(ClassifyFontCull(entry.data()) == FontCullBytes::Foreign && FirstForeignByte(entry.data()) == 0,
	      "a detour on PrintChar's entry is left alone");
	std::vector<uint8_t> x = Retail();
	x[0x16]                = 0xD9;
	Check(ClassifyFontCull(x.data()) == FontCullBytes::Foreign && FirstForeignByte(x.data()) == 0x16,
	      "so is a change to the x test, and the log says where");
	std::vector<uint8_t> other = Retail();
	other[AT + 7]              = 0x11;
	Check(ClassifyFontCull(other.data()) == FontCullBytes::Foreign,
	      "and a different edit of the same eight bytes");
	std::vector<uint8_t> half = Fixed();
	half[BLOCK - 1] ^= 0x01;
	Check(ClassifyFontCull(half.data()) == FontCullBytes::Foreign,
	      "ours with something else changed after it is not ours");
	Check(FirstForeignByte(Retail().data()) == BLOCK, "and retail has no first foreign byte");
}

void TestWhereTheLineIs() {
	std::printf("\nwhere the line is\n");
	Check(TextCullLine(958.0f, 1000.0f, false) == 958.0f, "retail, a tall window stops at the width");
	Check(TextCullLine(958.0f, 1000.0f, true) == 1000.0f, "fixed, at the bottom");
	Check(TextCullLine(1920.0f, 1080.0f, false) == 1080.0f &&
	          TextCullLine(1920.0f, 1080.0f, true) == 1080.0f,
	      "and a wide one at the bottom either way");

	Check(LiftAboveCull(500.0f, 958.0f, 1000.0f) == 0.0f, "text above the line stays put");
	Check(LiftAboveCull(970.0f, 958.0f, 1000.0f) == 970.0f - (958.0f - TEXT_CULL_CLEARANCE_PX),
	      "text between the line and the bottom goes up to just above it");
	Check(LiftAboveCull(1000.0f, 958.0f, 1000.0f) == 0.0f &&
	          LiftAboveCull(1400.0f, 958.0f, 1000.0f) == 0.0f,
	      "text off the bottom is not dragged back on");
	Check(LiftAboveCull(970.0f, 1000.0f, 1000.0f) == 0.0f, "and nothing moves once it is fixed");
}

// A tag laid out the way nametag.cpp does it: the box above the head, then
// the cull, then the icon beside a two-line column.
TagBox PlaceTag(const TagMetrics &m, float headY, float cullY, float screenH) {
	const float fullH = m.columnH > m.icon ? m.columnH : m.icon;
	TagBox      box;
	box.left   = 100.0f;
	box.right  = 200.0f;
	box.bottom = headY - m.headGap;
	box.top    = box.bottom - fullH;
	const float up = LiftAboveCull(TagLowestGlyphTop(box, m), cullY, screenH);
	box.top -= up;
	box.bottom -= up;
	return box;
}

void TestTagsInATallWindow() {
	std::printf("\nnametags in a tall window\n");
	const TagMetrics m = MeasureTag(1000.0f, 1.0f);

	TagBox box;
	box.top    = 0.0f;
	box.bottom = m.columnH;
	Check(TagTextTop(box, m) == 0.0f &&
	          TagLowestGlyphTop(box, m) == 0.0f + m.nameH + m.lineGap + m.shadow,
	      "the lowest glyph is the health line's shadow");

	const float retail = TextCullLine(958.0f, 1000.0f, false);
	const float fixed  = TextCullLine(958.0f, 1000.0f, true);

	const TagBox high = PlaceTag(m, 500.0f, retail, 1000.0f);
	Check(high.bottom == 500.0f - m.headGap, "a tag in the upper part is where it always was");

	const TagBox low      = PlaceTag(m, 990.0f, fixed, 1000.0f);
	const float  lowGlyph = TagLowestGlyphTop(low, m);
	Check(low.bottom == 990.0f - m.headGap && lowGlyph > 958.0f && lowGlyph < 1000.0f,
	      "fixed, one low on the screen stays over the head, below y = width");

	const TagBox pinned = PlaceTag(m, 990.0f, retail, 1000.0f);
	Check(TagLowestGlyphTop(pinned, m) < 958.0f &&
	          TagLowestGlyphTop(pinned, m) >= 958.0f - TEXT_CULL_CLEARANCE_PX - 0.01f,
	      "retail, it goes up to the line rather than vanish");

	const TagBox under = PlaceTag(m, 1300.0f, retail, 1000.0f);
	Check(under.bottom == 1300.0f - m.headGap,
	      "and one whose player is off the bottom is not pulled on");
}

// ---- the bytes in the real exe ----------------------------------------------------------

bool LoadExe(std::vector<uint8_t> &image, std::string &from) {
	std::vector<std::string> candidates;
	if (const char *env = std::getenv("COOPIII_GTA3_EXE"))
		candidates.push_back(env);
	candidates.push_back("reference/bin/gta3.exe");
	candidates.push_back("../../../../reference/bin/gta3.exe");
	for (const std::string &path : candidates) {
		FILE *fh = std::fopen(path.c_str(), "rb");
		if (!fh)
			continue;
		std::fseek(fh, 0, SEEK_END);
		const long size = std::ftell(fh);
		std::fseek(fh, 0, SEEK_SET);
		image.resize(size > 0 ? size_t(size) : 0);
		const size_t got = image.empty() ? 0 : std::fread(image.data(), 1, image.size(), fh);
		std::fclose(fh);
		if (got == image.size() && image.size() == IMAGE_SIZE) {
			from = path;
			return true;
		}
	}
	return false;
}

uint32_t Dword(const std::vector<uint8_t> &img, uint32_t va) {
	const size_t o = va - IMAGE_BASE;
	return uint32_t(img[o]) | uint32_t(img[o + 1]) << 8 | uint32_t(img[o + 2]) << 16 |
	       uint32_t(img[o + 3]) << 24;
}

// .text, as the section table has it; file offset = VA - image base.
constexpr uint32_t TEXT_BEGIN = 0x00401000;
constexpr uint32_t TEXT_END   = 0x005E3238;

void TestTheBytesInTheImage() {
	std::printf("\nPrintChar's y test in gta3.exe\n");
	std::vector<uint8_t> img;
	std::string          from;
	if (!LoadExe(img, from)) {
		std::printf("  [skipped] no retail gta3.exe; set COOPIII_GTA3_EXE to check the "
		            "bytes against one\n");
		return;
	}
	std::printf("  reading %s\n", from.c_str());

	const uint8_t *const block = &img[CFont__PrintChar - IMAGE_BASE];
	Check(ClassifyFontCull(block) == FontCullBytes::Retail,
	      "the four tests at 0x00500C30 are the ones addresses.h spells out");
	Check(Dword(img, CFont__PrintChar + 0x02) == RsGlobal__maximumWidth &&
	          Dword(img, CFont__PrintChar + 0x2B) == 0x005FD704 &&
	          Dword(img, CFont__PrintChar + 0x40) == 0x005FD704,
	      "x and y are tested against the width and 0.0f");
	Check(Dword(img, 0x005FD704) == 0, "which really is 0.0f");

	const uint32_t call = CFont__PrintString_PrintCharCall;
	Check(img[call - IMAGE_BASE] == 0xE8 && call + 5 + Dword(img, call + 1) == CFont__PrintChar,
	      "PrintString's loop calls it at 0x0050179F");

	// Every rel32 call, jmp and jcc in .text: none may land inside the block
	// past its first byte.
	int into = 0, calls = 0;
	for (uint32_t va = TEXT_BEGIN; va + 6 <= TEXT_END; ++va) {
		const uint8_t op = img[va - IMAGE_BASE];
		uint32_t      to = 0;
		if (op == 0xE8 || op == 0xE9)
			to = va + 5 + Dword(img, va + 1);
		else if (op == 0x0F && (img[va + 1 - IMAGE_BASE] & 0xF0) == 0x80)
			to = va + 6 + Dword(img, va + 2);
		else
			continue;
		if (to == CFont__PrintChar && op == 0xE8)
			++calls;
		if (to > CFont__PrintChar && to < CFont__PrintChar + BLOCK)
			++into;
	}
	Check(into == 0, "nothing in .text branches into the block");
	Check(calls == 1, "and PrintString's is the only call to PrintChar");
	// PrintChar's one short jump, 74 09 at 0x00500CE4.
	Check(img[0x00500CE4 - IMAGE_BASE] == 0x74 && 0x00500CE6 + img[0x00500CE5 - IMAGE_BASE] == 0x00500CEF,
	      "its one short jump lands at 0x00500CEF, past the block");

	// The store the fix drops is scratch: the next write to [esp+10h] is at
	// 0x00500CB0, ahead of the next read at 0x00500CB8.
	Check(std::memcmp(&img[0x00500CB0 - IMAGE_BASE], "\x89\x54\x24\x10", 4) == 0 &&
	          std::memcmp(&img[0x00500CB8 - IMAGE_BASE], "\xDB\x44\x24\x10", 4) == 0,
	      "[esp+10h] is written again before it is read again");

	std::vector<uint8_t> patched(block, block + BLOCK);
	std::memcpy(&patched[AT], PRINTCHAR_CULL_Y_HEIGHT, FIX);
	Check(ClassifyFontCull(patched.data()) == FontCullBytes::Height,
	      "and the image with the fix in reads as fixed");
}

} // namespace

int RunFontCullTests() {
	TestWhatGetsRewritten();
	TestWhereTheLineIs();
	TestTagsInATallWindow();
	TestTheBytesInTheImage();
	return g_cullFailures;
}
