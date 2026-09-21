// Unit tests for the AOB pattern scanner.
//   xmake build patterntest && xmake run patterntest

#include "hook/pattern.h"

#include <cstdio>
#include <cstring>
#include <vector>

using namespace coopiii;

namespace {

int g_failures = 0;

void Check(bool cond, const char *what) {
	std::printf("  [%s] %s\n", cond ? "ok" : "FAIL", what);
	if (!cond)
		++g_failures;
}

void TestParsing() {
	std::printf("\npattern parsing\n");
	Check(Pattern::Parse("8B 0D ?? ?? ?? ?? 85 C9").IsValid(), "IDA-style pattern parses");
	Check(Pattern::Parse("8B0D85C9").IsValid(), "spaces are optional");
	Check(Pattern::Parse("8B ? C9").IsValid(), "single ? is a wildcard");
	Check(Pattern::Parse("8B 0D ?? ?? ?? ?? 85 C9").Size() == 8, "size counts wildcards");

	Check(!Pattern::Parse("8B 0G").IsValid(), "non-hex digit is rejected");
	Check(!Pattern::Parse("8B 0").IsValid(), "odd nibble is rejected");
	Check(!Pattern::Parse("?? ?? ??").IsValid(), "all-wildcard pattern is rejected");
	Check(!Pattern::Parse("").IsValid(), "empty pattern is rejected");
	Check(!Pattern::Parse(nullptr).IsValid(), "null pattern is rejected");
}

void TestScanning() {
	std::printf("\nscanning\n");
	const uint8_t buf[] = {0x00, 0x11, 0x8B, 0x0D, 0xDE, 0xAD, 0xBE, 0xEF,
	                       0x85, 0xC9, 0x22, 0x33};

	ScanResult r = Scan(buf, sizeof(buf), "8B 0D ?? ?? ?? ?? 85 C9");
	Check(r.Ok(), "finds a pattern with wildcards");
	Check(r.address == buf + 2, "returns the match address");
	Check(r.matches == 1, "reports one match");

	Check(Scan(buf, sizeof(buf), "8B 0D DE AD BE EF 85 C9").Ok(), "exact bytes match too");

	r = Scan(buf, sizeof(buf), "DE AD BE EF AA");
	Check(r.status == ScanResult::NOT_FOUND, "absent pattern is not found");
	Check(r.address == nullptr, "no address when not found");

	r = Scan(buf, sizeof(buf), "8B 0G");
	Check(r.status == ScanResult::BAD_PATTERN, "malformed pattern reported as such");
}

void TestAmbiguityIsAnError() {
	std::printf("\nambiguity\n");
	// Two matches must never silently resolve to the first one - that's the
	// whole point of this test.
	const uint8_t buf[] = {0x90, 0xAB, 0xCD, 0x90, 0x00, 0xAB, 0xCD, 0x90};

	ScanResult r = Scan(buf, sizeof(buf), "AB CD");
	Check(r.status == ScanResult::AMBIGUOUS, "two matches reported as ambiguous");
	Check(r.matches == 2, "counts every match");
	Check(r.address == nullptr, "refuses to hand back a guess");
	Check(!r.Ok(), "Ok() is false for an ambiguous scan");
}

void TestEdges() {
	std::printf("\nedges\n");
	const uint8_t buf[] = {0xAA, 0xBB, 0xCC};

	ScanResult r = Scan(buf, sizeof(buf), "AA BB CC");
	Check(r.Ok() && r.address == buf, "pattern spanning the whole range matches");

	r = Scan(buf, sizeof(buf), "AA BB CC DD");
	Check(r.status == ScanResult::NOT_FOUND, "pattern longer than range is not found");

	r = Scan(buf, sizeof(buf), "CC");
	Check(r.Ok() && r.address == buf + 2, "match at the very end is found");

	r = Scan(nullptr, 0, "AA");
	Check(!r.Ok(), "null range does not crash");
}

void TestResolveRelative() {
	std::printf("\nrel32 resolution\n");
	// E8 <rel32> = call. Target = next instruction + rel32.
	uint8_t call[5] = {0xE8, 0x00, 0x00, 0x00, 0x00};
	const int32_t rel = 0x20;
	std::memcpy(call + 1, &rel, sizeof(rel));

	uint8_t *target = ResolveRelative(call, 1, 5);
	Check(target == call + 5 + 0x20, "forward call resolves");

	const int32_t back = -0x10;
	std::memcpy(call + 1, &back, sizeof(back));
	target = ResolveRelative(call, 1, 5);
	Check(target == call + 5 - 0x10, "backward call resolves");

	Check(ResolveRelative(nullptr, 1, 5) == nullptr, "null instruction is handled");
}

void TestRealisticPrologue() {
	std::printf("\nrealistic use\n");
	// A plausible MSVC6 prologue followed by a global access. Wildcarding the
	// global's address is why patterns keep working across builds where only
	// the data layout moved.
	const uint8_t code[] = {
	    0x55,                                            // push ebp
	    0x8B, 0xEC,                                      // mov ebp, esp
	    0x83, 0xEC, 0x10,                                // sub esp, 10h
	    0xA1, 0x78, 0x56, 0x34, 0x12,                    // mov eax, ds:[12345678]
	    0x85, 0xC0,                                      // test eax, eax
	    0x74, 0x0A,                                      // jz short
	};

	ScanResult r = Scan(code, sizeof(code), "55 8B EC 83 EC ?? A1 ?? ?? ?? ?? 85 C0");
	Check(r.Ok(), "prologue + wildcarded global address matches");

	// And the wildcarded global can then be read back out of the match.
	if (r.Ok()) {
		uint32_t global = 0;
		std::memcpy(&global, r.address + 7, sizeof(global));
		Check(global == 0x12345678, "global address readable from the match");
	}
}

} // namespace

int main() {
	TestParsing();
	TestScanning();
	TestAmbiguityIsAnError();
	TestEdges();
	TestResolveRelative();
	TestRealisticPrologue();

	std::printf("\n%s (%d failure%s)\n", g_failures == 0 ? "PASS" : "FAIL",
	            g_failures, g_failures == 1 ? "" : "s");
	return g_failures == 0 ? 0 : 1;
}
