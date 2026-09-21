// Byte-pattern (AOB) scanning.
//
// CoopIII finds game addresses by scanning for code patterns instead of
// hardcoding offsets. The user's install runs SilentPatch, which rewrites
// some game code at runtime, so an address that's correct on disk can land
// in rewritten bytes once it's in memory. Scanning what's actually mapped is
// the only way to be sure.
//
// Scanning takes an explicit range so it can be tested against a plain
// buffer without the game running (see tools/patterntest).
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace coopiii {

// An IDA-style pattern: "8B 0D ?? ?? ?? ?? 85 C9", where ?? matches any byte.
class Pattern {
public:
	// Returns an invalid Pattern if the text is malformed; check IsValid().
	static Pattern Parse(const char *ida);

	bool IsValid() const { return !m_bytes.empty(); }
	size_t Size() const { return m_bytes.size(); }

	bool MatchesAt(const uint8_t *p) const;

private:
	std::vector<uint8_t> m_bytes;
	std::vector<bool>    m_wildcard;
};

struct ScanResult {
	enum Status : uint8_t {
		FOUND,          // exactly one match, the only status safe to use
		NOT_FOUND,
		AMBIGUOUS,      // several matches, pattern needs to be more specific
		BAD_PATTERN,
	};

	Status    status  = NOT_FOUND;
	uint8_t  *address = nullptr;
	size_t    matches = 0;

	bool Ok() const { return status == FOUND; }
	const char *StatusName() const;
};

// Scans [begin, begin+size). Always scans the whole range instead of
// stopping at the first hit - a pattern matching twice is a latent bug, and
// silently taking the first match is exactly how mods end up hooking the
// wrong function.
ScanResult Scan(const uint8_t *begin, size_t size, const Pattern &pattern);
ScanResult Scan(const uint8_t *begin, size_t size, const char *idaPattern);

// Range of the main module's executable section. Returns false off-Windows or
// if the PE headers look wrong.
bool GetMainModuleCode(const uint8_t **begin, size_t *size);

// Convenience: scan the main module's code section.
ScanResult ScanModule(const char *idaPattern);

// x86 helper: given the address of an instruction with a rel32 displacement
// (e.g. E8 call / E9 jmp), return the absolute target.
// `offsetToRel32` is how far the rel32 sits from `instruction`.
uint8_t *ResolveRelative(uint8_t *instruction, size_t offsetToRel32, size_t instructionLength);

} // namespace coopiii
