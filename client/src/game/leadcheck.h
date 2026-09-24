// Reading a call out of the image, for an address that is a lead rather than
// something addresses.h proves (docs/addresses-unverified.md). A lead is only
// used where the code around a call site addresses.h does prove says it is
// what the lead claims, and the check is these few byte compares.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace coopiii::game {

// Is there `call target` at `insn`, a five-byte E8 with a 32-bit displacement
// from the next instruction?
inline bool RelCallAt(const uint8_t *insn, uintptr_t insnAddr, uintptr_t target) {
	if (insn[0] != 0xE8)
		return false;
	int32_t rel;
	std::memcpy(&rel, insn + 1, sizeof rel);
	return insnAddr + 5 + static_cast<uintptr_t>(static_cast<int64_t>(rel)) == target;
}

// A call to `target` anywhere in the `len` bytes at `window`.
inline bool CallIn(const uint8_t *window, uintptr_t windowAddr, size_t len, uintptr_t target) {
	for (size_t i = 0; i + 5 <= len; ++i)
		if (RelCallAt(window + i, windowAddr + i, target))
			return true;
	return false;
}

// A one-argument __cdecl call to `target` anywhere in the `len` bytes at
// `window`: the call, then `add esp,4` or `pop ecx`, maybe after a
// `test al,al` on a bool result. Retail's rocket tests (0x0055B8F1,
// 0x0055B9CB) are the `test al,al / pop ecx` kind.
inline bool CdeclCallIn(const uint8_t *window, uintptr_t windowAddr, size_t len,
                        uintptr_t target) {
	for (size_t i = 0; i + 5 <= len; ++i) {
		if (!RelCallAt(window + i, windowAddr + i, target))
			continue;
		size_t j = i + 5;
		if (j + 2 <= len && window[j] == 0x84 && window[j + 1] == 0xC0)
			j += 2;
		if (j + 1 <= len && window[j] == 0x59)
			return true;
		if (j + 3 <= len && window[j] == 0x83 && window[j + 1] == 0xC4 &&
		    window[j + 2] == 0x04)
			return true;
	}
	return false;
}

} // namespace coopiii::game
