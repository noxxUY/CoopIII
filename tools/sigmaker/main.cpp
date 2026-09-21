// Turns an address in gta3.exe into a byte pattern the client can scan for at
// runtime, and checks the pattern is unique inside .text.
//
// Hardcoded addresses break the moment anything shifts. A pattern that's
// verified unique tends to survive. Basically the bridge between binary
// analysis and client code.
//
//   sigmaker <exe> <VA>            generate a unique pattern for that address
//   sigmaker <exe> --test "AA BB"  check how many times a pattern occurs
//
// VAs are as seen in IDA, e.g. 0x004A1234.

#include "hook/pattern.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

using namespace coopiii;

namespace {

struct Section {
	std::string name;
	uint32_t    va    = 0;   // virtual address (absolute, image base included)
	uint32_t    vsize = 0;
	uint32_t    raw   = 0;   // file offset
	uint32_t    rsize = 0;
	bool        exec  = false;
};

struct Image {
	std::vector<uint8_t> file;
	std::vector<Section> sections;
	uint32_t             base = 0;

	const Section *SectionForVA(uint32_t va) const {
		for (const Section &s : sections)
			if (va >= s.va && va < s.va + s.vsize)
				return &s;
		return nullptr;
	}

	const Section *Text() const {
		for (const Section &s : sections)
			if (s.exec)
				return &s;
		return nullptr;
	}

	// File-backed bytes for a VA, or null if it is not mapped from the file.
	const uint8_t *At(uint32_t va, size_t need) const {
		const Section *s = SectionForVA(va);
		if (!s)
			return nullptr;
		const uint32_t delta = va - s->va;
		if (delta + need > s->rsize)
			return nullptr;
		return file.data() + s->raw + delta;
	}
};

bool LoadImage(const char *path, Image &img) {
	FILE *fh = std::fopen(path, "rb");
	if (!fh) {
		std::printf("cannot open %s\n", path);
		return false;
	}
	std::fseek(fh, 0, SEEK_END);
	const long size = std::ftell(fh);
	std::fseek(fh, 0, SEEK_SET);
	img.file.resize(static_cast<size_t>(size));
	const size_t got = std::fread(img.file.data(), 1, img.file.size(), fh);
	std::fclose(fh);
	if (got != img.file.size()) {
		std::printf("short read\n");
		return false;
	}

	if (img.file.size() < 0x40 || img.file[0] != 'M' || img.file[1] != 'Z') {
		std::printf("not a PE\n");
		return false;
	}

	uint32_t peOff = 0;
	std::memcpy(&peOff, img.file.data() + 0x3C, 4);
	if (peOff + 0xF8 > img.file.size()) {
		std::printf("bad PE offset\n");
		return false;
	}

	const uint8_t *nt = img.file.data() + peOff;
	if (std::memcmp(nt, "PE\0\0", 4) != 0) {
		std::printf("bad PE signature\n");
		return false;
	}

	uint16_t nSections = 0, optSize = 0;
	std::memcpy(&nSections, nt + 6, 2);
	std::memcpy(&optSize, nt + 20, 2);
	std::memcpy(&img.base, nt + 24 + 28, 4);   // OptionalHeader.ImageBase (PE32)

	const uint8_t *sec = nt + 24 + optSize;
	for (uint16_t i = 0; i < nSections; ++i, sec += 40) {
		Section s;
		char name[9] = {0};
		std::memcpy(name, sec, 8);
		s.name = name;
		uint32_t vsize = 0, vaddr = 0, rsize = 0, raw = 0, chars = 0;
		std::memcpy(&vsize, sec + 8, 4);
		std::memcpy(&vaddr, sec + 12, 4);
		std::memcpy(&rsize, sec + 16, 4);
		std::memcpy(&raw, sec + 20, 4);
		std::memcpy(&chars, sec + 36, 4);
		s.va    = img.base + vaddr;
		s.vsize = vsize;
		s.raw   = raw;
		s.rsize = rsize;
		s.exec  = (chars & 0x20000000u) != 0;   // IMAGE_SCN_MEM_EXECUTE
		img.sections.push_back(s);
	}
	return true;
}

std::string HexByte(uint8_t b) {
	static const char *digits = "0123456789ABCDEF";
	std::string out;
	out += digits[b >> 4];
	out += digits[b & 0xF];
	return out;
}

// Wildcards bytes that look like an absolute address into the image.
// Those are the ones most likely to move between builds; keep them literal
// and the pattern breaks on the next recompile.
std::vector<bool> WildcardMask(const Image &img, const uint8_t *code, size_t len) {
	std::vector<bool> mask(len, false);
	if (len < 4)
		return mask;

	const uint32_t lo = img.base;
	const uint32_t hi = img.base + 0x600000;   // generous: covers .text..bss

	for (size_t i = 0; i + 4 <= len; ++i) {
		uint32_t dword = 0;
		std::memcpy(&dword, code + i, 4);
		if (dword >= lo && dword < hi) {
			for (size_t k = 0; k < 4; ++k)
				mask[i + k] = true;
			i += 3;
		}
	}
	return mask;
}

std::string FormatPattern(const uint8_t *code, const std::vector<bool> &mask, size_t len) {
	std::string out;
	for (size_t i = 0; i < len; ++i) {
		if (i)
			out += ' ';
		out += mask[i] ? "??" : HexByte(code[i]);
	}
	return out;
}

size_t CountMatches(const Image &img, const std::string &pattern) {
	const Section *text = img.Text();
	if (!text)
		return 0;
	const ScanResult r = Scan(img.file.data() + text->raw, text->rsize, pattern.c_str());
	return r.matches;
}

int Generate(const Image &img, uint32_t va) {
	const Section *sec = img.SectionForVA(va);
	if (!sec) {
		std::printf("VA 0x%08X is not inside any section\n", va);
		return 1;
	}
	std::printf("VA 0x%08X is in %s\n", va, sec->name.c_str());

	const size_t kMax = 64;
	const uint8_t *code = img.At(va, kMax);
	if (!code) {
		std::printf("VA is not backed by file data (uninitialised section?)\n");
		return 1;
	}

	const std::vector<bool> mask = WildcardMask(img, code, kMax);

	// Grow the pattern until exactly one match remains. Start short so the
	// result stays minimal - every extra literal byte is one more thing a
	// future patch could break.
	for (size_t len = 8; len <= kMax; ++len) {
		// Do not end on a wildcard: trailing ?? adds length without specificity.
		if (mask[len - 1])
			continue;

		const std::string pattern = FormatPattern(code, mask, len);
		const size_t      hits    = CountMatches(img, pattern);

		if (hits == 1) {
			std::printf("\nunique at %zu bytes:\n\n  \"%s\"\n\n", len, pattern.c_str());
			size_t wild = 0;
			for (size_t i = 0; i < len; ++i)
				if (mask[i])
					++wild;
			std::printf("(%zu literal bytes, %zu wildcarded as image addresses)\n",
			            len - wild, wild);
			return 0;
		}
		if (hits == 0) {
			std::printf("internal error: generated pattern matches nothing at len %zu\n", len);
			return 1;
		}
	}

	std::printf("no unique pattern within %zu bytes; the code here is too generic,\n"
	            "anchor on a nearby distinctive site instead\n", kMax);
	return 1;
}

int Test(const Image &img, const char *pattern) {
	const Pattern p = Pattern::Parse(pattern);
	if (!p.IsValid()) {
		std::printf("malformed pattern\n");
		return 1;
	}

	const Section *text = img.Text();
	const ScanResult r = Scan(img.file.data() + text->raw, text->rsize, p);
	std::printf("matches: %zu (%s)\n", r.matches, r.StatusName());

	if (r.matches >= 1) {
		// Report every hit as a VA so an ambiguous pattern can be diagnosed.
		const uint8_t *begin = img.file.data() + text->raw;
		for (size_t i = 0; i + p.Size() <= text->rsize; ++i) {
			if (!p.MatchesAt(begin + i))
				continue;
			std::printf("  0x%08X\n", static_cast<uint32_t>(text->va + i));
		}
	}
	return r.matches == 1 ? 0 : 1;
}

} // namespace

int main(int argc, char **argv) {
	if (argc < 3) {
		std::printf("usage:\n"
		            "  sigmaker <exe> <VA>             generate a unique pattern\n"
		            "  sigmaker <exe> --test \"AA BB\"   count occurrences of a pattern\n");
		return 2;
	}

	Image img;
	if (!LoadImage(argv[1], img))
		return 1;

	std::printf("image base 0x%08X, %zu sections\n", img.base, img.sections.size());

	if (std::strcmp(argv[2], "--test") == 0) {
		if (argc < 4) {
			std::printf("--test needs a pattern\n");
			return 2;
		}
		return Test(img, argv[3]);
	}

	const uint32_t va = static_cast<uint32_t>(std::strtoul(argv[2], nullptr, 0));
	return Generate(img, va);
}
