// The side jobs' rules (docs/mission-audit.md 3, "Side jobs, step by step"):
// what a side job's delta takes of another save's progress (sideprogress.h),
// the odd jobs' given-up start (game/sidejob.h) and the safehouses' racks
// (game/pickup.h). With the retail main.scm at hand, each table is found in it.

#include "game/mission.h"
#include "game/pickup.h"
#include "game/sidejob.h"
#include "sideprogress.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace coopiii;
using namespace coopiii::game;

namespace {

int g_sideFailures = 0;

void Check(bool cond, const char *what) {
	std::printf("  [%s] %s\n", cond ? "ok" : "FAIL", what);
	if (!cond)
		++g_sideFailures;
}

bool LoadScm(std::vector<uint8_t> &scm) {
	std::vector<std::string> candidates;
	if (const char *env = std::getenv("COOPIII_MAIN_SCM"))
		candidates.push_back(env);
	candidates.push_back("reference/scm/Compiled SCM/main.scm");
	candidates.push_back("../../../../reference/scm/Compiled SCM/main.scm");
	candidates.push_back("D:/CoopIII/reference/scm/Compiled SCM/main.scm");
	for (const std::string &path : candidates) {
		FILE *fh = std::fopen(path.c_str(), "rb");
		if (!fh)
			continue;
		std::fseek(fh, 0, SEEK_END);
		const long size = std::ftell(fh);
		std::fseek(fh, 0, SEEK_SET);
		scm.resize(size > 0 ? size_t(size) : 0);
		const size_t got = scm.empty() ? 0 : std::fread(scm.data(), 1, scm.size(), fh);
		std::fclose(fh);
		if (got == scm.size() && scm.size() > 0x20000)
			return true;
	}
	return false;
}

uint32_t Le32(const std::vector<uint8_t> &b, size_t at) {
	return at + 4 <= b.size() ? uint32_t(b[at]) | uint32_t(b[at + 1]) << 8 | uint32_t(b[at + 2]) << 16 |
	                                uint32_t(b[at + 3]) << 24
	                          : 0;
}

bool Has(const std::vector<uint8_t> &b, uint32_t from, uint32_t to, const std::vector<uint8_t> &pat) {
	for (uint32_t p = from; p + pat.size() <= to; ++p)
		if (std::memcmp(b.data() + p, pat.data(), pat.size()) == 0)
			return true;
	return false;
}

std::vector<uint8_t> GlobalOp(uint16_t opcode, uint16_t at) {
	return {uint8_t(opcode & 0xFF), uint8_t(opcode >> 8), 0x02, uint8_t(at & 0xFF), uint8_t(at >> 8)};
}

void TestTheRules() {
	std::printf("\nwhat a side job's delta takes of another save\n");
	using namespace sideprogress;
	Check(ValueGoes(1078 * 4, 10, 30) && !ValueGoes(1078 * 4, 90, 3),
	      "the saved patients go up to the leader's, never back down");
	Check(!ValueGoes(1080 * 4, 2, 1) && ValueGoes(1080 * 4, 0, 1),
	      "a bribe's pager flag at two stays at two; at nought it takes one");
	Check(ValueGoes(386 * 4, 300, 212) && !ValueGoes(386 * 4, 180, 212),
	      "a best time goes when it is better, and a worse one is kept out");
	Check(ValueGoes(1234 * 4, 5, 0), "anything not in the table goes as it always did");
	Check(!ValueGoes(395 * 4, 7, 7), "and the same count is nothing to write");
	Check(IsSideJob(3) && IsSideJob(14) && !IsSideJob(2) && !IsSideJob(15) && !IsSideJob(75),
	      "the RC, 4x4, Mayhem runs and odd jobs are side jobs; Toyminator is the story's");
	const uint8_t progress[] = {0x0C, 0x03, 0x04, 0x01};
	const uint8_t passed[]   = {0x18, 0x03, 'R', 'C', '1', 0, 0, 0, 0, 0};
	const uint8_t gen[]      = {0x4C, 0x01, 0x02, 0x00, 0x00, 0x04, 0x65};
	Check(IsProgressOp(progress, sizeof progress) && IsProgressOp(passed, sizeof passed) &&
	          !IsProgressOp(gen, sizeof gen) && !IsProgressOp(progress, 1),
	      "PLAYER_MADE_PROGRESS and REGISTER_MISSION_PASSED are the progress, a car generator is not");
	size_t        n     = 0;
	const sideprogress::Global *t = Table(&n);
	bool          align = true;
	for (size_t i = 0; i < n; ++i)
		align = align && (t[i].offset & 3u) == 0 && Find(t[i].offset) == &t[i];
	Check(align, "every global in the table is a whole word, and in it once");
}

void TestTheGivenUpStart() {
	std::printf("\nan odd job's start given up\n");
	const uint8_t taxi[] = {0x04, 0x00, 0x02, 0x34, 0x06, 0x04, 0x01, 0x02, 0x00};
	Check(GivenUpSequelLength(taxi, sizeof taxi, 0, 14) == 7,
	      "the taxi's $ON_TAXI_MISSION = 1 after START_MISSION is skipped with it");
	Check(GivenUpSequelLength(taxi, sizeof taxi, 0, 7) == 0, "not a 4x4 run's, whose trigger has none");
	Check(GivenUpSequelLength(taxi, sizeof taxi, 2, 14) == 0, "nor anything that is not that instruction");
	Check(GivenUpSequelLength(taxi, 6, 0, 14) == 0, "nor one cut short");
}

void TestTheRacks() {
	std::printf("\nthe safehouses' racks\n");
	Check(OnHideoutRack(858.75f, -317.0625f, 10.0f), "Portland's flamethrower spot is on the rack");
	Check(OnHideoutRack(-673.0f, -28.0f, 18.25f) && OnHideoutRack(92.5625f, -472.5f, 15.5f),
	      "so are Shoreside's last bribe and Staunton's adrenaline");
	Check(!OnHideoutRack(858.75f, -319.0625f, 10.0f), "two metres off a spot is not");
	Check(!OnHideoutRack(858.75f, -317.0625f, 30.0f), "nor the same spot on another floor");
	size_t          n    = 0;
	const RackSpot *rack = HideoutRack(&n);
	float           near = 1e9f;
	for (size_t i = 0; i < n; ++i)
		for (size_t j = i + 1; j < n; ++j) {
			const float d = std::hypot(rack[i].x - rack[j].x, rack[i].y - rack[j].y);
			near          = d < near ? d : near;
		}
	Check(n == 60 && near >= 2.0f, "sixty spots, no two nearer than two metres");
}

int16_t Fixed(float v) { return static_cast<int16_t>(std::lround(v * 16.0f)); }

void TestAgainstMainScm() {
	std::printf("\nthe side jobs, against the retail main.scm\n");
	std::vector<uint8_t> scm;
	if (!LoadScm(scm)) {
		std::printf("  [skipped] no main.scm; set COOPIII_MAIN_SCM to check the tables against one\n");
		return;
	}
	Check(ScriptCodeHash(scm.data(), scripts::MAIN_SCRIPT_SIZE) == sideprogress::RETAIL_SCRIPT_HASH,
	      "its hash is the one the side jobs' rules are kept for");
	const uint32_t globalsEnd = GlobalsEnd(scm.data(), static_cast<uint32_t>(scm.size()));
	const uint32_t seg3       = Le32(scm, globalsEnd + 3);
	const uint32_t mainSize   = Le32(scm, seg3 + 8);
	const uint32_t size       = static_cast<uint32_t>(scm.size());
	const auto     from = [&](uint16_t m) { return Le32(scm, seg3 + 20 + 4 * m); };
	const auto     to   = [&](uint16_t m) { return m + 1 < 80 ? from(m + 1) : size; };

	// The best times are what the four runs register (03FD..0400), and the
	// RC records what RC1-4 save (042F); the counts are added to by their job.
	bool times = true;
	for (uint16_t k = 0; k < 4; ++k)
		times = times && Has(scm, from(7 + k), to(7 + k), GlobalOp(0x03FD + k, (386 + k) * 4));
	Check(times, "the four best times are what the 4x4 and Mayhem runs register");
	bool records = true;
	for (uint16_t k = 0; k < 4; ++k)
		records = records && Has(scm, from(3 + k), to(3 + k), {0x2F, 0x04}) &&
		          Has(scm, from(3 + k), to(3 + k), GlobalOp(0x0084, (402 + k) * 4));
	Check(records, "the four RC records are what RC1-4 copy their kills into");
	Check(Has(scm, from(14), to(14), GlobalOp(0x0008, 395 * 4)) &&
	          Has(scm, from(11), to(11), GlobalOp(0x0058, 1078 * 4)) &&
	          Has(scm, from(12), to(12), GlobalOp(0x0008, 1083 * 4)) &&
	          Has(scm, from(13), to(13), GlobalOp(0x0008, 1077 * 4)),
	      "the fares, the saved patients, the fires and the criminals are counted up by their job");
	Check(Has(scm, globalsEnd, mainSize, GlobalOp(0x0018, 1078 * 4)) &&
	          Has(scm, globalsEnd, mainSize, GlobalOp(0x0038, 1055 * 4)),
	      "and rewards.sc reads the patients and the flamethrower");

	// Every progress point a side job pays is beside one of its reward
	// flags, but the Paramedic's two for the last level, which has none.
	size_t points = 0, flagged = 0, bare11 = 0, bareElse = 0;
	for (uint16_t m = 3; m <= 14; ++m)
		for (uint32_t p = from(m); p + 4 <= to(m); ++p) {
			if (!(scm[p] == 0x0C && scm[p + 1] == 0x03 && scm[p + 2] == 0x04 && scm[p + 3] == 0x01))
				continue;
			++points;
			bool near = false;
			for (uint32_t q = p - 16; q <= p + 16 && q + 5 <= size; ++q) {
				if (scm[q] != 0x04 || scm[q + 1] != 0x00 || scm[q + 2] != 0x02)
					continue;
				const sideprogress::Global *g =
				    sideprogress::Find(static_cast<uint16_t>(scm[q + 3] | scm[q + 4] << 8));
				near = near || (g && g->reward);
			}
			if (near)
				++flagged;
			else if (m == 11)
				++bare11;
			else
				++bareElse;
		}
	Check(points >= 20 && bareElse == 0 && bare11 == 2,
	      "every side job's progress point is set beside a reward flag of the table, but the "
	      "Paramedic's last level");
	std::printf("    (%zu progress points, %zu beside a reward flag)\n", points, flagged);

	// Every odd job's START_MISSION is followed by the flag the trigger
	// sets, the taxi's $ON_TAXI_MISSION.
	size_t starts = 0, sequels = 0;
	bool   taxi   = false;
	for (uint32_t p = globalsEnd; p + 11 <= mainSize; ++p) {
		if (scm[p] != 0x17 || scm[p + 1] != 0x04 || scm[p + 2] != 0x04 || scm[p + 3] < 11 ||
		    scm[p + 3] > 14)
			continue;
		++starts;
		if (GivenUpSequelLength(scm.data(), mainSize, p + 4, scm[p + 3]) == 7) {
			++sequels;
			if (scm[p + 3] == 14)
				taxi = taxi || (scm[p + 7] | scm[p + 8] << 8) == 397 * 4;
		}
	}
	Check(starts == 8 && sequels == 8 && taxi,
	      "the eight odd-job starts each set a flag straight after, the taxi's $ON_TAXI_MISSION");

	// Every rack spot is a pickup rewards.sc makes there.
	size_t          n = 0, found = 0;
	const RackSpot *rack = HideoutRack(&n);
	for (size_t i = 0; i < n; ++i) {
		const int16_t x = Fixed(rack[i].x), y = Fixed(rack[i].y), z = Fixed(rack[i].z);
		const std::vector<uint8_t> pat = {0x06, uint8_t(x & 0xFF), uint8_t(uint16_t(x) >> 8),
		                                  0x06, uint8_t(y & 0xFF), uint8_t(uint16_t(y) >> 8),
		                                  0x06, uint8_t(z & 0xFF), uint8_t(uint16_t(z) >> 8)};
		if (Has(scm, globalsEnd, mainSize, pat))
			++found;
		else
			std::printf("    no pickup at %.4f %.4f %.4f\n", rack[i].x, rack[i].y, rack[i].z);
	}
	Check(found == n, "every rack spot is a pickup the main script lays out there");
}

} // namespace

int RunSideJobTests() {
	TestTheRules();
	TestTheGivenUpStart();
	TestTheRacks();
	TestAgainstMainScm();
	return g_sideFailures;
}
