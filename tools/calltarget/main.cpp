// Resolves E8/E9 rel32 targets in gta3.exe and dumps the bytes at the
// destination, so you can tell whether it's actually a function start.
//
//   calltarget <exe> <VA-of-call> [<VA-of-call> ...]
//
// Wrote this after getting a rel32 target wrong by hand, twice in one
// session. First time I caught it reading the disassembly, second time the
// game just crashed at the bogus address. Land one byte off and you end up
// mid-instruction; the prologue check below is what makes that obvious.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace {

constexpr uint32_t IMAGE_BASE = 0x00400000;

// The exe is not relocated and .text is mapped at a flat delta from the file,
// so VA - IMAGE_BASE is the file offset.
long FileOffset(uint32_t va) {
	return static_cast<long>(va - IMAGE_BASE);
}

bool ReadAt(FILE *fh, long offset, void *out, size_t len) {
	return std::fseek(fh, offset, SEEK_SET) == 0 && std::fread(out, 1, len, fh) == len;
}

// Common x86 prologue byte patterns. Not proof of anything, but it catches
// the failure that matters: landing inside another instruction.
const char *LooksLikeFunctionStart(const uint8_t *b) {
	if (b[0] == 0x55 && b[1] == 0x8B && b[2] == 0xEC) return "push ebp; mov ebp,esp";
	if (b[0] == 0x53 && b[1] == 0x89 && b[2] == 0xCB) return "push ebx; mov ebx,ecx (thiscall)";
	if (b[0] == 0x56 && b[1] == 0x89 && b[2] == 0xCE) return "push esi; mov esi,ecx (thiscall)";
	if (b[0] == 0x8B && b[1] == 0x0D)                 return "mov ecx,[global]";
	if (b[0] == 0x8B && b[1] == 0x44 && b[2] == 0x24) return "mov eax,[esp+..]";
	if (b[0] == 0x81 && b[1] == 0xEC)                 return "sub esp,imm32";
	if (b[0] == 0x83 && b[1] == 0xEC)                 return "sub esp,imm8";
	if (b[0] == 0x53 || b[0] == 0x55 || b[0] == 0x56 || b[0] == 0x57)
		return "push (reg): plausible prologue";
	if (b[0] == 0xA1)                                 return "mov eax,[global]";
	return nullptr;
}

// Signs of landing mid-function instead: an epilogue.
const char *LooksLikeEpilogue(const uint8_t *b) {
	if (b[0] == 0xC3)                 return "ret: this is the END of a function";
	if (b[0] == 0xC2)                 return "ret imm16: this is the END of a function";
	if (b[0] == 0x83 && b[1] == 0xC4) return "add esp,imm: epilogue, not a start";
	if (b[0] == 0x5B || b[0] == 0x5D || b[0] == 0x5E || b[0] == 0x5F)
		return "pop (reg): epilogue, not a start";
	return nullptr;
}

void Report(FILE *fh, uint32_t callVa) {
	uint8_t op = 0;
	if (!ReadAt(fh, FileOffset(callVa), &op, 1)) {
		std::printf("0x%08X: cannot read\n", callVa);
		return;
	}
	if (op != 0xE8 && op != 0xE9) {
		std::printf("0x%08X: not a call/jmp rel32 (first byte is 0x%02X)\n", callVa, op);
		return;
	}

	int32_t rel = 0;
	if (!ReadAt(fh, FileOffset(callVa) + 1, &rel, 4)) {
		std::printf("0x%08X: cannot read the displacement\n", callVa);
		return;
	}

	const uint32_t next   = callVa + 5;
	const uint32_t target = static_cast<uint32_t>(next + rel);

	std::printf("0x%08X  %s rel32=%+d (0x%08X)\n", callVa, op == 0xE8 ? "call" : "jmp ",
	            rel, static_cast<uint32_t>(rel));
	std::printf("            next=0x%08X  ->  TARGET 0x%08X\n", next, target);

	uint8_t bytes[16] = {0};
	if (!ReadAt(fh, FileOffset(target), bytes, sizeof(bytes))) {
		std::printf("            target is not inside the file\n\n");
		return;
	}

	std::printf("            bytes:");
	for (uint8_t b : bytes)
		std::printf(" %02X", b);
	std::printf("\n");

	if (const char *bad = LooksLikeEpilogue(bytes))
		std::printf("            *** SUSPECT: %s\n", bad);
	else if (const char *good = LooksLikeFunctionStart(bytes))
		std::printf("            looks like a function start: %s\n", good);
	else
		std::printf("            unrecognised prologue, check it by hand\n");
	std::printf("\n");
}

} // namespace

int main(int argc, char **argv) {
	if (argc < 3) {
		std::printf("usage: calltarget <exe> <VA-of-call> [<VA-of-call> ...]\n");
		return 2;
	}

	FILE *fh = std::fopen(argv[1], "rb");
	if (!fh) {
		std::printf("cannot open %s\n", argv[1]);
		return 1;
	}

	for (int i = 2; i < argc; ++i)
		Report(fh, static_cast<uint32_t>(std::strtoul(argv[i], nullptr, 0)));

	std::fclose(fh);
	return 0;
}
