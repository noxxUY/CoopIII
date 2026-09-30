// The map's parked cars rolled from the session's seed (game/cargen.h): the
// arithmetic every machine has to agree on, the packet that carries the seed,
// and - with a copy of the retail exe - the call and the rand() it leans on.

#include "client.h"
#include "game/addresses.h"
#include "game/cargen.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using namespace coopiii;
using namespace coopiii::game;

namespace {

int g_cargenFailures = 0;

void Check(bool cond, const char *what) {
	std::printf("  [%s] %s\n", cond ? "ok" : "FAIL", what);
	if (!cond)
		++g_cargenFailures;
}

void TestTheRollIsTheSessions() {
	std::printf("\na parked car's roll: the session's and the generator's\n");
	Check(ParkedCarRoll(0x12345678u, 7) == ParkedCarRoll(0x12345678u, 7),
	      "one seed and one generator give one number, on any machine");
	Check(ParkedCarRoll(0x12345678u, 7) != ParkedCarRoll(0x12345679u, 7),
	      "another session rolls the same generator differently");

	// Neighbouring generators must not come out alike, or a car park would be
	// one colour.
	int      rows[8]  = {};
	uint32_t distinct = 0;
	for (uint32_t g = 0; g < 149; ++g) {
		const uint8_t row = ParkedColourRow(ParkedCarRoll(0xC0FFEEu, g), 8);
		if (rows[row]++ == 0)
			++distinct;
	}
	bool spread = distinct == 8;
	for (int r : rows)
		spread = spread && r >= 8;
	Check(spread, "the map's 149 generators spread over every row of an eight-colour table");
	Check(ParkedCarRoll(0, 0) != ParkedCarRoll(0, 1), "and a zero seed still tells them apart");
}

void TestWhichColourIsRolled() {
	std::printf("\nwhich generators' colours are the session's\n");
	Check(GeneratorRollsColour(-1, -1), "a generator that asks for a roll");
	Check(GeneratorRollsColour(3, -1) && GeneratorRollsColour(-1, 3),
	      "one colour missing: the engine rolls both, so do we");
	Check(!GeneratorRollsColour(3, 5), "both set: the generator's own, written by the engine");
	Check(ParkedColourRow(0xFFFFFFFFu, 0) == 0, "a model with no colours keeps the engine's 0/0");
	bool inRange = true;
	for (uint8_t n = 1; n <= 8; ++n)
		for (uint32_t g = 0; g < 64; ++g)
			inRange = inRange && ParkedColourRow(ParkedCarRoll(99, g), n) < n;
	Check(inRange, "the row is always inside the model's table");
}

void TestOneSeedOneSequence() {
	std::printf("\nrand() from the seed\n");
	uint32_t a = ParkedCarRoll(42, 3), b = a;
	bool     same = true;
	for (int i = 0; i < 16; ++i)
		same = same && CrtRand(a) == CrtRand(b);
	Check(same, "one state draws one sequence: the extras, the alarm and the lock agree");
	uint32_t s = 1;
	Check(CrtRand(s) == 16838 && CrtRand(s) == 5758 && CrtRand(s) == 10113,
	      "the draw is this CRT's own, the K&R sequence 16838, 5758, 10113 from a state of 1");
}

bool     g_seamValid = false;
uint32_t g_seamSeed  = 0;
uint32_t g_seamCalls = 0;
void RecSetParkedSeed(bool valid, uint32_t seed) {
	g_seamValid = valid;
	g_seamSeed  = seed;
	++g_seamCalls;
}

void TestTheSeedReachesTheSeam() {
	std::printf("\nthe seed off the wire\n");
	Client      c;
	WorldBridge b;
	b.SetParkedSeed = &RecSetParkedSeed;
	c.SetBridge(b);
	uint32_t seed = 0;
	Check(!c.ParkedSeed(seed), "nothing until the server says so");
	c.PreFrame();
	Check(g_seamCalls == 1 && !g_seamValid, "and the engine rolls its own meanwhile");

	S_ParkedSeed pkt;
	InitHeader(pkt, 0);
	pkt.seed = 0xDEADBEEFu;
	Message m;
	m.opcode  = pkt.OPCODE;
	m.channel = CH_EVENT;
	m.data.assign(reinterpret_cast<const uint8_t *>(&pkt),
	              reinterpret_cast<const uint8_t *>(&pkt) + sizeof pkt);
	c.HandleMessage(m);
	Check(c.ParkedSeed(seed) && seed == 0xDEADBEEFu, "the session's seed, as sent");
	c.PreFrame();
	Check(g_seamValid && g_seamSeed == 0xDEADBEEFu,
	      "handed to the engine before the frame the generators run in");

	c.ClearRosterForTest();
	Check(!c.ParkedSeed(seed), "and forgotten with the session");
	c.PreFrame();
	Check(!g_seamValid, "which gives the engine its own rolls back");
}

// ---- against the real exe ------------------------------------------------------

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

uint8_t Byte(const std::vector<uint8_t> &img, uint32_t va) { return img[va - IMAGE_BASE]; }

uint32_t Dword(const std::vector<uint8_t> &img, uint32_t va) {
	const size_t o = va - IMAGE_BASE;
	return uint32_t(img[o]) | uint32_t(img[o + 1]) << 8 | uint32_t(img[o + 2]) << 16 |
	       uint32_t(img[o + 3]) << 24;
}

bool Bytes(const std::vector<uint8_t> &img, uint32_t va, std::initializer_list<uint8_t> want) {
	uint32_t at = va;
	for (uint8_t b : want)
		if (Byte(img, at++) != b)
			return false;
	return true;
}

bool CallsTo(const std::vector<uint8_t> &img, uint32_t site, uint32_t to) {
	return Byte(img, site) == 0xE8 && site + 5 + Dword(img, site + 1) == to;
}

void TestAgainstTheImage() {
	std::printf("\nthe car generators and rand(), against gta3.exe\n");
	std::vector<uint8_t> img;
	std::string          from;
	if (!LoadExe(img, from)) {
		std::printf("  [skipped] no retail gta3.exe; set COOPIII_GTA3_EXE to check the "
		            "generator's call and rand() against one\n");
		return;
	}
	std::printf("  reading %s\n", from.c_str());

	Check(CallsTo(img, CARGEN_INTERNAL_CALL, CCarGenerator__DoInternalProcessing) &&
	          Bytes(img, CARGEN_INTERNAL_CALL - 2, {0x89, 0xD9}) &&
	          Bytes(img, CCarGenerator__Process, {0x53, 0x89, 0xCB, 0x83, 0x7B, 0x24, 0xFF}),
	      "Process calls DoInternalProcessing on the generator, and only with no car");
	uint32_t calls = 0;
	for (uint32_t va = 0x00401000; va + 5 < 0x005F0000; ++va)
		if (CallsTo(img, va, CCarGenerator__DoInternalProcessing))
			++calls;
	Check(calls == 1, "and nothing else calls it");

	bool prologue = true;
	for (size_t i = 0; i < sizeof RAND_PROLOGUE; ++i)
		prologue = prologue && Byte(img, uint32_t(CGeneral__GetRandomNumber + i)) == RAND_PROLOGUE[i];
	Check(prologue && CallsTo(img, CGeneral__GetRandomNumber + 3, CRT_GET_THREAD_DATA),
	      "rand() keeps its state at +8 of the thread's CRT data");
	Check(Bytes(img, CGeneral__GetRandomNumber + 0x2E, {0xC1, 0xE8, 0x10, 0x25, 0xFF, 0x7F, 0x00, 0x00}),
	      "and draws the top fifteen bits, as CrtRand does");
	Check(Bytes(img, 0x005428D8, {0x66, 0x8B, 0x45, uint8_t(offs::CARGEN_COLOUR1)}) &&
	          Bytes(img, 0x005428E2, {0x66, 0x83, 0x7D, uint8_t(offs::CARGEN_COLOUR2), 0xFF}) &&
	          Bytes(img, 0x00542B36, {0x66, 0x8B, 0x45, uint8_t(offs::CARGEN_COLOUR1)}),
	      "both arms write the generator's own colours only when neither is -1");
	Check(Bytes(img, 0x00542711, {0x8B, 0x45, 0x00}) && Bytes(img, 0x005429B6, {0x8B, 0x7D, 0x00}),
	      "and take the model from the generator itself");
	Check(Bytes(img, CVehicleModelInfo__ChooseVehicleColour + 3, {0x8A, 0x83, 0xD4, 0x01, 0x00, 0x00}),
	      "the colour table's count is the model info's m_numColours");
}

} // namespace

int RunCarGenTests() {
	std::printf("\n");
	TestTheRollIsTheSessions();
	TestWhichColourIsRolled();
	TestOneSeedOneSequence();
	TestTheSeedReachesTheSeam();
	TestAgainstTheImage();
	return g_cargenFailures;
}
