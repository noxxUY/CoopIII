#include "verify.h"

#include "addresses.h"

#include <cstdio>
#include <cstring>

#ifdef _WIN32
#include <windows.h>
#endif

namespace coopiii::game {

namespace {

// Reads `len` bytes at `va` only if the whole span is committed and
// readable. A wrong exe is exactly the case where an address might be
// unmapped, so this check can't itself fault while deciding whether it's
// safe to proceed.
bool SafeRead(uintptr_t va, void *out, size_t len) {
#ifdef _WIN32
	MEMORY_BASIC_INFORMATION mbi;
	uintptr_t                cursor = va;
	const uintptr_t          end    = va + len;

	while (cursor < end) {
		if (VirtualQuery(reinterpret_cast<LPCVOID>(cursor), &mbi, sizeof(mbi)) != sizeof(mbi))
			return false;
		if (mbi.State != MEM_COMMIT)
			return false;
		const DWORD prot = mbi.Protect & 0xFF;
		if (prot == PAGE_NOACCESS || (mbi.Protect & PAGE_GUARD))
			return false;
		cursor = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
	}
	std::memcpy(out, reinterpret_cast<const void *>(va), len);
	return true;
#else
	(void)va; (void)out; (void)len;
	return false;
#endif
}

bool MatchesString(uintptr_t va, const char *expect) {
	const size_t len = std::strlen(expect);
	char         buf[64];
	if (len >= sizeof(buf))
		return false;
	if (!SafeRead(va, buf, len))
		return false;
	return std::memcmp(buf, expect, len) == 0;
}

bool MatchesBytes(uintptr_t va, const uint8_t *expect, size_t len) {
	uint8_t buf[32];
	if (len > sizeof(buf))
		return false;
	if (!SafeRead(va, buf, len))
		return false;
	return std::memcmp(buf, expect, len) == 0;
}

// Anchors chosen so each one independently distinguishes this build.
//
// Both strings live in .rdata. That matters: SilentPatch and friends
// rewrite *code*, so a code anchor could in principle get patched out from
// under us, while a read-only string just stays put. See docs/compat.md
// §2.2.
constexpr uintptr_t VA_CHEAT_ARMOUR  = 0x005F6618;   // "ESIOTRUT" - 1.0 spelling
constexpr uintptr_t VA_VERSION_MAGIC = 0x005F4DF4;   // "grandtheftauto3"

// Function prologue of ProcessCommands1100To1199. Code, so treated as
// advisory only - a mismatch gets logged but doesn't by itself block
// loading.
constexpr uint8_t PROLOGUE_1100[] = {0x53, 0x56, 0x57, 0x55, 0x81, 0xEC, 0x90, 0x01, 0x00, 0x00};

} // namespace

VerifyResult VerifyGameImage() {
	VerifyResult r;

#ifndef _WIN32
	r.detail = "not a Windows build";
	return r;
#else
	const HMODULE exe = GetModuleHandleA(nullptr);
	if (!exe) {
		r.detail = "cannot resolve the main module";
		return r;
	}

	const auto base = reinterpret_cast<uintptr_t>(exe);
	if (base != IMAGE_BASE) {
		char buf[160];
		std::snprintf(buf, sizeof(buf),
		              "gta3.exe is loaded at 0x%08X, expected 0x%08X. "
		              "Every address CoopIII uses is absolute, so a relocated image "
		              "cannot be hooked safely.",
		              static_cast<unsigned>(base), static_cast<unsigned>(IMAGE_BASE));
		r.detail = buf;
		return r;
	}

	if (!MatchesString(VA_VERSION_MAGIC, "grandtheftauto3")) {
		r.detail = "this does not look like GTA III, the version magic is missing. "
		           "CoopIII will not load.";
		return r;
	}

	if (!MatchesString(VA_CHEAT_ARMOUR, "ESIOTRUT")) {
		r.detail =
		    "this is GTA III, but not the v1.0 retail build CoopIII targets "
		    "(the 1.0 marker is absent; v1.1 and the Steam build spell it "
		    "differently). Expected MD5 " +
		    std::string(IMAGE_MD5) + ", " + std::to_string(IMAGE_SIZE) +
		    " bytes. CoopIII will not load, its addresses belong to 1.0 only.";
		return r;
	}

	r.ok = true;

	// Advisory only. A patched prologue just means a mod got here first,
	// which is expected and fine - still worth a log line when diagnosing
	// a conflict though.
	const bool prologueClean =
	    MatchesBytes(CRunningScript__ProcessCommands1100To1199, PROLOGUE_1100,
	                 sizeof(PROLOGUE_1100));

	char versionName[64] = {0};
	const bool haveName = SafeRead(version_name, versionName, sizeof(versionName) - 1);

	char buf[256];
	std::snprintf(buf, sizeof(buf),
	              "GTA III v1.0 retail confirmed at 0x%08X (data reports \"%s\"%s)",
	              static_cast<unsigned>(base),
	              haveName && versionName[0] ? versionName : "unknown",
	              prologueClean ? "" : "; script dispatcher already patched by another mod");
	r.detail = buf;
	return r;
#endif
}

} // namespace coopiii::game
