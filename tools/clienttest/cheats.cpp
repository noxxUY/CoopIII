// Cheats in a session: the pure half of game/cheats.h, the routing through
// Client, the shared wreck queue BANGBANGBANG overflowed, and - when a copy of
// the retail exe is handed over - the cheat table read back out of it.
//
// Nothing here types a key into a game. What runs is every decision in front
// of the engine: which cheat a buffer completes, where each one is allowed to
// run, what a receiver calls to arrive where the typist's machine was left,
// and that the car refusals BANGBANGBANG runs into are decided on the car.

#include "chatfeed.h"
#include "client.h"
#include "game/cheats.h"
#include "game/nametag.h"
#include "game/tpto.h"
#include "game/vehicle.h"
#include "game/wreckqueue.h"

#include <coopiii/protocol.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace coopiii;
using namespace coopiii::game;

namespace {

int g_cheatFailures = 0;

void Check(bool cond, const char *what) {
	std::printf("  [%s] %s\n", cond ? "ok" : "FAIL", what);
	if (!cond)
		++g_cheatFailures;
}

// What the player types: the table holds it backwards, the way it is compared.
std::string Forward(uint8_t id) {
	const std::string reversed(CHEAT_SITES[id].reversed);
	return std::string(reversed.rbegin(), reversed.rend());
}

// A buffer the way the game starts with it: zeros, from .bss. Nothing ever
// fills it with anything else, which is the whole of why BOOOOORING works
// only as the first thing typed (addresses.h, CHEAT_SITES).
struct Keyboard {
	char buffer[KEYBOARD_CHEAT_STRING_LEN];
	Keyboard() { std::memset(buffer, 0, sizeof(buffer)); }

	// Types one key, and returns what it fired.
	std::vector<uint8_t> Key(char c) {
		PushCheatChar(buffer, c);
		uint8_t       ids[CHEAT_COUNT];
		const uint8_t n = MatchTypedCheats(buffer, ids, CHEAT_COUNT);
		return std::vector<uint8_t>(ids, ids + n);
	}
};

// ---- the table ---------------------------------------------------------------

void TestTheTableIsWhatTheEngineCompares() {
	std::printf("\nthe cheat table\n");

	bool lengths = true, unique = true, bounded = true;
	for (uint8_t a = 0; a < CHEAT_COUNT; ++a) {
		const CheatSite &s = CHEAT_SITES[a];
		const size_t     n = std::strlen(s.reversed);
		if (a == CHEAT_SLOW_TIME ? !(n == 10 && s.length == 16) : n != s.length)
			lengths = false;
		if (s.length > KEYBOARD_CHEAT_STRING_LEN)
			bounded = false;
		for (uint8_t b = a + 1; b < CHEAT_COUNT; ++b)
			if (s.handler == CHEAT_SITES[b].handler ||
			    s.string == CHEAT_SITES[b].string ||
			    std::strcmp(s.reversed, CHEAT_SITES[b].reversed) == 0)
				unique = false;
	}
	Check(lengths, "every row compares its own length, except BOOOOORING's 16 for ten letters");
	Check(bounded, "and fits the twenty-byte buffer");
	Check(unique, "no two rows share a string, an address or a handler");

	Check(Forward(CHEAT_WANTED_UP) == "MOREPOLICEPLEASE",
	      "row 3 is what the user types for two more stars");
	Check(Forward(CHEAT_ARMOUR) == "TURTOISE", "the armour row is the 1.0 spelling");
	Check(Forward(CHEAT_BLOW_UP_CARS) == "BANGBANGBANG", "row 6 blows up the cars");
	Check(Forward(CHEAT_FOGGY) == "PEASOUP", "row 17 is fog");
	Check(Forward(CHEAT_NASTY_LIMBS) == "NASTYLIMBSCHEAT", "and the last is the one that does nothing");
}

// ---- the matcher -------------------------------------------------------------

void TestEachCheatFiresOnItsLastKeyAndOnlyThen() {
	std::printf("\ntyping every cheat\n");
	int wrong = 0;
	for (uint8_t id = 0; id < CHEAT_COUNT; ++id) {
		Keyboard          kb;
		const std::string word = Forward(id);
		for (size_t i = 0; i < word.size(); ++i) {
			const std::vector<uint8_t> fired = kb.Key(word[i]);
			const bool last = i + 1 == word.size();
			if (last ? (fired.size() != 1 || fired[0] != id) : !fired.empty()) {
				++wrong;
				std::printf("    %s: key %zu fired %zu cheat(s)\n", word.c_str(), i,
				            fired.size());
			}
		}
	}
	Check(wrong == 0, "all 23 fire exactly once, on the last key, as themselves");
}

// The function is 23 separate ifs, so a cheat whose spelling ends another's
// would fire both. None does, and this is what says so rather than an
// assumption: every cheat typed straight after every other, with no pause.
//
// Two cheats do fire more than twice when typed twice, and only as
// themselves: GUNSGUNSGUNS and BANGBANGBANG repeat every four letters, so
// every further GUNS or BANG completes them again. That is the engine's own
// behaviour - the same buffer, the same compare - and single player has it.
void TestNoCheatFiresAnotherOnTheWay() {
	std::printf("\nevery cheat straight after every other\n");
	int wrong = 0;
	for (uint8_t a = 0; a < CHEAT_COUNT; ++a) {
		for (uint8_t b = 0; b < CHEAT_COUNT; ++b) {
			Keyboard          kb;
			const std::string both = Forward(a) + Forward(b);
			std::vector<uint8_t> all;
			for (char c : both) {
				const std::vector<uint8_t> f = kb.Key(c);
				all.insert(all.end(), f.begin(), f.end());
			}
			// What the engine would fire: A, then B - unless B is BOOOOORING,
			// which A's last letter now sits in the way of - and for the two
			// periodic cheats typed twice, the two extra completions.
			std::vector<uint8_t> want{a};
			if (b != CHEAT_SLOW_TIME)
				want.push_back(b);
			if (a == b && (a == CHEAT_WEAPONS || a == CHEAT_BLOW_UP_CARS))
				want.insert(want.end(), {a, a});
			const bool right = all == want;
			if (!right) {
				++wrong;
				std::printf("    %s then %s fired %zu:", Forward(a).c_str(),
				            Forward(b).c_str(), all.size());
				for (uint8_t id : all)
					std::printf(" %s", Forward(id).c_str());
				std::printf("\n");
			}
		}
	}
	Check(wrong == 0, "529 pairs: nothing ever fires a cheat that was not typed");

	Keyboard kb;
	int      fired = 0;
	for (char c : std::string("BANGBANGBANGBANGBANG"))
		fired += int(kb.Key(c).size());
	Check(fired == 3, "BANGBANGBANG then BANG, BANG fires three times, as in single player");
}

// ---- the chat key inside a cheat --------------------------------------------

// The chat window's key handling (game/chat.cpp) in front of the engine's
// buffer: T opens the line unless it finishes a cheat, keys typed into an open
// line reach the engine only when the line is the rest of a cheat, and Enter
// sends. Letters arrive in the line in lower case, the way ToUnicode gives
// them without Shift.
struct ChatKeyboard {
	// The game's cheats as the running code compares them (cheats.h,
	// TypedCheatTable), retail's unless a test hands another plugin's.
	TypedCheatTable          table = TableFromSites();
	char                     buffer[KEYBOARD_CHEAT_STRING_LEN] = {};
	bool                     open = false;
	std::string              line;
	std::vector<uint8_t>     fired;   // the game's, by CheatId
	std::vector<std::string> coop;    // CoopIII's own, as typed: "TPTO3"
	std::vector<std::string> sent;

	ChatKeyboard() = default;
	explicit ChatKeyboard(const TypedCheatTable &t) : table(t) {}

	// A key the engine pushes, and what the buffer then completes: the game's
	// rows fire, and ours are read off the buffer as NoticeTypedKeys does.
	void Engine(char c) {
		PushCheatChar(buffer, c);
		const TypedMatch m = MatchTyped(table, buffer);
		fired.insert(fired.end(), m.engine, m.engine + m.engineCount);
		if (m.hasCoop)
			coop.push_back(std::string(COOP_CHEATS[m.coop.id].word) + m.coop.arg);
	}

	void Key(char c) {
		if (!open) {
			if (c == 'T' && !ChatKeyFinishesCheat(table, buffer, 'T')) {
				open = true;
				line.clear();
				return;
			}
			Engine(c);
			return;
		}
		line += (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
		char    keys[KEYBOARD_CHEAT_STRING_LEN];
		uint8_t count = 0;
		if (CheatFinishedInChatLine(table, buffer, 'T', line.c_str(), keys, &count)) {
			for (uint8_t i = 0; i < count; ++i)
				Engine(keys[i]);
			open = false;
			line.clear();
		}
	}

	void Type(const std::string &s) {
		for (char c : s)
			Key(c);
	}

	void Enter() {
		if (open)
			sent.push_back(line);
		open = false;
		line.clear();
	}
};

void TestEveryCheatCanBeTypedPastTheChatKey() {
	std::printf("\ntyping every cheat with T as the chat key\n");

	int withT = 0;
	for (uint8_t id = 0; id < CHEAT_COUNT; ++id)
		if (Forward(id).find('T') != std::string::npos)
			++withT;
	Check(withT == 11, "eleven cheats have a T in them, which the chat key used to eat");

	// From a fresh buffer, and after some walking and a jump: the buffer is
	// every key the engine saw, movement included. BOOOOORING only ever works
	// first (the retail bug, above), so it is left out of the second.
	int wrong = 0;
	for (int walked = 0; walked < 2; ++walked) {
		for (uint8_t id = 0; id < CHEAT_COUNT; ++id) {
			if (walked && id == CHEAT_SLOW_TIME)
				continue;
			ChatKeyboard kb;
			if (walked)
				kb.Type("WWWWASDDW ");
			kb.Type(Forward(id));
			if (kb.fired != std::vector<uint8_t>{id} || kb.open || !kb.sent.empty()) {
				++wrong;
				std::printf("    %s%s: fired %zu, line %s\n", walked ? "after walking, " : "",
				            Forward(id).c_str(), kb.fired.size(), kb.open ? "open" : "shut");
			}
		}
	}
	Check(wrong == 0, "all 23 fire once, as themselves, with the line shut and nothing sent");

	// The engine's own pairs test, through the chat key: whatever the engine
	// fires for two cheats in a row, the chat key changes none of it.
	int pairs = 0;
	for (uint8_t a = 0; a < CHEAT_COUNT; ++a)
		for (uint8_t b = 0; b < CHEAT_COUNT; ++b) {
			Keyboard     plain;
			ChatKeyboard chat;
			std::vector<uint8_t> want;
			for (char c : Forward(a) + Forward(b)) {
				const std::vector<uint8_t> f = plain.Key(c);
				want.insert(want.end(), f.begin(), f.end());
			}
			chat.Type(Forward(a) + Forward(b));
			if (chat.fired != want || !chat.sent.empty())
				++pairs;
		}
	Check(pairs == 0, "529 pairs fire what they fire without a chat key, and send nothing");

	ChatKeyboard gesundheit;
	gesundheit.Type("GESUNDHEI");
	Check(ChatKeyFinishesCheat(gesundheit.table, gesundheit.buffer, 'T'),
	      "the T after GESUNDHEI finishes a cheat, so it goes to the game");
	ChatKeyboard fresh;
	Check(!ChatKeyFinishesCheat(fresh.table, fresh.buffer, 'T'),
	      "a T on its own finishes nothing and opens the line");
	Check(!ChatKeyFinishesCheat(gesundheit.table, gesundheit.buffer, '\0'),
	      "a chat key the engine pushes nothing for never finishes a cheat");

	ChatKeyboard scotland;
	scotland.Type("ILOVESCOTLAND");
	Check(scotland.fired == std::vector<uint8_t>{CHEAT_RAINY},
	      "LAND after ILOVESCOT is rain, not ILIKESCOTLAND's cloud: what came before T decides");

	ChatKeyboard tank;
	tank.Type("SDT");
	tank.Type("ANK");
	Check(tank.fired.empty() && tank.open,
	      "\"ank\" is only the rest of GIVEUSATANK after GIVEUSA; after anything else it is chat");
}

void TestChatTextNeverStartsACheat() {
	std::printf("\nchat text never starts a cheat\n");

	// Every cheat as a whole line, and inside a sentence, after the chat key
	// opened it - from a fresh buffer and from one that ends in the cheat's
	// own first letters, which is the nearest a line can come to one.
	int fired = 0, lost = 0;
	for (uint8_t id = 0; id < CHEAT_COUNT; ++id) {
		const std::string word  = Forward(id);
		const std::string lines[] = {word, "i said " + word, word + " again",
		                             "well " + word + " then"};
		for (const std::string &text : lines) {
			ChatKeyboard kb;
			kb.Key('T');
			kb.Type(text);
			kb.Enter();
			if (!kb.fired.empty())
				++fired;
			if (kb.sent.size() != 1)
				++lost;
		}
	}
	Check(fired == 0, "no cheat word fires from inside a chat line, whole or mid-sentence");
	Check(lost == 0, "and every one of those lines is sent as chat");

	ChatKeyboard hello;
	hello.Key('T');
	hello.Type("hello there, turtoise fans");
	hello.Enter();
	Check(hello.fired.empty() && hello.sent == std::vector<std::string>{"hello there, turtoise fans"},
	      "an ordinary line with Ts in it goes out whole");

	ChatKeyboard urtoise;
	urtoise.Key('T');
	urtoise.Type("URTOISE ");
	Check(urtoise.fired == std::vector<uint8_t>{CHEAT_ARMOUR} && !urtoise.open,
	      "TURTOISE finishes on its E, before anything after it can join the line");

	const TypedCheatTable retail = TableFromSites();
	char                  keys[KEYBOARD_CHEAT_STRING_LEN];
	uint8_t               count = 0;
	char                  buffer[KEYBOARD_CHEAT_STRING_LEN] = {};
	TypedMatch            what;
	Check(CheatFinishedInChatLine(retail, buffer, 'T', "urtoise", keys, &count, &what) &&
	          what.engineCount == 1 && what.engine[0] == CHEAT_ARMOUR && !what.hasCoop &&
	          count == 8 && std::memcmp(keys, "TURTOISE", 8) == 0,
	      "the keys handed to the game are the chat key and the line, in capitals");
	Check(!CheatFinishedInChatLine(retail, buffer, 'T', "Ur ToIse", keys, &count) && count == 0,
	      "a space is not a letter of any cheat");
	Check(!CheatFinishedInChatLine(retail, buffer, 'T', "", keys, &count),
	      "an empty line is nothing");
	Check(!CheatFinishedInChatLine(retail, buffer, '\0', "urtoise", keys, &count),
	      "and a chat key the engine never sees rescues nothing");
	Check(!CheatFinishedInChatLine(retail, buffer, 'T', "urtoiseurtoiseurtoise", keys, &count),
	      "a line longer than the buffer is never a cheat");
}

// ---- what the running game compares, not what retail does ----------------------------

// SilentPatch III's two rows, as read out of a running game with it in
// (2026-09-30, both processes): the armour row's `push` points at its own
// "ESIOTROT" and BOOOOORING's length is 0Ah. Everything else is retail's.
TypedCheatTable SilentPatchTable() {
	TypedCheatTable t = TableFromSites();
	std::memset(t.rows[CHEAT_ARMOUR].reversed, 0, sizeof t.rows[CHEAT_ARMOUR].reversed);
	std::memcpy(t.rows[CHEAT_ARMOUR].reversed, "ESIOTROT", 8);
	t.rows[CHEAT_ARMOUR].length = 8;
	t.rows[CHEAT_SLOW_TIME].length = 10;
	return t;
}

// Why typing armour past the chat key did nothing in noxx's game: the rule
// went by retail's table, which has TURTOISE, while the game with SilentPatch
// in only answers to TORTOISE. Read off the running code, each game's own
// spelling is the one that is rescued.
void TestTheChatKeyGoesByTheRunningGamesTable() {
	std::printf("\nthe chat key goes by the table the running game has\n");

	ChatKeyboard retailOnly;   // the rule on retail's table, the game SilentPatched
	retailOnly.table = TableFromSites();
	retailOnly.Type("TORTOISE");
	Check(retailOnly.fired.empty() && retailOnly.open,
	      "with retail's table, TORTOISE is chat: the line stays open and nothing fires "
	      "(what noxx saw)");

	ChatKeyboard patched(SilentPatchTable());
	patched.Type("TORTOISE");
	Check(patched.fired == std::vector<uint8_t>{CHEAT_ARMOUR} && !patched.open &&
	          patched.sent.empty(),
	      "with SilentPatch's table, TORTOISE is armour, fired on its E with the line shut");
	ChatKeyboard patchedOld(SilentPatchTable());
	patchedOld.Type("TURTOISE");
	Check(patchedOld.fired.empty() && patchedOld.open,
	      "and TURTOISE, which that game no longer has, stays chat");

	ChatKeyboard slow(SilentPatchTable());
	slow.Type("WWWW");
	slow.Type("BOOOOORING");
	Check(slow.fired == std::vector<uint8_t>{CHEAT_SLOW_TIME},
	      "SilentPatch's BOOOOORING works after other keys, as its ten says");

	int wrong = 0;
	for (uint8_t id = 0; id < CHEAT_COUNT; ++id) {
		if (id == CHEAT_SLOW_TIME)
			continue;
		const TypedCheatTable t = SilentPatchTable();
		const TypedRow       &r = t.rows[id];
		std::string           word;
		for (int i = RowTypedLength(r) - 1; i >= 0; --i)
			word += r.reversed[i];
		ChatKeyboard kb(t);
		kb.Type("WWASD");
		kb.Type(word);
		if (kb.fired != std::vector<uint8_t>{id} || kb.open || !kb.sent.empty())
			++wrong;
	}
	Check(wrong == 0, "every row of that game fires as itself past the chat key");
}

// The cases noxx named: straight through, no Enter.
void TestTheCheatsNoxxTypes() {
	std::printf("\nTURTOISE, ITSALLGOINGMAAAD, NOBODYLIKESME and TPTO3, typed straight through\n");

	ChatKeyboard turtoise;
	turtoise.Type("WWW");
	turtoise.Type("TURTOISE");
	Check(turtoise.fired == std::vector<uint8_t>{CHEAT_ARMOUR} && !turtoise.open &&
	          turtoise.sent.empty(),
	      "TURTOISE: T opens the line, E shuts it and the armour fires, nothing sent");

	ChatKeyboard mad;
	mad.Type("ITSALLGOINGMAAAD");
	Check(mad.fired == std::vector<uint8_t>{CHEAT_MAYHEM} && !mad.open && mad.sent.empty(),
	      "ITSALLGOINGMAAAD: I goes to the game, T opens the line, D finishes it");

	ChatKeyboard nobody;
	nobody.Type("NOBODYLIKESME");
	Check(nobody.fired == std::vector<uint8_t>{CHEAT_EVERYBODY_ATTACKS} && !nobody.open,
	      "NOBODYLIKESME has no T and never meets the chat line");

	ChatKeyboard tpto;
	tpto.Type("SD");
	tpto.Type("TPTO3");
	Check(tpto.coop == std::vector<std::string>{"TPTO3"} && tpto.fired.empty() && !tpto.open &&
	          tpto.sent.empty(),
	      "TPTO3: the line shuts on the 3, nothing is sent, and the buffer ends in TPTO3");

	ChatKeyboard hello;
	hello.Key('T');
	hello.Type("Thanks for the lift");
	hello.Enter();
	Check(hello.fired.empty() && hello.coop.empty() &&
	          hello.sent == std::vector<std::string>{"thanks for the lift"},
	      "a chat line that starts with T stays chat and goes out whole when Enter is pressed");

	ChatKeyboard about;
	about.Key('T');
	about.Type("tpto3 takes you to player 3");
	about.Enter();
	Check(about.coop.empty() && about.fired.empty() &&
	          about.sent == std::vector<std::string>{"tpto3 takes you to player 3"},
	      "and one about TPTO3 is chat too: the line's own T is not the cheat's first");

	ChatKeyboard tpt;
	tpt.Type("TPTO hi");
	tpt.Enter();
	Check(tpt.coop.empty() && tpt.sent == std::vector<std::string>{"pto hi"},
	      "\"TPTO hi\" is chat: a space is no player's number");

	ChatKeyboard nine;
	nine.Type("TPTO9");
	Check(nine.coop.empty() && nine.open, "TPTO9 is nobody's number on an eight-player list");
}

// CoopIII's table.
void TestOurOwnCheats() {
	std::printf("\nCoopIII's own cheats\n");

	char    buffer[KEYBOARD_CHEAT_STRING_LEN] = {};
	CoopCheatHit hit;
	for (char c : std::string("TPTO1"))
		PushCheatChar(buffer, c);
	Check(MatchCoopCheat(buffer, &hit) && hit.id == COOP_CHEAT_TPTO && hit.arg == '1',
	      "TPTO1 is TPTO with 1");
	for (char c : std::string("TPTO8"))
		PushCheatChar(buffer, c);
	Check(MatchCoopCheat(buffer, &hit) && hit.arg == '8', "TPTO8 with 8");
	for (char c : std::string("TPTO0"))
		PushCheatChar(buffer, c);
	Check(!MatchCoopCheat(buffer, &hit), "TPTO0 is nothing: the list starts at 1");
	for (char c : std::string("TPTO"))
		PushCheatChar(buffer, c);
	Check(!MatchCoopCheat(buffer, &hit), "and TPTO alone waits for its digit");
	Check(CoopCheatTypedLength(COOP_CHEAT_TPTO) == 5, "five keys");

	// No word of ours ends one of the game's or is ended by one, so one key
	// never fires both.
	bool apart = true;
	const TypedCheatTable t = TableFromSites();
	for (uint8_t c = 0; c < COOP_CHEAT_COUNT; ++c) {
		const std::string ours = COOP_CHEATS[c].word;
		for (uint8_t id = 0; id < CHEAT_COUNT; ++id) {
			std::string game;
			for (int i = RowTypedLength(t.rows[id]) - 1; i >= 0; --i)
				game += t.rows[id].reversed[i];
			const bool oursEndsGame =
			    game.size() >= ours.size() && game.compare(game.size() - ours.size(), ours.size(), ours) == 0;
			const bool gameEndsOurs =
			    ours.size() >= game.size() && ours.compare(ours.size() - game.size(), game.size(), game) == 0;
			if (oursEndsGame || gameEndsOurs)
				apart = false;
		}
	}
	Check(apart, "no word of ours ends one of the game's 23, nor the other way round");

	int wrong = 0;
	for (char d = '1'; d <= '8'; ++d) {
		ChatKeyboard kb;
		kb.Type("WWWW");
		kb.Type(std::string("TPTO") + d);
		if (kb.coop != std::vector<std::string>{std::string("TPTO") + d} || kb.open ||
		    !kb.fired.empty())
			++wrong;
	}
	Check(wrong == 0, "TPTO1 to TPTO8 each shut the line on the digit and come out as typed");

	ChatKeyboard twice;
	twice.Type("TPTO2");
	twice.Type("TPTO4");
	Check(twice.coop == (std::vector<std::string>{"TPTO2", "TPTO4"}),
	      "two in a row are two");

	ChatKeyboard shut;   // a chat key that is not T: nothing opens
	for (char c : std::string("TPTO5"))
		shut.Engine(c);
	Check(shut.coop == std::vector<std::string>{"TPTO5"},
	      "typed with the line shut, the buffer gives it all the same");
}

// Who TPTO goes to, and why it does not.
void TestTptoDecides() {
	std::printf("\nTPTO decides\n");

	Check(TptoTargetOf('1') == 0 && TptoTargetOf('8') == 7 && TptoTargetOf('9') == INVALID_PLAYER &&
	          TptoTargetOf('0') == INVALID_PLAYER,
	      "the digit is the Tab list's number, the slot plus one");
	Check(ListNumber(TptoTargetOf('3')) == 3 && PlayerIdFromListNumber(ListNumber(5)) == 5,
	      "the same number both ways");

	TptoFacts go;
	go.inSession    = true;
	go.localId      = 0;
	go.targetId     = 2;
	go.targetHere   = true;
	go.targetPlaced = true;
	go.self.havePed = true;
	Check(DecideTpto(go) == Tpto::OnFoot, "on foot, to somebody here: go");

	TptoFacts f = go;
	f.inSession = false;
	Check(DecideTpto(f) == Tpto::NotInSession, "no session: nothing");
	f      = go;
	f.rule = COOP_CHEATS_OFF;
	Check(DecideTpto(f) == Tpto::Off, "the server switched them off");
	f          = go;
	f.targetId = 0;
	Check(DecideTpto(f) == Tpto::Self, "yourself");
	f            = go;
	f.targetHere = false;
	Check(DecideTpto(f) == Tpto::NoSuchPlayer, "nobody in that slot");
	f          = go;
	f.targetId = INVALID_PLAYER;
	Check(DecideTpto(f) == Tpto::NoSuchPlayer, "or no slot at all");

	f              = go;
	f.self.havePed = false;
	Check(DecideTpto(f) == Tpto::NoPed, "no player in the world");
	f              = go;
	f.self.wbState = 2;
	Check(DecideTpto(f) == Tpto::Busted, "busted");
	f               = go;
	f.self.pedState = 56;
	Check(DecideTpto(f) == Tpto::Busted, "being arrested");
	f             = go;
	f.self.health = 0.0f;
	Check(DecideTpto(f) == Tpto::Wasted, "wasted");
	f               = go;
	f.self.cutscene = true;
	Check(DecideTpto(f) == Tpto::Cutscene, "a cutscene");

	f                = go;
	f.self.onMission = true;
	Check(DecideTpto(f) == Tpto::Mission, "a mission, by default");
	f.self.frenzyOngoing = true;
	Check(DecideTpto(f) == Tpto::Mission, "a rampage counts as one");
	f.rule = COOP_CHEATS_ALWAYS;
	Check(DecideTpto(f) == Tpto::OnFoot, "unless the server allows them always");

	f        = go;
	f.moving = true;
	Check(DecideTpto(f) == Tpto::Busy, "already on the way");
	f              = go;
	f.targetPlaced = false;
	Check(DecideTpto(f) == Tpto::NoPlace, "nothing says where he is");
	f            = go;
	f.islandOpen = false;
	Check(DecideTpto(f) == Tpto::IslandShut, "an island the story has not opened");
	f             = go;
	f.otherIsland = true;
	Check(DecideTpto(f) == Tpto::OnFoot, "another island on foot is the vote's move, which loads it");

	f            = go;
	f.inCar      = true;
	f.driver     = true;
	f.carMovable = true;
	Check(DecideTpto(f) == Tpto::InCar, "at the wheel of a car: the car comes along");
	f.otherIsland = true;
	Check(DecideTpto(f) == Tpto::IslandInCar, "but not onto another island");
	f.otherIsland = false;
	f.carMovable  = false;
	Check(DecideTpto(f) == Tpto::Vehicle, "nor in a boat, a plane or a train");
	f.driver = false;
	Check(DecideTpto(f) == Tpto::Passenger, "and a passenger gets out first");

	char line[FEED_MESSAGE];
	TptoMessage(Tpto::NoSuchPlayer, 5, nullptr, line, sizeof line);
	Check(std::strcmp(line, "TPTO: nobody is number 5") == 0, "\"TPTO: nobody is number 5\"");
	TptoMessage(Tpto::IslandInCar, 2, "bob", line, sizeof line);
	Check(std::strcmp(line, "TPTO: bob is on another island, leave the car first") == 0,
	      "the island in a car names him");
	TptoMessage(Tpto::OnFoot, 2, "bob", line, sizeof line);
	Check(line[0] == '\0', "and going says nothing in the feed: the game's own line says it");
	int unsaid = 0;
	for (uint8_t v = static_cast<uint8_t>(Tpto::Off); v <= static_cast<uint8_t>(Tpto::NoRoom); ++v) {
		TptoMessage(static_cast<Tpto>(v), 3, "bob", line, sizeof line);
		if (line[0] == '\0' || std::strlen(line) >= FEED_MESSAGE - 1)
			++unsaid;
	}
	Check(unsaid == 0, "every refusal has a line, and each fits the feed");

	float dx = 0.0f, dy = 0.0f;
	bool  ring = true;
	for (int a = 0; a < TPTO_CAR_TRIES; ++a) {
		TptoCarSpot(a, &dx, &dy);
		const float r = std::sqrt(dx * dx + dy * dy);
		if (std::fabs(r - TPTO_CAR_RADII_M[a < TPTO_CAR_DIRECTIONS ? 0 : 1]) > 0.01f)
			ring = false;
	}
	Check(ring, "the car ring is 7 m, then 10 m, eight ways each");
}

// The retail bug, kept. Sixteen compared for ten letters means the eleventh
// has to be the zero the buffer started with.
void TestBoooooringOnlyWorksAsTheFirstThingTyped() {
	std::printf("\nBOOOOORING\n");
	{
		Keyboard kb;
		int      fired = 0;
		for (char c : std::string("BOOOOORING"))
			fired += int(kb.Key(c).size());
		Check(fired == 1, "typed first, it fires");
	}
	{
		Keyboard kb;
		kb.Key('X');
		int fired = 0;
		for (char c : std::string("BOOOOORING"))
			fired += int(kb.Key(c).size());
		Check(fired == 0, "after one other key, never - which is 1.0 exactly");
	}
	{
		Keyboard kb;
		for (char c : std::string("PEASOUP"))
			kb.Key(c);
		int fired = 0;
		for (char c : std::string("BOOOOORING"))
			fired += int(kb.Key(c).size());
		Check(fired == 0, "after another cheat, never");
	}
}

void TestTheBufferKeepsTheLastTwentyNewestFirst() {
	std::printf("\nthe buffer\n");
	Keyboard kb;
	for (char c = 'A'; c <= 'Y'; ++c)   // 25 keys
		kb.Key(c);
	Check(kb.buffer[0] == 'Y' && kb.buffer[19] == 'F',
	      "twenty-five keys leave the last twenty, newest at [0]");
}

// ---- the classification --------------------------------------------------------

void TestWhatEachCheatTouches() {
	std::printf("\nwho each cheat belongs to\n");

	const uint8_t world[] = {CHEAT_BLOW_UP_CARS, CHEAT_MAYHEM, CHEAT_WEAPONS_FOR_ALL,
	                         CHEAT_FAST_TIME,    CHEAT_SLOW_TIME, CHEAT_SUNNY,
	                         CHEAT_CLOUDY,       CHEAT_RAINY,  CHEAT_FOGGY,
	                         CHEAT_FAST_WEATHER};
	int worldCount = 0;
	for (uint8_t id = 0; id < CHEAT_COUNT; ++id)
		if (CheatChangesTheWorld(id))
			++worldCount;
	bool listed = true;
	for (uint8_t id : world)
		if (!CheatChangesTheWorld(id))
			listed = false;
	Check(worldCount == 10 && listed, "exactly the ten world cheats change the world");
	Check(!CheatChangesTheWorld(CHEAT_EVERYBODY_ATTACKS),
	      "NOBODYLIKESME is the typist's: PLAYER1 is the local player everywhere");
	Check(!CheatChangesTheWorld(CHEAT_TANK) && !CheatChangesTheWorld(CHEAT_WANTED_UP),
	      "the tank and the stars are the typist's own");

	bool skies = true;
	for (uint8_t id = CHEAT_SUNNY; id <= CHEAT_FOGGY; ++id)
		if (CheatRouteOf(id) != CHEAT_ROUTE_HOST)
			skies = false;
	Check(skies, "the four skies go to the host");
	Check(CheatRouteOf(CHEAT_MAYHEM) == CHEAT_ROUTE_EVERYONE &&
	          CheatRouteOf(CHEAT_WEAPONS_FOR_ALL) == CHEAT_ROUTE_EVERYONE &&
	          CheatRouteOf(CHEAT_FAST_TIME) == CHEAT_ROUTE_EVERYONE &&
	          CheatRouteOf(CHEAT_SLOW_TIME) == CHEAT_ROUTE_EVERYONE &&
	          CheatRouteOf(CHEAT_FAST_WEATHER) == CHEAT_ROUTE_EVERYONE,
	      "the clock's speed, the riot and the armed crowd run everywhere");
	Check(CheatRouteOf(CHEAT_BLOW_UP_CARS) == CHEAT_ROUTE_LOCAL,
	      "BANGBANGBANG runs where it was typed; its wrecks are what travel");

	// Anything routed has to be a world cheat, or `personal` would let it
	// reach other machines.
	bool routedIsWorld = true;
	for (uint8_t id = 0; id < CHEAT_COUNT; ++id)
		if (CheatRouteOf(id) != CHEAT_ROUTE_LOCAL && !CheatChangesTheWorld(id))
			routedIsWorld = false;
	Check(routedIsWorld, "nothing personal is ever routed");
}

void TestTheRuleDecidesWhatIsAllowed() {
	std::printf("\nthe server's rule\n");
	int shared = 0, personal = 0, off = 0;
	for (uint8_t id = 0; id < CHEAT_COUNT; ++id) {
		shared += CheatAllowed(CHEAT_RULE_SHARED, id);
		personal += CheatAllowed(CHEAT_RULE_PERSONAL, id);
		off += CheatAllowed(CHEAT_RULE_OFF, id);
	}
	Check(shared == 23, "shared allows all 23");
	Check(personal == 13, "personal allows the 13 about the typist");
	Check(off == 0, "off allows none");
	Check(!CheatAllowed(CHEAT_RULE_SHARED, CHEAT_COUNT), "a cheat that does not exist is never allowed");

	const uint8_t others = SESSION_FRIENDLY_FIRE | SESSION_AMMO_SYNC |
	                       SESSION_WANTED_MASK | SESSION_RAMPAGE_MASK;
	bool roundTrip = true;
	for (uint8_t rule = 0; rule <= CHEAT_RULE_OFF; ++rule) {
		const uint8_t flags = FlagsWithCheatRule(others, rule);
		if (CheatRuleFromFlags(flags) != rule || (flags & others) != others)
			roundTrip = false;
	}
	Check(roundTrip, "the rule rides bits 6-7 and leaves the other rules alone");
	Check(CheatRuleFromFlags(0xC0) == CHEAT_RULE_SHARED, "3 is not a rule and reads as shared");
	Check(CheatRuleFromFlags(0) == CHEAT_RULE_SHARED,
	      "and a server too old to set the bits means shared");
}

void TestWhereATypedCheatRuns() {
	std::printf("\nwhere a typed cheat runs\n");

	CheatContext alone;
	bool vanilla = true;
	for (uint8_t id = 0; id < CHEAT_COUNT; ++id)
		for (uint8_t rule = 0; rule <= CHEAT_RULE_OFF; ++rule) {
			alone.rule = rule;
			if (PlanTypedCheat(alone, id) != CheatVerdict::Vanilla)
				vanilla = false;
		}
	Check(vanilla, "with no session every cheat is single player's, whatever rule was last heard");

	CheatContext guest{true, false, CHEAT_RULE_SHARED};
	CheatContext host{true, true, CHEAT_RULE_SHARED};
	Check(PlanTypedCheat(guest, CHEAT_FOGGY) == CheatVerdict::SendToHost,
	      "a guest's PEASOUP goes to the host and is not run here");
	Check(PlanTypedCheat(host, CHEAT_FOGGY) == CheatVerdict::RunHereAndSend,
	      "the host's PEASOUP runs here, and the world packet goes at once");
	Check(PlanTypedCheat(guest, CHEAT_MAYHEM) == CheatVerdict::RunHereAndSend &&
	          PlanTypedCheat(host, CHEAT_SLOW_TIME) == CheatVerdict::RunHereAndSend,
	      "a riot or a slowed clock runs here and goes to everybody, host or not");
	Check(PlanTypedCheat(guest, CHEAT_WANTED_UP) == CheatVerdict::RunHere &&
	          PlanTypedCheat(guest, CHEAT_TANK) == CheatVerdict::RunHere &&
	          PlanTypedCheat(guest, CHEAT_BLOW_UP_CARS) == CheatVerdict::RunHere,
	      "MOREPOLICEPLEASE, the tank and BANGBANGBANG run here and nowhere else");

	CheatContext personal{true, false, CHEAT_RULE_PERSONAL};
	Check(PlanTypedCheat(personal, CHEAT_RAINY) == CheatVerdict::Refused &&
	          PlanTypedCheat(personal, CHEAT_BLOW_UP_CARS) == CheatVerdict::Refused,
	      "under personal the sky and BANGBANGBANG are refused");
	Check(PlanTypedCheat(personal, CHEAT_HEALTH) == CheatVerdict::RunHere,
	      "and GESUNDHEIT still works");
	CheatContext off{true, true, CHEAT_RULE_OFF};
	Check(PlanTypedCheat(off, CHEAT_MONEY) == CheatVerdict::Refused,
	      "under off not even the money");
	Check(std::strstr(CheatRefusalReason(CHEAT_RULE_OFF), "cheats = off") != nullptr &&
	          std::strstr(CheatRefusalReason(CHEAT_RULE_PERSONAL), "cheats = personal") != nullptr,
	      "and the log line names the setting that refused it");
}

// ---- the state a cheat leaves, and arriving at it --------------------------------

void TestTheTimeScaleAsAState() {
	std::printf("\nthe time scale as a state\n");
	uint8_t s = 0xFF;
	Check(TimeScaleToCheatState(1.0f, s) && s == CHEAT_TIME_SCALE_STATE_NORMAL, "1.0 is 2");
	Check(TimeScaleToCheatState(0.25f, s) && s == 0, "0.25 is 0");
	Check(TimeScaleToCheatState(4.0f, s) && s == 4, "4.0 is 4");
	Check(!TimeScaleToCheatState(0.3f, s) && !TimeScaleToCheatState(3.0f, s) &&
	          !TimeScaleToCheatState(8.0f, s),
	      "anything the two handlers cannot reach is not a state");
}

// The two handlers' own arithmetic, off their disassembly: double below 4,
// halve above 0.25.
float FastHandler(float scale) { return scale < 4.0f ? scale * 2.0f : scale; }
float SlowHandler(float scale) { return scale > 0.25f ? scale * 0.5f : scale; }

void TestEveryTimeScaleCanReachEveryOther() {
	std::printf("\narriving at somebody else's time scale\n");
	const float scales[] = {0.25f, 0.5f, 1.0f, 2.0f, 4.0f};
	int wrong = 0;
	for (float from : scales)
		for (uint8_t want = 0; want <= CHEAT_TIME_SCALE_STATE_MAX; ++want) {
			EngineCheatState engine;
			engine.timeScale = from;
			const CheatSteps steps = PlanRoutedCheat(CHEAT_FAST_TIME, want, engine);
			if (steps.refused) {
				++wrong;
				continue;
			}
			float scale = from;
			for (uint8_t i = 0; i < steps.count; ++i)
				scale = steps.handler == CHEAT_FAST_TIME ? FastHandler(scale)
				                                         : SlowHandler(scale);
			uint8_t got = 0xFF;
			if (!TimeScaleToCheatState(scale, got) || got != want)
				++wrong;
		}
	Check(wrong == 0, "25 starts and targets, each reached by the engine's own handlers");

	EngineCheatState mission;
	mission.timeScale = 0.3f;
	Check(PlanRoutedCheat(CHEAT_SLOW_TIME, 1, mission).refused,
	      "a time scale no cheat made is not stepped from");
}

void TestTogglesArriveRatherThanFlip() {
	std::printf("\na toggle arrives, it does not flip\n");
	EngineCheatState armed;
	armed.givePedsWeapons = true;
	Check(PlanRoutedCheat(CHEAT_WEAPONS_FOR_ALL, 1, armed).count == 0,
	      "already armed and told armed: nothing runs");
	Check(PlanRoutedCheat(CHEAT_WEAPONS_FOR_ALL, 0, armed).count == 1,
	      "armed and told unarmed: the handler runs once");
	EngineCheatState calm;
	Check(PlanRoutedCheat(CHEAT_FAST_WEATHER, 1, calm).count == 1 &&
	          PlanRoutedCheat(CHEAT_FAST_WEATHER, 1, calm).handler == CHEAT_FAST_WEATHER,
	      "MADWEATHER off and told on: its own handler, once");
	Check(PlanRoutedCheat(CHEAT_MAYHEM, 1, calm).count == 1,
	      "a riot always runs - writing the table twice is the same table");
	Check(PlanRoutedCheat(CHEAT_CLOUDY, 0, calm).handler == CHEAT_CLOUDY,
	      "a sky runs its own ForceWeatherNow");
	Check(PlanRoutedCheat(CHEAT_MAYHEM, 0, calm).refused,
	      "an un-riot is refused: there is no such thing");
	Check(PlanRoutedCheat(CHEAT_MONEY, 0, calm).refused,
	      "and a personal cheat off the wire is refused outright");

	uint8_t state = 0xFF;
	Check(CurrentCheatState(CHEAT_WEAPONS_FOR_ALL, armed, state) && state == 1,
	      "what WEAPONSFORALL left us at is read back as 1");
}

// ---- the two exceptions -------------------------------------------------------------

void TestGesundheitLeavesSomebodyElsesCarAlone() {
	std::printf("\nGESUNDHEIT in the passenger seat\n");
	Check(HealthCheatMayRepairCar(false, true, false),
	      "single player heals the car whoever is driving");
	Check(HealthCheatMayRepairCar(true, true, true), "the driver heals his own car");
	Check(!HealthCheatMayRepairCar(true, true, false),
	      "a passenger does not heal somebody else's");
	Check(HealthCheatMayRepairCar(true, false, false), "on foot there is no car to argue about");
}

void TestReplicasDoNotLookForThreats() {
	std::printf("\nScanForThreats on a ped CoopIII built\n");
	Check(MayScanForThreats(false, false), "the engine's own pedestrians scan as always");
	Check(!MayScanForThreats(true, false), "a remote player does not");
	Check(!MayScanForThreats(false, true), "a replica pedestrian does not");
}

// ---- BANGBANGBANG ------------------------------------------------------------------

// The cheat calls slot 29 on every car in the pool, and the detour classifies
// each by what it is, never by who called. So the refusals are the same ones
// protocols 23, 24, 28 and 30 added, walked here for every kind of car a pool
// can hold.
void TestBangBangBangMeetsTheSameRefusals() {
	std::printf("\nBANGBANGBANG over a pool of every kind of car\n");
	struct Car {
		const char *what;
		bool        weDrive, remoteDriver, custodian, replica;
		bool        mayBlow;
	};
	const Car pool[] = {
	    {"the car we are driving", true, false, false, false, true},
	    {"a session car another player drives (23)", false, true, false, false, false},
	    {"a car another player is settling (24, 30)", false, false, true, false, false},
	    {"a replica of another machine's traffic (28)", false, false, false, true, false},
	    {"a parked car, a session car nobody holds, our own traffic", false, false, false, false, true},
	};
	for (const Car &c : pool) {
		const CarOwner owner = ClassifyCar(false, c.weDrive, c.remoteDriver,
		                                   c.custodian, c.replica);
		Check(MayBlowUpCar(owner) == c.mayBlow, c.what);
	}

	Check(CAutomobile__BlowUpCar != CVehicle__BlowUpCarBase &&
	          CBoat__BlowUpCar != CVehicle__BlowUpCarBase &&
	          CAutomobile__BlowUpCar != CBoat__BlowUpCar,
	      "slot 29 has two bodies worth hooking and one that is a bare ret");
}

void TestAPoolOfWrecksFitsTheQueue() {
	std::printf("\na whole pool of wrecks in one frame\n");

	WreckQueue<VEHICLE_POOL_SIZE> q;
	for (uint16_t i = 0; i < VEHICLE_POOL_SIZE; ++i) {
		UnownedBlast b{};
		b.key.kind = (i % 2) ? UNOWNED_PARKED : UNOWNED_SESSION;
		b.key.id   = static_cast<uint16_t>(i / 2);
		q.Push(b);
	}
	Check(q.Count() == size_t(VEHICLE_POOL_SIZE) && q.Dropped() == 0,
	      "110 wrecks in one frame, none dropped");

	UnownedBlast dup{};
	dup.key.kind = UNOWNED_PARKED;
	dup.key.id   = 3;
	Check(!q.Push(dup), "the same car twice is one report");

	// Client drains four a frame.
	std::vector<UnownedBlast> out;
	int                       frames = 0;
	UnownedBlast              batch[4];
	for (;;) {
		const uint8_t n = q.Drain(batch, 4);
		if (n == 0)
			break;
		out.insert(out.end(), batch, batch + n);
		++frames;
	}
	bool inOrder = out.size() == size_t(VEHICLE_POOL_SIZE);
	for (size_t i = 0; inOrder && i < out.size(); ++i)
		if (out[i].key.id != i / 2 ||
		    out[i].key.kind != ((i % 2) ? UNOWNED_PARKED : UNOWNED_SESSION))
			inOrder = false;
	Check(inOrder, "every one comes out, oldest first");
	Check(frames == 28, "over 28 frames of four");

	// What the old ring of eight did with the same frame.
	WreckQueue<8> old;
	for (uint16_t i = 0; i < VEHICLE_POOL_SIZE; ++i) {
		UnownedBlast b{};
		b.key.kind = UNOWNED_PARKED;
		b.key.id   = i;
		old.Push(b);
	}
	Check(old.Count() == 8 && old.Dropped() == 102,
	      "an eight-slot ring loses 102 of them - the bug this replaced");
}

// ---- Client ---------------------------------------------------------------------------

int g_wreckAsks = 0;

// The other end of the same frame: somebody across town typed BANGBANGBANG,
// and every parked car it wrecked arrives here as an instruction for a car
// this machine has not streamed in. Each one has to be held until it is.
void TestAFarAwayReceiverHoldsAWholePoolOfWrecks() {
	std::printf("\nsomebody else's BANGBANGBANG, from across town\n");
	WorldBridge b;
	b.WreckUnownedVehicle = [](const UnownedVehicleKey &) {
		++g_wreckAsks;
		return UnownedWreckOutcome::NotHere;
	};
	Client c;
	c.SetBridge(b);
	S_Welcome w;
	InitHeader(w, 1000);
	w.reject = 0;
	w.playerId = 1;
	w.netId = 101;
	w.maxPlayers = MAX_PLAYERS;
	w.snapshotHz = SNAPSHOT_HZ;
	w.hour = 12;
	w.minute = 0;
	w.weather = 0;
	w.weatherOld = 0;
	w.hostPlayerId = 0;
	w.flags = 0;
	Message m;
	m.opcode  = S_Welcome::OPCODE;
	m.channel = CH_EVENT;
	m.data.resize(sizeof(w));
	std::memcpy(m.data.data(), &w, sizeof(w));
	c.HandleMessage(m);

	for (uint16_t id = 0; id < VEHICLE_POOL_SIZE; ++id) {
		S_UnownedBlowUp blast{};
		InitHeader(blast, 4000);
		blast.where.rot        = {0.0f, 0.0f, 0.0f, 1.0f};
		blast.reporterPlayerId = 0;
		blast.key.kind         = UNOWNED_PARKED;
		blast.key.id           = id;
		Message bm;
		bm.opcode  = S_UnownedBlowUp::OPCODE;
		bm.channel = CH_EVENT;
		bm.data.resize(sizeof(blast));
		std::memcpy(bm.data.data(), &blast, sizeof(blast));
		c.HandleMessage(bm);
	}
	g_wreckAsks = 0;
	c.Tick();
	Check(g_wreckAsks == VEHICLE_POOL_SIZE,
	      "all 110 are held and asked about, where 32 used to be");
}

struct CheatRecord {
	int     sessionCalls = 0;
	bool    inSession    = false;
	bool    isHost       = false;
	uint8_t rule         = 0xFF;
	std::vector<CheatBody> applied;
};
CheatRecord g_cheatRec;

WorldBridge CheatBridge() {
	g_cheatRec = CheatRecord{};
	WorldBridge b;
	b.SetCheatSession = [](bool inSession, bool isHost, uint8_t rule) {
		++g_cheatRec.sessionCalls;
		g_cheatRec.inSession = inSession;
		g_cheatRec.isHost    = isHost;
		g_cheatRec.rule      = rule;
	};
	b.ApplyRoutedCheat = [](uint8_t cheat, uint8_t state) {
		CheatBody body{};
		body.cheat = cheat;
		body.state = state;
		g_cheatRec.applied.push_back(body);
	};
	return b;
}

template <class T>
Message WrapCheat(const T &pkt) {
	Message m;
	m.opcode  = T::OPCODE;
	m.channel = CH_EVENT;
	m.data.resize(sizeof(T));
	std::memcpy(m.data.data(), &pkt, sizeof(T));
	return m;
}

S_Welcome Welcome(uint8_t playerId, uint8_t hostPlayerId, uint8_t cheatRule) {
	S_Welcome w;
	InitHeader(w, 1000);
	w.reject       = 0;
	w.playerId     = playerId;
	w.netId        = uint16_t(100 + playerId);
	w.maxPlayers   = MAX_PLAYERS;
	w.snapshotHz   = SNAPSHOT_HZ;
	w.hour         = 12;
	w.minute       = 0;
	w.weather      = 0;
	w.weatherOld   = 0;
	w.hostPlayerId = hostPlayerId;
	w.flags        = FlagsWithCheatRule(0, cheatRule);
	return w;
}

S_Cheat Typed(uint8_t by, uint8_t id, uint8_t state) {
	S_Cheat c;
	InitHeader(c, 2000);
	c.playerId   = by;
	c.body.cheat = id;
	c.body.state = state;
	return c;
}

void TestTheWelcomeTellsTheSeam() {
	std::printf("\nthe welcome tells the seam\n");
	Client c;
	c.SetBridge(CheatBridge());
	c.HandleMessage(WrapCheat(Welcome(2, 0, CHEAT_RULE_PERSONAL)));
	Check(c.CheatRule() == CHEAT_RULE_PERSONAL, "the rule comes off the welcome's bits 6-7");
	Check(g_cheatRec.sessionCalls > 0 && g_cheatRec.inSession && !g_cheatRec.isHost &&
	          g_cheatRec.rule == CHEAT_RULE_PERSONAL,
	      "and the seam hears it before any keystroke can: in a session, not host, personal");

	Client h;
	h.SetBridge(CheatBridge());
	h.HandleMessage(WrapCheat(Welcome(0, 0, CHEAT_RULE_SHARED)));
	Check(g_cheatRec.isHost, "a player who is the host is told so");
}

void TestARoutedCheatIsBroughtAboutHere() {
	std::printf("\nsomebody else's cheat\n");
	Client c;
	c.SetBridge(CheatBridge());
	c.HandleMessage(WrapCheat(Welcome(1, 0, CHEAT_RULE_SHARED)));

	c.HandleMessage(WrapCheat(Typed(0, CHEAT_MAYHEM, 1)));
	Check(g_cheatRec.applied.size() == 1 && g_cheatRec.applied[0].cheat == CHEAT_MAYHEM,
	      "the host's riot is run here");

	c.HandleMessage(WrapCheat(Typed(1, CHEAT_WEAPONS_FOR_ALL, 1)));
	Check(g_cheatRec.applied.size() == 1, "our own cheat coming back is not run twice");

	c.HandleMessage(WrapCheat(Typed(0, CHEAT_FOGGY, 0)));
	Check(g_cheatRec.applied.size() == 1,
	      "a sky that reaches a guest is dropped - the host's world packet is the sky");

	c.HandleMessage(WrapCheat(Typed(0, CHEAT_FAST_TIME, 9)));
	Check(g_cheatRec.applied.size() == 1, "a state that is not one is dropped");

	c.HandleMessage(WrapCheat(Typed(INVALID_PLAYER, CHEAT_SLOW_TIME, 1)));
	Check(g_cheatRec.applied.size() == 2 && g_cheatRec.applied[1].state == 1,
	      "a joiner's replay, from nobody in particular, is run");
}

void TestTheHostTakesASkyAndSendsItAtOnce() {
	std::printf("\na sky cheat reaching the host\n");
	Client h;
	h.SetBridge(CheatBridge());
	h.HandleMessage(WrapCheat(Welcome(0, 0, CHEAT_RULE_SHARED)));
	Check(!h.WorldSendPending(), "nothing waiting to begin with");
	h.HandleMessage(WrapCheat(Typed(3, CHEAT_RAINY, 0)));
	Check(g_cheatRec.applied.size() == 1 && g_cheatRec.applied[0].cheat == CHEAT_RAINY,
	      "the host runs the guest's ILOVESCOTLAND");
	Check(h.WorldSendPending(), "and its world packet goes without waiting out the second");
}

S_MissionState MissionRunning(uint8_t owner, uint8_t state = MISSION_STATE_RUNNING) {
	S_MissionState s;
	InitHeader(s, 1000);
	// The server's defaults, as every server sends them.
	s.checkpointWaitS = MISSION_CHECKPOINT_WAIT_MS / 1000;
	s.catchUpM        = MISSION_CATCH_UP_M_DEFAULT;
	s.behindM         = MISSION_BEHIND_M_DEFAULT;
	s.behindS         = MISSION_BEHIND_S_DEFAULT;
	s.campaignLog   = 77;
	s.state         = state;
	s.ownerId       = owner;
	s.missionNumber = 19;
	s.participants  = 0x03;
	s.marginCm      = 500;
	return s;
}

void TestASkyCheatGoesWhereTheSkyIs() {
	std::printf("\na sky cheat while somebody's mission runs\n");
	Client o;
	o.SetBridge(CheatBridge());
	o.HandleMessage(WrapCheat(Welcome(1, 0, CHEAT_RULE_SHARED)));
	Check(!g_cheatRec.isHost, "a guest's seam is not told the sky is its own");
	o.HandleMessage(WrapCheat(MissionRunning(1)));
	Check(g_cheatRec.isHost, "until its mission is the session's, and then it is, at once");
	o.HandleMessage(WrapCheat(Typed(0, CHEAT_RAINY, 0)));
	Check(g_cheatRec.applied.size() == 1 && g_cheatRec.applied[0].cheat == CHEAT_RAINY,
	      "the host's ILOVESCOTLAND is run by the mission's owner, whose sky everybody has");

	Client h;
	h.SetBridge(CheatBridge());
	h.HandleMessage(WrapCheat(Welcome(0, 0, CHEAT_RULE_SHARED)));
	h.HandleMessage(WrapCheat(MissionRunning(1)));
	Check(!g_cheatRec.isHost, "the host's seam hears its sky has gone to the owner");
	h.HandleMessage(WrapCheat(Typed(1, CHEAT_FOGGY, 0)));
	Check(g_cheatRec.applied.empty(), "and a sky reaching the host then is dropped, not fought over");
	h.HandleMessage(WrapCheat(MissionRunning(1, MISSION_STATE_IDLE)));
	Check(g_cheatRec.isHost, "the mission over, it is the host's again");
}

void TestTheRuleIsKeptEvenIfTheServerDoesNot() {
	std::printf("\na server that relays what the rule refuses\n");
	Client c;
	c.SetBridge(CheatBridge());
	c.HandleMessage(WrapCheat(Welcome(1, 0, CHEAT_RULE_PERSONAL)));
	c.HandleMessage(WrapCheat(Typed(0, CHEAT_MAYHEM, 1)));
	c.HandleMessage(WrapCheat(Typed(0, CHEAT_FAST_TIME, 3)));
	Check(g_cheatRec.applied.empty(), "under personal no riot and no clock arrive here");

	Client o;
	o.SetBridge(CheatBridge());
	o.HandleMessage(WrapCheat(Welcome(1, 0, CHEAT_RULE_OFF)));
	o.HandleMessage(WrapCheat(Typed(0, CHEAT_WEAPONS_FOR_ALL, 1)));
	Check(g_cheatRec.applied.empty(), "under off nothing does");
}

// ---- the table against the real exe ----------------------------------------------------

// Opt-in: COOPIII_GTA3_EXE names a retail 1.0 gta3.exe, or reference/bin/
// gta3.exe is found from the working directory or four levels above it (the
// build output). Without one this says so and checks nothing.
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

uint32_t Dword(const std::vector<uint8_t> &img, uint32_t va) {
	const size_t o = va - IMAGE_BASE;
	return uint32_t(img[o]) | uint32_t(img[o + 1]) << 8 | uint32_t(img[o + 2]) << 16 |
	       uint32_t(img[o + 3]) << 24;
}

uint8_t Byte(const std::vector<uint8_t> &img, uint32_t va) { return img[va - IMAGE_BASE]; }

void TestTheTableAgainstTheImage() {
	std::printf("\nthe cheat table against gta3.exe\n");
	std::vector<uint8_t> img;
	std::string          from;
	if (!LoadExe(img, from)) {
		std::printf("  [skipped] no retail gta3.exe; set COOPIII_GTA3_EXE to check the "
		            "table against one\n");
		return;
	}
	std::printf("  reading %s\n", from.c_str());

	// The prologue the seam checks before it hooks, and the shift: `mov
	// edx,12h` and the buffer twice.
	Check(std::memcmp(&img[CPad__AddToPCCheatString - IMAGE_BASE],
	                  CPAD_ADD_TO_PC_CHEAT_STRING_PROLOGUE,
	                  sizeof(CPAD_ADD_TO_PC_CHEAT_STRING_PROLOGUE)) == 0,
	      "the function opens with the prologue the seam looks for");
	Check(Byte(img, 0x00492457) == 0xBA && Dword(img, 0x00492458) == 0x12,
	      "the shift starts at index 18 (mov edx,12h)");
	Check(Dword(img, 0x00492462) == CPad__KeyBoardCheatString &&
	          Dword(img, 0x00492468) == CPad__KeyBoardCheatString + 1,
	      "and moves KeyBoardCheatString[i] to [i+1]");
	Check(std::memcmp(&img[CPad__CheatShiftBegin - IMAGE_BASE], CPAD_CHEAT_SHIFT,
	                  sizeof(CPAD_CHEAT_SHIFT)) == 0 &&
	          CPad__CheatShiftBegin ==
	              CPad__AddToPCCheatString + sizeof(CPAD_ADD_TO_PC_CHEAT_STRING_PROLOGUE),
	      "CPAD_CHEAT_SHIFT is the shift's bytes, from the end of the prologue to row 0");

	// Then every row, through the same decoder game/cheats.cpp runs over the
	// live process before it hooks anything.
	const ImageByteFn fromFile = [](const void *ctx, uint32_t va) -> uint8_t {
		return (*static_cast<const std::vector<uint8_t> *>(ctx))[va - IMAGE_BASE];
	};
	DecodedCheatRow rows[CHEAT_COUNT];
	uint8_t         bad = 0xFF;
	Check(DecodeCheatRows(fromFile, &img, rows, &bad),
	      "23 rows in the shape addresses.h gives them, then the epilogue");
	bool pushes = true;
	for (uint8_t id = 0; id < CHEAT_COUNT; ++id)
		if (rows[id].length != CHEAT_SITES[id].length) {
			pushes = false;
			std::printf("    row %u (%s) pushes %u, the table says %u\n", id,
			            Forward(id).c_str(), rows[id].length, CHEAT_SITES[id].length);
		}
	Check(pushes, "every pushed length is the table's, BOOOOORING's 16 included");
	Check(CheatRowsMatchTable(fromFile, &img, &bad),
	      "and every string, its NUL and every handler are too");
	if (bad < CHEAT_COUNT)
		std::printf("    first bad row: %u (%s)\n", bad, Forward(bad).c_str());

	// The table the chat key goes by, read the way game/cheats.cpp reads the
	// running code: from retail it is retail's.
	TypedCheatTable fromImage;
	const TypedCheatTable retail = TableFromSites();
	bool            same = TableFromRows(rows, fromFile, &img, &fromImage, &bad);
	for (uint8_t id = 0; same && id < CHEAT_COUNT; ++id)
		same = fromImage.rows[id].length == retail.rows[id].length &&
		       std::memcmp(fromImage.rows[id].reversed, retail.rows[id].reversed,
		                   sizeof retail.rows[id].reversed) == 0;
	Check(same, "the typed table read off the image is retail's, row for row");

	// TPTO's two addresses.
	Check(std::memcmp(&img[CHEAT_ACTIVATED_KEY - IMAGE_BASE], "CHEAT1\0", 7) == 0 &&
	          Dword(img, 0x00490EEE) == CHEAT_ACTIVATED_KEY &&
	          Dword(img, 0x0049104C) == CHEAT_ACTIVATED_KEY,
	      "5F64C0h is \"CHEAT1\", the key the tank and BANGBANGBANG push for their line");
	Check(Dword(img, CAutomobile__vtable + ENTITY_VT_TELEPORT) == CAutomobile__Teleport,
	      "CAutomobile's slot 11 is CAutomobile::Teleport, 0x00535180");
	Check(Dword(img, CVehicle__vtable + ENTITY_VT_TELEPORT) == 0x00405930 &&
	          Dword(img, CBoat__vtable + ENTITY_VT_TELEPORT) != CAutomobile__Teleport,
	      "and nothing else's is: CVehicle's is the empty one, a boat's its own");
	const uint8_t teleport[] = {0x56, 0x57, 0x55, 0x89, 0xCD, 0x83, 0xEC, 0x18};
	Check(std::memcmp(&img[CAutomobile__Teleport - IMAGE_BASE], teleport, sizeof teleport) == 0 &&
	          Byte(img, 0x00535199) == 0xE8 && Dword(img, 0x0053519A) + 0x0053519E == CWorld__Remove &&
	          Byte(img, 0x00535236) == 0xE8 && Dword(img, 0x00535237) + 0x0053523B == CWorld__Add &&
	          Byte(img, 0x00535242) == 0xC2 && Byte(img, 0x00535243) == 0x0C,
	      "which takes the car out of the world, puts it back and returns `ret 0Ch`");
	const uint8_t still[] = {0xC7, 0x45, 0x78, 0x00, 0x00, 0x00, 0x00};
	Check(std::memcmp(&img[0x005351FA - IMAGE_BASE], still, sizeof still) == 0,
	      "and stops it on the way: `mov [ebp+78h],0`, the first of the six speeds");

	// Slot 29 of every vehicle-pool vtable.
	Check(Dword(img, 0x00600C1C + 0x74) == CAutomobile__BlowUpCar &&
	          Dword(img, 0x00600EA4 + 0x74) == CBoat__BlowUpCar,
	      "CAutomobile and CBoat reach the two hooked BlowUpCars");
	Check(Dword(img, CHeli__vtable + 0x74) == CVehicle__BlowUpCarBase &&
	          Dword(img, CPlane__vtable + 0x74) == CVehicle__BlowUpCarBase &&
	          Dword(img, CTrain__vtable + 0x74) == CVehicle__BlowUpCarBase &&
	          Dword(img, CVehicle__vtable + 0x74) == CVehicle__BlowUpCarBase,
	      "the heli, the planes, the trains and the base reach the empty one");
	const uint8_t empty[] = {0x83, 0xEC, 0x08, 0x89, 0x4C, 0x24, 0x04,
	                         0x83, 0xC4, 0x08, 0xC2, 0x04, 0x00};
	Check(std::memcmp(&img[CVehicle__BlowUpCarBase - IMAGE_BASE], empty, sizeof(empty)) == 0,
	      "which is sub esp,8 / mov [esp+4],ecx / add esp,8 / ret 4 and nothing else");

	// The fear copy in CPed's constructor, and ScanForThreats reading it.
	Check(Dword(img, 0x004C4CDF) == CPedType__ms_apPedType &&
	          Dword(img, 0x004C4CE8) == offs::PED_FEAR_FLAGS,
	      "CPed's constructor copies ms_apPedType[type]->m_threats into +0x188");
	Check(Dword(img, CPed__ScanForThreats + 0x0B) == offs::PED_FEAR_FLAGS,
	      "and ScanForThreats reads +0x188, not the table");
}

// ---- may the detour go in: both shapes of the function, built byte by byte ----

// A sparse image: only the bytes put in it exist, and only those can be read.
struct SparseImage {
	std::vector<uint32_t> at;
	std::vector<uint8_t>  bytes;

	void Put(uint32_t va, uint8_t b) {
		for (size_t i = 0; i < at.size(); ++i)
			if (at[i] == va) {
				bytes[i] = b;
				return;
			}
		at.push_back(va);
		bytes.push_back(b);
	}
	void PutDword(uint32_t va, uint32_t v) {
		for (uint32_t i = 0; i < 4; ++i)
			Put(va + i, static_cast<uint8_t>(v >> (8 * i)));
	}
	void PutString(uint32_t va, const char *s) {
		for (uint32_t i = 0;; ++i) {
			Put(va + i, static_cast<uint8_t>(s[i]));
			if (s[i] == '\0')
				break;
		}
	}
	bool Has(uint32_t va) const {
		for (uint32_t a : at)
			if (a == va)
				return true;
		return false;
	}
	uint8_t Get(uint32_t va) const {
		for (size_t i = 0; i < at.size(); ++i)
			if (at[i] == va)
				return bytes[i];
		return 0xCC;
	}
};

uint8_t SparseByte(const void *ctx, uint32_t va) {
	return static_cast<const SparseImage *>(ctx)->Get(va);
}

bool SparseReadable(const void *ctx, uint32_t va, size_t n) {
	const SparseImage *img = static_cast<const SparseImage *>(ctx);
	for (size_t i = 0; i < n; ++i)
		if (!img->Has(va + static_cast<uint32_t>(i)))
			return false;
	return true;
}

// Where SilentPatch's own "TORTOISE" was in noxx's game. Its module's load
// decides it, so any address has to do.
constexpr uint32_t SP_TORTOISE = 0x61E98EF4;

// CPad::AddToPCCheatString as retail has it: the prologue, the shift, the 23
// rows (`A2 <buffer>` folded into row 0), the epilogue, and every string in
// .rdata. With `silentPatch`, rows 12 and 13 the way SilentPatch III
// rewrites them.
SparseImage BuildCheatFunction(bool silentPatch) {
	SparseImage img;
	uint32_t    pc = static_cast<uint32_t>(CPad__AddToPCCheatString);
	for (uint8_t b : CPAD_ADD_TO_PC_CHEAT_STRING_PROLOGUE)
		img.Put(pc++, b);
	for (uint8_t b : CPAD_CHEAT_SHIFT)
		img.Put(pc++, b);
	const uint32_t buffer = static_cast<uint32_t>(CPad__KeyBoardCheatString);
	for (uint8_t id = 0; id < CHEAT_COUNT; ++id) {
		const CheatSite &s      = CHEAT_SITES[id];
		uint8_t          length = s.length;
		uint32_t         string = static_cast<uint32_t>(s.string);
		if (silentPatch && id == CHEAT_SLOW_TIME)
			length = 0x0A;
		if (silentPatch && id == CHEAT_ARMOUR)
			string = SP_TORTOISE;
		img.Put(pc, 0x6A);
		img.Put(pc + 1, length);
		img.Put(pc + 2, 0x68);
		img.PutDword(pc + 3, buffer);
		img.Put(pc + 7, 0x68);
		img.PutDword(pc + 8, string);
		pc += 12;
		if (id == 0) {
			img.Put(pc, 0xA2);
			img.PutDword(pc + 1, buffer);
			pc += 5;
		}
		img.Put(pc, 0xE8);
		img.PutDword(pc + 1, static_cast<uint32_t>(crt_strncmp) - (pc + 5));
		pc += 5;
		const uint8_t test[] = {0x83, 0xC4, 0x0C, 0x85, 0xC0, 0x75, 0x05};
		for (uint8_t b : test)
			img.Put(pc++, b);
		img.Put(pc, 0xE8);
		img.PutDword(pc + 1, static_cast<uint32_t>(s.handler) - (pc + 5));
		pc += 5;
		// .rdata pads each string with zeros to four bytes, and BOOOOORING's
		// sixteen reach into that and the next string.
		img.PutString(static_cast<uint32_t>(s.string), s.reversed);
		for (uint32_t at = static_cast<uint32_t>(s.string + std::strlen(s.reversed) + 1); at % 4 != 0; ++at)
			img.Put(at, 0);
	}
	const uint8_t epilogue[] = {0x83, 0xC4, 0x08, 0xC2, 0x04, 0x00};
	for (uint8_t b : epilogue)
		img.Put(pc++, b);
	if (silentPatch)
		img.PutString(SP_TORTOISE, "ESIOTROT");
	return img;
}

// Where row `id`'s first byte is in the built function.
uint32_t RowAt(uint8_t id) {
	return static_cast<uint32_t>(CPad__CheatRowsBegin) + 29u * id + (id > 0 ? 5u : 0u);
}

bool SameTable(const TypedCheatTable &a, const TypedCheatTable &b) {
	for (uint8_t id = 0; id < CHEAT_COUNT; ++id)
		if (a.rows[id].length != b.rows[id].length ||
		    std::memcmp(a.rows[id].reversed, b.rows[id].reversed, sizeof a.rows[id].reversed) != 0)
			return false;
	return true;
}

// The decision game/cheats.cpp makes before it hooks, on the two shapes the
// function is known to have and on the rewrites it must still refuse.
void TestTheDetourGoesInOnlyOnCodeItKnows() {
	std::printf("\nthe detour goes in on retail's code and SilentPatch's, and nothing else\n");

	const SparseImage retail = BuildCheatFunction(false);
	const CheatImageCheck r  = CheckCheatImage(&SparseByte, &SparseReadable, &retail);
	Check(r.MayHook() && r.verdict == CheatImageVerdict::Retail && r.tableRead &&
	          SameTable(r.table, TableFromSites()),
	      "retail's function: the detour goes in, and the table it goes by is retail's");

	const SparseImage sp = BuildCheatFunction(true);
	const CheatImageCheck s = CheckCheatImage(&SparseByte, &SparseReadable, &sp);
	Check(s.MayHook() && s.verdict == CheatImageVerdict::Known && s.tableRead &&
	          SameTable(s.table, SilentPatchTable()),
	      "SilentPatch's: it goes in too, and goes by TORTOISE and BOOOOORING's ten");
	int marked = 0;
	for (uint8_t id = 0; id < CHEAT_COUNT; ++id)
		if (s.rewritten[id] != 0)
			++marked;
	Check(marked == 2 && s.rewritten[CHEAT_SLOW_TIME] != 0 && s.rewritten[CHEAT_ARMOUR] != 0,
	      "with exactly those two rows put down as SilentPatch's");

	SparseImage moved = BuildCheatFunction(true);
	moved.PutString(0x7FFE0000, "ESIOTROT");
	moved.PutDword(RowAt(CHEAT_ARMOUR) + 8, 0x7FFE0000);
	Check(CheckCheatImage(&SparseByte, &SparseReadable, &moved).MayHook(),
	      "its TORTOISE is taken wherever its module was loaded");

	// Each row on its own: one of SilentPatch's two is as good as both.
	SparseImage onlyLength = BuildCheatFunction(false);
	onlyLength.Put(RowAt(CHEAT_SLOW_TIME) + 1, 0x0A);
	const CheatImageCheck ol = CheckCheatImage(&SparseByte, &SparseReadable, &onlyLength);
	Check(ol.MayHook() && ol.verdict == CheatImageVerdict::Known,
	      "BOOOOORING's ten alone goes in");

	struct Refusal {
		const char     *what;
		CheatImageFault fault;
		uint8_t         row;
		SparseImage     img;
	};
	std::vector<Refusal> refusals;
	auto add = [&](const char *what, CheatImageFault fault, uint8_t row) -> SparseImage & {
		refusals.push_back({what, fault, row, BuildCheatFunction(true)});
		return refusals.back().img;
	};

	add("a jump over the prologue (another plugin's hook) stays out", CheatImageFault::Prologue,
	    CHEAT_COUNT)
	    .Put(static_cast<uint32_t>(CPad__AddToPCCheatString), 0xE9);
	add("a shift that starts at 17, not 18, stays out", CheatImageFault::Shift, CHEAT_COUNT)
	    .Put(static_cast<uint32_t>(CPad__CheatShiftBegin) + 1, 0x11);
	{
		SparseImage &img = add("an armour row whose string reads ESIOTRAT stays out",
		                       CheatImageFault::Row, CHEAT_ARMOUR);
		img.PutString(SP_TORTOISE, "ESIOTRAT");
	}
	add("an armour row comparing nine bytes stays out", CheatImageFault::Row, CHEAT_ARMOUR)
	    .Put(RowAt(CHEAT_ARMOUR) + 1, 0x09);
	add("BOOOOORING comparing eleven stays out", CheatImageFault::Row, CHEAT_SLOW_TIME)
	    .Put(RowAt(CHEAT_SLOW_TIME) + 1, 0x0B);
	{
		// The ten is only known at retail's string; elsewhere it is somebody else's.
		SparseImage &img = add("BOOOOORING's ten pointed at a copy of its string stays out",
		                       CheatImageFault::Row, CHEAT_SLOW_TIME);
		img.PutString(0x7FFE0100, "GNIROOOOOB");
		img.PutDword(RowAt(CHEAT_SLOW_TIME) + 8, 0x7FFE0100);
	}
	{
		SparseImage &img = add("SilentPatch's TORTOISE on another row stays out",
		                       CheatImageFault::Row, CHEAT_HEALTH);
		img.Put(RowAt(CHEAT_HEALTH) + 1, 0x08);
		img.PutDword(RowAt(CHEAT_HEALTH) + 8, SP_TORTOISE);
	}
	{
		SparseImage   &img  = add("a handler that is not retail's stays out", CheatImageFault::Row,
		                          CHEAT_SUNNY);
		const uint32_t call = RowAt(CHEAT_SUNNY) + 12 + 5 + 7;
		img.PutDword(call + 1, 0x00401000 - (call + 5));
	}
	add("a string that cannot be read stays out", CheatImageFault::Unreadable, CHEAT_ARMOUR)
	    .PutDword(RowAt(CHEAT_ARMOUR) + 8, 0x7FFF0000);
	add("a row that asks for more than the buffer stays out", CheatImageFault::Length,
	    CHEAT_MONEY)
	    .Put(RowAt(CHEAT_MONEY) + 1, 0x15);
	add("a row that is not a row any more stays out", CheatImageFault::Shape, CHEAT_TANK)
	    .Put(RowAt(CHEAT_TANK), 0x90);

	for (const Refusal &x : refusals) {
		const CheatImageCheck c = CheckCheatImage(&SparseByte, &SparseReadable, &x.img);
		Check(!c.MayHook() && c.fault == x.fault && c.row == x.row, x.what);
	}

	// What the detour then compares, in a session: the running table.
	const TypedCheatTable t = s.table;
	auto fired = [&](const char *prefix, const char *word) {
		char buffer[KEYBOARD_CHEAT_STRING_LEN] = {};
		std::vector<uint8_t> all;
		for (const char *p = prefix; *p; ++p)
			PushCheatChar(buffer, *p);
		for (const char *p = word; *p; ++p) {
			PushCheatChar(buffer, *p);
			uint8_t       ids[CHEAT_COUNT];
			const uint8_t n = MatchTypedCheats(t, buffer, ids, CHEAT_COUNT);
			all.insert(all.end(), ids, ids + n);
		}
		return all;
	};
	Check(fired("", "TORTOISE") == std::vector<uint8_t>{CHEAT_ARMOUR},
	      "in a SilentPatched session the detour runs TORTOISE as armour");
	Check(fired("", "TURTOISE").empty(), "and TURTOISE as nothing, the way that game does");
	Check(fired("WASDWASD", "BOOOOORING") == std::vector<uint8_t>{CHEAT_SLOW_TIME},
	      "and BOOOOORING after other keys, as its ten says");
	Check(fired("", "ILIKESCOTLAND") == std::vector<uint8_t>{CHEAT_CLOUDY},
	      "while the rows it left alone still fire as themselves");

	// The builder is the exe, byte for byte, when there is one to compare.
	std::vector<uint8_t> exe;
	std::string          from;
	if (LoadExe(exe, from)) {
		bool same = true;
		for (size_t i = 0; i < retail.at.size(); ++i)
			if (exe[retail.at[i] - IMAGE_BASE] != retail.bytes[i])
				same = false;
		Check(same, "the retail shape built here is gta3.exe's, every byte of it");
	}
}

} // namespace

int RunCheatTests() {
	TestTheTableIsWhatTheEngineCompares();
	TestEachCheatFiresOnItsLastKeyAndOnlyThen();
	TestNoCheatFiresAnotherOnTheWay();
	TestBoooooringOnlyWorksAsTheFirstThingTyped();
	TestEveryCheatCanBeTypedPastTheChatKey();
	TestChatTextNeverStartsACheat();
	TestTheChatKeyGoesByTheRunningGamesTable();
	TestTheCheatsNoxxTypes();
	TestOurOwnCheats();
	TestTptoDecides();
	TestTheBufferKeepsTheLastTwentyNewestFirst();
	TestWhatEachCheatTouches();
	TestTheRuleDecidesWhatIsAllowed();
	TestWhereATypedCheatRuns();
	TestTheTimeScaleAsAState();
	TestEveryTimeScaleCanReachEveryOther();
	TestTogglesArriveRatherThanFlip();
	TestGesundheitLeavesSomebodyElsesCarAlone();
	TestReplicasDoNotLookForThreats();
	TestBangBangBangMeetsTheSameRefusals();
	TestAPoolOfWrecksFitsTheQueue();
	TestAFarAwayReceiverHoldsAWholePoolOfWrecks();
	TestTheWelcomeTellsTheSeam();
	TestARoutedCheatIsBroughtAboutHere();
	TestTheHostTakesASkyAndSendsItAtOnce();
	TestASkyCheatGoesWhereTheSkyIs();
	TestTheRuleIsKeptEvenIfTheServerDoesNot();
	TestTheTableAgainstTheImage();
	TestTheDetourGoesInOnlyOnCodeItKnows();
	return g_cheatFailures;
}
