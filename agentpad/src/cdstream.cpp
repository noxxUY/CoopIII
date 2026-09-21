#include "cdstream.h"

#include <cstring>

#ifdef _WIN32
#include <windows.h>
#endif

namespace agentpad {

namespace {

// Exact nine bytes we're allowed to overwrite, nothing else. If the image
// doesn't match this at CdStreamSemaphoreName, it's not the build we looked
// at, or something already renamed it - either way, leave it alone.
constexpr char ORIGINAL[] = "CdStream";   // 8 chars + NUL = 9 bytes

constexpr char BASE62[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";

} // namespace

void FormatCdStreamName(uint32_t pid, char *out, size_t cap) {
	if (!out || cap < sizeof(ORIGINAL))
		return;

	// "Cd" + six base-62 digits = eight characters, same length as "CdStream",
	// so it fits in the original footprint and nothing adjacent gets touched.
	// 62^6 = 56,800,235,584, bigger than 2^32, so every pid maps to a unique name.
	out[0] = 'C';
	out[1] = 'd';
	uint32_t v = pid;
	for (int i = 7; i >= 2; --i) {
		out[i] = BASE62[v % 62];
		v /= 62;
	}
	out[8] = '\0';
}

#ifdef _WIN32

CdStreamPatch ApplyCdStreamPatch(Settings::MultiInstance mode, uint32_t pid) {
	CdStreamPatch out;

	if (mode == Settings::MultiInstance::Off) {
		out.reason = "MultiInstance=off, the game keeps the shared \"CdStream\" semaphore name";
		return out;
	}

	if (mode == Settings::MultiInstance::Auto) {
		// Only deviate if we have to. No existing "CdStream" means we're the
		// first (or only) copy, so behave exactly like unpatched retail.
		const HANDLE existing = OpenSemaphoreA(SEMAPHORE_MODIFY_STATE, FALSE, ORIGINAL);
		if (!existing) {
			out.reason = "MultiInstance=auto and no other copy of the game holds \"CdStream\""
			             ", left alone";
			return out;
		}
		CloseHandle(existing);
	}

	// Don't write blind - if this isn't the image we derived the offsets from,
	// the bytes won't match and we bail.
	if (reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr)) != 0x00400000u) {
		out.reason = "the game image is not at its expected base, not patching anything";
		return out;
	}

	auto *dst = reinterpret_cast<char *>(CdStreamSemaphoreName);
	if (std::memcmp(dst, ORIGINAL, sizeof(ORIGINAL)) != 0) {
		out.reason = "the bytes at 0x005EC034 are not \"CdStream\": wrong build, or something "
		             "else renamed it already; left alone";
		return out;
	}

	FormatCdStreamName(pid, out.name, sizeof(out.name));

	DWORD old = 0;
	if (!VirtualProtect(dst, sizeof(ORIGINAL), PAGE_READWRITE, &old)) {
		out.name[0] = '\0';
		out.reason  = "VirtualProtect refused the string page, left alone";
		return out;
	}
	std::memcpy(dst, out.name, sizeof(ORIGINAL));
	VirtualProtect(dst, sizeof(ORIGINAL), old, &old);

	out.applied = true;
	out.reason  = "renamed the streaming semaphore so a second copy of the game cannot share it";
	return out;
}

#else

CdStreamPatch ApplyCdStreamPatch(Settings::MultiInstance, uint32_t) {
	CdStreamPatch out;
	out.reason = "not a Windows build";
	return out;
}

#endif

} // namespace agentpad
