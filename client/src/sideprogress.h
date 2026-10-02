// What a side job leaves in the campaign, and how much of it another save takes
// (docs/mission-audit.md 3, "Side jobs, step by step").
//
// The odd jobs, the RC runs, the 4x4 runs and Multistorey Mayhem are anybody's
// to start, so their delta comes from whichever save the starter brought. Its
// values are that save's: Taxi Driver's fare count, the Paramedic's saved
// patients, the fires and criminals of each island, the RC records, the best
// times. Written as they came, a guest's three fares put the host's count of
// ninety back to three, a guest's first bribe put the host's pager flag from
// two back to one, and the host's own Vigilante then paged the bribe and paid
// its progress point a second time.
//
// So on a machine that applies a side job's delta:
//
//   - a value of the table below never takes this game's progress back: a
//     count or a flag is written only when it is higher than ours, a best
//     time only when it is lower. Whoever is furthest along leads, and every
//     save follows them; nobody's count goes down;
//   - the job's PLAYER_MADE_PROGRESS and REGISTER_MISSION_PASSED (the delta's
//     own parts, replay.h Kind::Campaign) count here only when one of the
//     job's reward flags in the delta goes up here, the flag the job sets
//     beside the progress point. A save that already had the flamethrower,
//     the bribe or the Borgnine taxi is not given the point again, nor is
//     an RC run's first pass counted twice;
//   - every other value goes as it always did.
//
// The table is retail main.scm's (Sanny's CustomVariables, index times four),
// checked against it by tools/clienttest. A main.scm with another hash keeps
// the old rule for everything.
#pragma once

#include <cstddef>
#include <cstdint>

namespace coopiii::sideprogress {

// ScriptCodeHash (game/mission.h) of the retail main.scm.
constexpr uint32_t RETAIL_SCRIPT_HASH = 0x49D0F4C5u;

enum class Rule : uint8_t {
	Higher,   // a count, a record of kills, a flag that only goes up
	Lower,    // a best time
};

struct Global {
	uint16_t    offset;
	Rule        rule;
	bool        reward;   // set beside a progress point: the job's unlock
	const char *name;
};

inline const Global *Table(size_t *count) {
	static const Global kTable[] = {
	    {377 * 4, Rule::Higher, false, "GOT_SIREN_HELP_BEFORE"},
	    {378 * 4, Rule::Higher, true, "PATRIOT_PLAYGROUND_COMPLETED"},
	    {379 * 4, Rule::Higher, true, "A_RIDE_IN_THE_PARK_COMPLETED"},
	    {380 * 4, Rule::Higher, true, "GRIPPED_COMPLETED"},
	    {381 * 4, Rule::Higher, true, "MULTISTOREY_MAYHEM_COMPLETED"},
	    {386 * 4, Rule::Lower, false, "PATRIOT_PLAYGROUND_BEST_TIME"},
	    {387 * 4, Rule::Lower, false, "A_RIDE_IN_THE_PARK_BEST_TIME"},
	    {388 * 4, Rule::Lower, false, "GRIPPED_BEST_TIME"},
	    {389 * 4, Rule::Lower, false, "MULTISTOREY_MAYHEM_BEST_TIME"},
	    {395 * 4, Rule::Higher, false, "TAXI_MISSION_DELIVERIES"},
	    {399 * 4, Rule::Higher, true, "NEW_TAXI_CREATED_BEFORE"},
	    {401 * 4, Rule::Higher, false, "DISPLAYED_TAXI_HELP_MESSAGE"},
	    {402 * 4, Rule::Higher, false, "RC1_RECORD"},
	    {403 * 4, Rule::Higher, false, "RC2_RECORD"},
	    {404 * 4, Rule::Higher, false, "RC3_RECORD"},
	    {405 * 4, Rule::Higher, false, "RC4_RECORD"},
	    {409 * 4, Rule::Higher, true, "DIABLO_DESTRUCTION_COMPLETED"},
	    {410 * 4, Rule::Higher, true, "MAFIA_MASSACRE_COMPLETED"},
	    {411 * 4, Rule::Higher, true, "RUMPO_RAMPAGE_COMPLETED"},
	    {412 * 4, Rule::Higher, true, "CASINO_CALAMITY_COMPLETED"},
	    {1055 * 4, Rule::Higher, true, "EARNED_FREE_FLAMETHROWER"},
	    {1075 * 4, Rule::Higher, false, "IND_COPCAR_KILLS"},
	    {1076 * 4, Rule::Higher, false, "COM_COPCAR_KILLS"},
	    {1077 * 4, Rule::Higher, false, "SUB_COPCAR_KILLS"},
	    {1078 * 4, Rule::Higher, false, "TOTAL_SAVED_PEDS"},
	    {1079 * 4, Rule::Higher, true, "AMBULANCE_PAGER_FLAG"},
	    {1080 * 4, Rule::Higher, true, "PLAY_PAGER_MESSAGE1"},
	    {1081 * 4, Rule::Higher, true, "PLAY_PAGER_MESSAGE2"},
	    {1082 * 4, Rule::Higher, true, "PLAY_PAGER_MESSAGE3"},
	    {1083 * 4, Rule::Higher, false, "IND_FIRES_EXTING"},
	    {1084 * 4, Rule::Higher, false, "COM_FIRES_EXTING"},
	    {1085 * 4, Rule::Higher, false, "SUB_FIRES_EXTING"},
	};
	*count = sizeof kTable / sizeof kTable[0];
	return kTable;
}

inline const Global *Find(uint16_t offset) {
	size_t        n = 0;
	const Global *t = Table(&n);
	for (size_t i = 0; i < n; ++i)
		if (t[i].offset == offset)
			return &t[i];
	return nullptr;
}

// The RC runs (3-6), the 4x4 runs and Mayhem (7-10) and the odd jobs (11-14).
inline bool IsSideJob(uint16_t missionNumber) { return missionNumber >= 3 && missionNumber <= 14; }

// Whether a game holding `here` takes `value`.
inline bool Takes(Rule rule, int32_t here, int32_t value) {
	return rule == Rule::Lower ? value < here : value > here;
}

// Whether `value` for `offset` goes into a game that holds `here`: a value of
// the table only when it moves this game on, anything else as ever.
inline bool ValueGoes(uint16_t offset, int32_t here, int32_t value) {
	const Global *g = Find(offset);
	return g == nullptr || Takes(g->rule, here, value);
}

// One of the job's progress instructions: PLAYER_MADE_PROGRESS (030C) or
// REGISTER_MISSION_PASSED (0318), as a delta's part carries them.
inline bool IsProgressOp(const uint8_t *op, size_t length) {
	return length >= 2 && op[1] == 0x03 && (op[0] == 0x0C || op[0] == 0x18);
}

} // namespace coopiii::sideprogress
