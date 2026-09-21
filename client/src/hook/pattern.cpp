#include "pattern.h"

#include <cctype>
#include <cstring>

#ifdef _WIN32
#include <windows.h>
#endif

namespace coopiii {

namespace {

int HexVal(char c) {
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return -1;
}

} // namespace

const char *ScanResult::StatusName() const {
	switch (status) {
	case FOUND:       return "found";
	case NOT_FOUND:   return "not found";
	case AMBIGUOUS:   return "ambiguous";
	case BAD_PATTERN: return "bad pattern";
	}
	return "?";
}

Pattern Pattern::Parse(const char *ida) {
	Pattern out;
	if (!ida)
		return out;

	for (const char *p = ida; *p != '\0';) {
		if (std::isspace(static_cast<unsigned char>(*p))) {
			++p;
			continue;
		}

		if (*p == '?') {
			++p;
			if (*p == '?')
				++p;   // "??" and "?" both mean one wildcard byte
			out.m_bytes.push_back(0);
			out.m_wildcard.push_back(true);
			continue;
		}

		const int hi = HexVal(p[0]);
		const int lo = p[1] != '\0' ? HexVal(p[1]) : -1;
		if (hi < 0 || lo < 0) {
			// Malformed - return an invalid pattern instead of a partial one
			// that would just silently match the wrong thing.
			return Pattern{};
		}

		out.m_bytes.push_back(static_cast<uint8_t>(hi * 16 + lo));
		out.m_wildcard.push_back(false);
		p += 2;
	}

	// A pattern that's all wildcards matches everywhere. Refuse it.
	bool anyConcrete = false;
	for (size_t i = 0; i < out.m_wildcard.size(); ++i)
		if (!out.m_wildcard[i])
			anyConcrete = true;
	if (!anyConcrete)
		return Pattern{};

	return out;
}

bool Pattern::MatchesAt(const uint8_t *p) const {
	for (size_t i = 0; i < m_bytes.size(); ++i) {
		if (m_wildcard[i])
			continue;
		if (p[i] != m_bytes[i])
			return false;
	}
	return true;
}

ScanResult Scan(const uint8_t *begin, size_t size, const Pattern &pattern) {
	ScanResult result;

	if (!pattern.IsValid() || !begin) {
		result.status = ScanResult::BAD_PATTERN;
		return result;
	}
	if (pattern.Size() > size) {
		result.status = ScanResult::NOT_FOUND;
		return result;
	}

	const size_t last = size - pattern.Size();
	for (size_t i = 0; i <= last; ++i) {
		if (!pattern.MatchesAt(begin + i))
			continue;

		++result.matches;
		if (result.matches == 1)
			result.address = const_cast<uint8_t *>(begin + i);
	}

	result.status = result.matches == 0   ? ScanResult::NOT_FOUND
	                : result.matches == 1 ? ScanResult::FOUND
	                                      : ScanResult::AMBIGUOUS;
	if (result.status == ScanResult::AMBIGUOUS)
		result.address = nullptr;

	return result;
}

ScanResult Scan(const uint8_t *begin, size_t size, const char *idaPattern) {
	return Scan(begin, size, Pattern::Parse(idaPattern));
}

bool GetMainModuleCode(const uint8_t **begin, size_t *size) {
#ifdef _WIN32
	auto *base = reinterpret_cast<const uint8_t *>(GetModuleHandleA(nullptr));
	if (!base)
		return false;

	const auto *dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(base);
	if (dos->e_magic != IMAGE_DOS_SIGNATURE)
		return false;

	const auto *nt = reinterpret_cast<const IMAGE_NT_HEADERS *>(base + dos->e_lfanew);
	if (nt->Signature != IMAGE_NT_SIGNATURE)
		return false;

	const IMAGE_SECTION_HEADER *sections = IMAGE_FIRST_SECTION(nt);
	for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
		if (!(sections[i].Characteristics & IMAGE_SCN_MEM_EXECUTE))
			continue;
		*begin = base + sections[i].VirtualAddress;
		*size  = sections[i].Misc.VirtualSize;
		return true;
	}
	return false;
#else
	(void)begin;
	(void)size;
	return false;
#endif
}

ScanResult ScanModule(const char *idaPattern) {
	const uint8_t *begin = nullptr;
	size_t         size  = 0;
	if (!GetMainModuleCode(&begin, &size)) {
		ScanResult r;
		r.status = ScanResult::NOT_FOUND;
		return r;
	}
	return Scan(begin, size, idaPattern);
}

uint8_t *ResolveRelative(uint8_t *instruction, size_t offsetToRel32, size_t instructionLength) {
	if (!instruction)
		return nullptr;

	int32_t rel = 0;
	std::memcpy(&rel, instruction + offsetToRel32, sizeof(rel));
	return instruction + instructionLength + rel;
}

} // namespace coopiii
