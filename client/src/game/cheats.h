// Cheats in a session: who a cheat belongs to, and where it runs.
//
// docs/cheats.md is the investigation and the table. What a reader of this
// file needs is four sentences.
//
// 1. **There is one door.** Every cheat in retail 1.0 comes through
//    CPad::AddToPCCheatString, which shifts the key into a 20-byte buffer
//    and runs 23 strncmps over it (addresses.h, "cheats"). CoopIII detours it
//    and, in a session, does that shift and those compares itself, so it
//    knows which cheat was typed before any of it has run.
//
// 2. **A cheat about the typist runs where it was typed.** Weapons, money,
//    health, armour, stars, the skin, the tank, the handling toggles. Each of
//    them either changes the typist's own state, which already travels, or
//    only this machine's simulation of cars it is only watching, which the
//    per-frame correction puts back.
//
// 3. **A cheat about the world runs where the world is owned.** The sky is
//    the host's, so the four weather cheats go to the host and come back to
//    everybody on the next S_WorldState. The clock's speed and the crowd's
//    temper are every machine's own, so those run everywhere, carrying the
//    state they left behind rather than "toggle". BANGBANGBANG runs where it
//    was typed: the BlowUpCar detour already refuses every car somebody else
//    owns, and the wrecks it is allowed travel.
//
// 4. **Nothing CoopIII built may act on the result.** A riot rewrites the
//    table every pedestrian built afterwards copies its fears from, replicas
//    and remote players included. ScanForThreats is detoured to answer
//    "nothing" for those.
//
// CoopIII has typed cheats of its own as well (COOP_CHEATS below, TPTO first;
// game/tpto.h does them), read off the same buffer after the game has had
// each key.
//
// Everything below the line marked "the engine half" is game/cheats.cpp; the
// rest is arithmetic, with no engine, so tools/clienttest runs it.
#pragma once

#include "addresses.h"
#include "client.h"

#include <coopiii/protocol.h>

#include <cstddef>
#include <cstdint>

namespace coopiii::game {

// ---- the typed buffer, as arithmetic ----------------------------------------

// AddToPCCheatString's first half: every character moves one to the right,
// the oldest falls off the end, the new one goes in at [0]. `buffer` is
// KEYBOARD_CHEAT_STRING_LEN bytes.
inline void PushCheatChar(char *buffer, char c) {
	for (size_t i = KEYBOARD_CHEAT_STRING_LEN - 1; i > 0; --i)
		buffer[i] = buffer[i - 1];
	buffer[0] = c;
}

// strncmp(site.reversed, buffer, site.length) == 0, spelled out. The one
// part of strncmp that matters is that it stops at a NUL both sides share:
// BOOOOORING's length is 16 for a ten-letter string (addresses.h), so its
// eleventh comparison is the literal's NUL against buffer[10], and past that
// strncmp reads no further.
inline bool ReversedMatches(const char *reversed, uint8_t length, const char *buffer) {
	for (uint8_t i = 0; i < length; ++i) {
		if (reversed[i] != buffer[i])
			return false;
		if (reversed[i] == '\0')
			return true;
	}
	return true;
}

inline bool CheatSiteMatches(const CheatSite &site, const char *buffer) {
	return ReversedMatches(site.reversed, site.length, buffer);
}

// ---- the table, read back out of the code -----------------------------------

// One byte of the image at a virtual address. tools/clienttest hands this a
// copy of the exe read from disk; game/cheats.cpp hands it the running
// process. One decoder, so the check a test makes is the check the game makes.
using ImageByteFn = uint8_t (*)(const void *ctx, uint32_t va);

struct DecodedCheatRow {
	uint8_t  length  = 0;
	uint32_t string  = 0;
	uint32_t handler = 0;
};

inline uint32_t ImageDword(ImageByteFn read, const void *ctx, uint32_t va) {
	return uint32_t(read(ctx, va)) | uint32_t(read(ctx, va + 1)) << 8 |
	       uint32_t(read(ctx, va + 2)) << 16 | uint32_t(read(ctx, va + 3)) << 24;
}

// AddToPCCheatString's 23 rows, walked byte by byte in the shape addresses.h
// transcribes:
//
//   6A len / 68 <buffer> / 68 <string> / [A2 <buffer>, row 0 only] /
//   E8 <strncmp> / 83 C4 0C / 85 C0 / 75 xx / E8 <handler>
//
// and then the epilogue, `83 C4 08 / C2 04 00`. False, with `*badRow` set,
// at the first byte that is not where that shape puts it - which is the
// answer both for a transcription error and for a function another mod has
// rewritten.
inline bool DecodeCheatRows(ImageByteFn read, const void *ctx,
                            DecodedCheatRow out[CHEAT_COUNT], uint8_t *badRow) {
	const uint32_t buffer = static_cast<uint32_t>(CPad__KeyBoardCheatString);
	uint32_t       pc     = static_cast<uint32_t>(CPad__CheatRowsBegin);
	for (uint8_t id = 0; id < CHEAT_COUNT; ++id) {
		*badRow = id;
		if (read(ctx, pc) != 0x6A || read(ctx, pc + 2) != 0x68 ||
		    ImageDword(read, ctx, pc + 3) != buffer || read(ctx, pc + 7) != 0x68)
			return false;
		out[id].length = read(ctx, pc + 1);
		out[id].string = ImageDword(read, ctx, pc + 8);
		pc += 12;
		if (id == 0) {
			if (read(ctx, pc) != 0xA2 || ImageDword(read, ctx, pc + 1) != buffer)
				return false;
			pc += 5;
		}
		if (read(ctx, pc) != 0xE8 ||
		    pc + 5 + ImageDword(read, ctx, pc + 1) != static_cast<uint32_t>(crt_strncmp))
			return false;
		pc += 5;
		if (read(ctx, pc) != 0x83 || read(ctx, pc + 1) != 0xC4 || read(ctx, pc + 2) != 0x0C ||
		    read(ctx, pc + 3) != 0x85 || read(ctx, pc + 4) != 0xC0 || read(ctx, pc + 5) != 0x75)
			return false;
		pc += 7;
		if (read(ctx, pc) != 0xE8)
			return false;
		out[id].handler = pc + 5 + ImageDword(read, ctx, pc + 1);
		pc += 5;
	}
	*badRow = CHEAT_COUNT;
	return pc == static_cast<uint32_t>(CPad__CheatRowsEnd) && read(ctx, pc) == 0x83 &&
	       read(ctx, pc + 1) == 0xC4 && read(ctx, pc + 2) == 0x08 &&
	       read(ctx, pc + 3) == 0xC2 && read(ctx, pc + 4) == 0x04;
}

// Does the code say what CHEAT_SITES says, string bytes included? `*badRow`
// is the first row that does not, or CHEAT_COUNT when it is the epilogue or
// all is well.
inline bool CheatRowsMatchTable(ImageByteFn read, const void *ctx, uint8_t *badRow) {
	DecodedCheatRow rows[CHEAT_COUNT];
	if (!DecodeCheatRows(read, ctx, rows, badRow))
		return false;
	for (uint8_t id = 0; id < CHEAT_COUNT; ++id) {
		*badRow               = id;
		const CheatSite &site = CHEAT_SITES[id];
		if (rows[id].length != site.length || rows[id].string != site.string ||
		    rows[id].handler != site.handler)
			return false;
		// The literal, its NUL included, since a compare can reach it.
		for (uint32_t i = 0;; ++i) {
			if (read(ctx, site.string + i) != static_cast<uint8_t>(site.reversed[i]))
				return false;
			if (site.reversed[i] == '\0')
				break;
		}
	}
	*badRow = CHEAT_COUNT;
	return true;
}

// Its second half: which cheats the buffer now completes, in the order the
// engine would run them. Returns how many; `out` gets their CheatIds.
inline uint8_t MatchTypedCheats(const char *buffer, uint8_t *out, uint8_t max) {
	uint8_t n = 0;
	for (uint8_t id = 0; id < CHEAT_COUNT && n < max; ++id)
		if (CheatSiteMatches(CHEAT_SITES[id], buffer))
			out[n++] = id;
	return n;
}

// ---- what can be typed: the game's rows as the running code has them ------------

// One row of CPad::AddToPCCheatString as the code in memory compares it: the
// string backwards and how many bytes strncmp is asked for. CHEAT_SITES is
// retail 1.0; the running game can differ, because other plugins rewrite
// rows. SilentPatch III points the armour row at a "TORTOISE" of its own
// (`push 61E98EF4h` where retail has `push 5F6618h`) and gives BOOOOORING its
// ten (`push 0Ah`). In that game TORTOISE is the cheat and TURTOISE is not, so
// anything that has to recognise a cheat being typed - the chat key's two
// rules below, and the detour's own compares in a session - reads the table
// off the running code, not out of this file.
struct TypedRow {
	char    reversed[KEYBOARD_CHEAT_STRING_LEN + 1] = {};
	uint8_t length = 0;
};

struct TypedCheatTable {
	TypedRow rows[CHEAT_COUNT];
};

// Retail's, out of CHEAT_SITES: what the running code is when nothing else
// has touched it, and what is used when it cannot be read.
inline TypedCheatTable TableFromSites() {
	TypedCheatTable t;
	for (uint8_t id = 0; id < CHEAT_COUNT; ++id) {
		const CheatSite &s = CHEAT_SITES[id];
		for (size_t i = 0; i < KEYBOARD_CHEAT_STRING_LEN && s.reversed[i] != '\0'; ++i)
			t.rows[id].reversed[i] = s.reversed[i];
		t.rows[id].length = s.length;
	}
	return t;
}

// The rows DecodeCheatRows found, with each string's bytes read through
// `read`, up to the NUL or the length compared. False, with `*badRow`, for a
// row that asks strncmp for nothing or for more than the twenty-byte buffer
// holds. The caller makes sure every string can be read before it asks.
inline bool TableFromRows(const DecodedCheatRow (&rows)[CHEAT_COUNT], ImageByteFn read,
                          const void *ctx, TypedCheatTable *out, uint8_t *badRow) {
	TypedCheatTable t;
	for (uint8_t id = 0; id < CHEAT_COUNT; ++id) {
		*badRow = id;
		if (rows[id].length == 0 || rows[id].length > KEYBOARD_CHEAT_STRING_LEN)
			return false;
		t.rows[id].length = rows[id].length;
		for (uint8_t i = 0; i < rows[id].length; ++i) {
			const char c = static_cast<char>(read(ctx, rows[id].string + i));
			if (c == '\0')
				break;
			t.rows[id].reversed[i] = c;
		}
	}
	*badRow = CHEAT_COUNT;
	*out    = t;
	return true;
}

// How many keys a player types for the row: its letters, which is fewer than
// the compare when the compare runs on into the NUL (BOOOOORING in retail).
inline uint8_t RowTypedLength(const TypedRow &row) {
	uint8_t n = 0;
	while (n < row.length && row.reversed[n] != '\0')
		++n;
	return n;
}

// The second half of the function again, on the running game's table: which
// rows the buffer now completes, in the order the engine tests them. This is
// what the detour compares in a session, so a game whose rows another plugin
// has rewritten keeps its own spellings and lengths.
inline uint8_t MatchTypedCheats(const TypedCheatTable &t, const char *buffer, uint8_t *out,
                                uint8_t max) {
	uint8_t n = 0;
	for (uint8_t id = 0; id < CHEAT_COUNT && n < max; ++id)
		if (ReversedMatches(t.rows[id].reversed, t.rows[id].length, buffer))
			out[n++] = id;
	return n;
}

// ---- may the detour go in: the running code against what it imitates ------------
//
// In a session the detour does the whole function itself, so it only goes in
// while the running code does what it would be imitating. What it depends on:
//
//   - the seven bytes of prologue the detour moves aside;
//   - the shift (CPAD_CHEAT_SHIFT), which PushCheatChar does instead;
//   - every row's shape, its handler, and the bytes its strncmp can reach:
//     the pushed length, and the string up to that length or its NUL;
//   - the epilogue's `ret 4`.
//
// A row may be retail's, or one of the rewrites below that another plugin is
// known to make. Both say what the row compares; the detour compares the same
// through the table read off the code. Anything else and it stays out.
//
// SilentPatch III makes two (read out of both of noxx's running games on
// 2026-09-30, docs/cheats.md §8): BOOOOORING compares its ten letters
// (`push 0Ah`) and the armour row points at a "TORTOISE" in SilentPatch's own
// module (`push 61E98EF4h` there), so its address is whatever that module's
// load gave it and only its bytes are checked.
struct KnownCheatRow {
	uint8_t     id;
	const char *reversed;
	uint8_t     length;
	bool        anyAddress;   // the string can be anywhere; false: retail's address
	const char *by;
};

constexpr KnownCheatRow KNOWN_CHEAT_ROWS[] = {
    {CHEAT_SLOW_TIME, "GNIROOOOOB", 0x0A, false, "SilentPatch III"},
    {CHEAT_ARMOUR, "ESIOTROT", 0x08, true, "SilentPatch III"},
};
constexpr size_t KNOWN_CHEAT_ROW_COUNT = sizeof(KNOWN_CHEAT_ROWS) / sizeof(KNOWN_CHEAT_ROWS[0]);

// Whether `n` bytes from `va` can be read. The game asks VirtualQuery; a test
// asks its own image.
using ImageReadableFn = bool (*)(const void *ctx, uint32_t va, size_t n);

enum class CheatImageVerdict : uint8_t {
	Retail,    // every row is retail's
	Known,     // some rows are a rewrite KNOWN_CHEAT_ROWS has
	Unknown,   // something the detour depends on is not either: it stays out
};

enum class CheatImageFault : uint8_t {
	None,
	Prologue,     // another plugin has hooked the function itself
	Shift,
	Shape,        // DecodeCheatRows failed at `row`, or the epilogue is not there
	Unreadable,   // `row`'s string cannot be read
	Length,       // `row` compares nothing, or more than the buffer has
	Row,          // `row` is neither retail's nor a known rewrite
};

struct CheatImageCheck {
	CheatImageVerdict verdict   = CheatImageVerdict::Unknown;
	CheatImageFault   fault     = CheatImageFault::None;
	uint8_t           row       = CHEAT_COUNT;
	bool              tableRead = false;   // `table` is the running code's
	TypedCheatTable   table;               // retail's when !tableRead
	uint8_t           rewritten[CHEAT_COUNT] = {};   // KNOWN_CHEAT_ROWS index + 1, or 0

	bool MayHook() const { return verdict != CheatImageVerdict::Unknown; }
};

// The bytes a row's strncmp can reach are `expected`'s: up to `length`, or to
// a NUL both share, whichever comes first.
inline bool RowStringIs(ImageByteFn read, const void *ctx, uint32_t va, uint8_t length,
                        const char *expected) {
	for (uint8_t i = 0; i < length; ++i) {
		if (read(ctx, va + i) != static_cast<uint8_t>(expected[i]))
			return false;
		if (expected[i] == '\0')
			return true;
	}
	return true;
}

// 0 when the row is not one of KNOWN_CHEAT_ROWS, else its index + 1.
inline uint8_t KnownRewriteOf(uint8_t id, const DecodedCheatRow &row, ImageByteFn read,
                              const void *ctx) {
	for (size_t k = 0; k < KNOWN_CHEAT_ROW_COUNT; ++k) {
		const KnownCheatRow &known = KNOWN_CHEAT_ROWS[k];
		if (known.id != id || row.length != known.length)
			continue;
		if (!known.anyAddress && row.string != static_cast<uint32_t>(CHEAT_SITES[id].string))
			continue;
		if (RowStringIs(read, ctx, row.string, row.length, known.reversed))
			return static_cast<uint8_t>(k + 1);
	}
	return 0;
}

inline CheatImageCheck CheckCheatImage(ImageByteFn read, ImageReadableFn readable,
                                       const void *ctx) {
	CheatImageCheck c;
	c.table = TableFromSites();

	DecodedCheatRow rows[CHEAT_COUNT];
	uint8_t         bad = CHEAT_COUNT;
	if (!DecodeCheatRows(read, ctx, rows, &bad)) {
		c.fault = CheatImageFault::Shape;
		c.row   = bad;
		return c;
	}
	for (uint8_t id = 0; id < CHEAT_COUNT; ++id) {
		if (rows[id].length == 0 || rows[id].length > KEYBOARD_CHEAT_STRING_LEN) {
			c.fault = CheatImageFault::Length;
			c.row   = id;
			return c;
		}
		if (!readable(ctx, rows[id].string, rows[id].length)) {
			c.fault = CheatImageFault::Unreadable;
			c.row   = id;
			return c;
		}
	}
	TypedCheatTable live;
	if (!TableFromRows(rows, read, ctx, &live, &bad)) {
		c.fault = CheatImageFault::Length;   // already ruled out above
		c.row   = bad;
		return c;
	}
	// From here the chat key can go by what is running, whatever the detour
	// decides.
	c.table     = live;
	c.tableRead = true;

	for (size_t i = 0; i < sizeof(CPAD_ADD_TO_PC_CHEAT_STRING_PROLOGUE); ++i)
		if (read(ctx, static_cast<uint32_t>(CPad__AddToPCCheatString + i)) !=
		    CPAD_ADD_TO_PC_CHEAT_STRING_PROLOGUE[i]) {
			c.fault = CheatImageFault::Prologue;
			return c;
		}
	for (size_t i = 0; i < sizeof(CPAD_CHEAT_SHIFT); ++i)
		if (read(ctx, static_cast<uint32_t>(CPad__CheatShiftBegin + i)) != CPAD_CHEAT_SHIFT[i]) {
			c.fault = CheatImageFault::Shift;
			return c;
		}

	bool rewritten = false;
	for (uint8_t id = 0; id < CHEAT_COUNT; ++id) {
		const CheatSite       &site = CHEAT_SITES[id];
		const DecodedCheatRow &row  = rows[id];
		if (row.handler != static_cast<uint32_t>(site.handler)) {
			c.fault = CheatImageFault::Row;
			c.row   = id;
			return c;
		}
		if (row.length == site.length && row.string == static_cast<uint32_t>(site.string) &&
		    RowStringIs(read, ctx, row.string, row.length, site.reversed))
			continue;
		const uint8_t known = KnownRewriteOf(id, row, read, ctx);
		if (known == 0) {
			c.fault = CheatImageFault::Row;
			c.row   = id;
			return c;
		}
		c.rewritten[id] = known;
		rewritten       = true;
	}
	c.verdict = rewritten ? CheatImageVerdict::Known : CheatImageVerdict::Retail;
	return c;
}

// ---- CoopIII's own cheats ----------------------------------------------------------
//
// Typed the way the game's are, anywhere in play, and matched against the same
// buffer once the engine has pushed the key: a word in capitals and, for one
// that takes it, one digit after it. TPTO3 is the word TPTO and the digit 3.
//
// To add one: a CoopCheatId before COOP_CHEAT_COUNT; its row in COOP_CHEATS,
// the word and the digits it takes ('\0' and '\0' for none); and what it does,
// in game/tpto.cpp's RunCoopCheat. tools/clienttest types each one with the
// chat key in front of it, and checks that no word ends one of the game's 23
// or is ended by one, which would fire both on one key.
enum CoopCheatId : uint8_t {
	COOP_CHEAT_TPTO  = 0,   // TPTO<n>: beside the player the Tab list numbers n
	COOP_CHEAT_COUNT = 1,
};

struct CoopCheatDef {
	const char *word;    // capitals, as typed
	char        argLo;   // the digits it takes, or '\0' for none
	char        argHi;
};

constexpr CoopCheatDef COOP_CHEATS[COOP_CHEAT_COUNT] = {
    {"TPTO", '1', '8'},
};

struct CoopCheatHit {
	uint8_t id  = COOP_CHEAT_COUNT;
	char    arg = '\0';
};

inline size_t CoopWordLength(uint8_t id) {
	size_t n = 0;
	while (COOP_CHEATS[id].word[n] != '\0')
		++n;
	return n;
}

// Keys to type it, digit included.
inline size_t CoopCheatTypedLength(uint8_t id) {
	return CoopWordLength(id) + (COOP_CHEATS[id].argLo != '\0' ? 1 : 0);
}

// Does the buffer, newest key first, end in one of them? The first that does.
inline bool MatchCoopCheat(const char *buffer, CoopCheatHit *hit) {
	for (uint8_t id = 0; id < COOP_CHEAT_COUNT; ++id) {
		const CoopCheatDef &d   = COOP_CHEATS[id];
		size_t              at  = 0;
		char                arg = '\0';
		if (d.argLo != '\0') {
			if (buffer[0] < d.argLo || buffer[0] > d.argHi)
				continue;
			arg = buffer[0];
			at  = 1;
		}
		const size_t len = CoopWordLength(id);
		if (at + len > KEYBOARD_CHEAT_STRING_LEN)
			continue;
		bool same = true;
		for (size_t i = 0; i < len && same; ++i)
			same = buffer[at + i] == d.word[len - 1 - i];
		if (!same)
			continue;
		if (hit) {
			hit->id  = id;
			hit->arg = arg;
		}
		return true;
	}
	return false;
}

// Everything a buffer completes, the game's and ours, and the fewest keys any
// of them takes to type.
struct TypedMatch {
	uint8_t      engine[CHEAT_COUNT] = {};
	uint8_t      engineCount         = 0;
	CoopCheatHit coop;
	bool         hasCoop             = false;
	size_t       shortest            = 0;

	bool Any() const { return engineCount != 0 || hasCoop; }
};

inline TypedMatch MatchTyped(const TypedCheatTable &t, const char *buffer) {
	TypedMatch m;
	size_t     shortest = KEYBOARD_CHEAT_STRING_LEN + 1;
	for (uint8_t id = 0; id < CHEAT_COUNT; ++id) {
		const TypedRow &row = t.rows[id];
		if (!ReversedMatches(row.reversed, row.length, buffer))
			continue;
		m.engine[m.engineCount++] = id;
		const size_t n            = RowTypedLength(row);
		if (n < shortest)
			shortest = n;
	}
	if (MatchCoopCheat(buffer, &m.coop)) {
		m.hasCoop      = true;
		const size_t n = CoopCheatTypedLength(m.coop.id);
		if (n < shortest)
			shortest = n;
	}
	m.shortest = m.Any() ? shortest : 0;
	return m;
}

// ---- the chat key inside a cheat ------------------------------------------------

// The chat key is T, and eleven of the 23 cheats have a T in them, TPTO two.
// Opening the line swallows that T and every key after it, so TURTOISE could
// never be typed in a session. Two rules give it back, and neither lets chat
// text start a cheat of its own. Both take the table the running game has
// (TypedCheatTable), and CoopIII's own cheats with it.
//
// 1. **A chat key that finishes a cheat is the game's.** GESUNDHEIT's last
//    letter goes to the engine the way it always has, and the line does not
//    open. `key` is the character the engine would push for the chat key
//    (0 when it pushes none, and then there is nothing to rescue).
inline bool ChatKeyFinishesCheat(const TypedCheatTable &t, const char *buffer, char key) {
	if (key == '\0')
		return false;
	char copy[KEYBOARD_CHEAT_STRING_LEN];
	for (size_t i = 0; i < KEYBOARD_CHEAT_STRING_LEN; ++i)
		copy[i] = buffer[i];
	PushCheatChar(copy, key);
	return MatchTyped(t, copy).Any();
}

// 2. **A line that is exactly the rest of a cheat is the rest of that cheat.**
//    `buffer` is what the engine has seen, which is everything typed before
//    the chat key opened the line; `line` is the whole of what has been
//    typed into it since. Checked after every key that goes into the line, so
//    nobody presses Enter: if the chat key and that line, pushed in that
//    order, finish a cheat the engine has not seen finish yet, and that
//    cheat began at or before the chat key, then the line was never chat. It
//    is shut there and then, unsent, and the keys go to the game.
//
// "Began at or before the chat key" is what keeps words out: every key of the
// line has to be part of the cheat, so a cheat word anywhere in a sentence, or
// a line that is nothing but a whole cheat word, never matches - the line
// would have to be preceded, outside chat, by the cheat's first letters.
// Nothing typed along the way may fire anything either, so handing `keys` to
// the engine one by one fires that cheat on the last of them and nothing
// before it. A line that stops being the start of a cheat is chat, and stays
// open for Enter.
//
// True when it is one; `keys` gets what to push, the chat key first, `*count`
// how many, and `*what` (if given) what they finish.
inline bool CheatFinishedInChatLine(const TypedCheatTable &t, const char *buffer, char key,
                                    const char *line, char (&keys)[KEYBOARD_CHEAT_STRING_LEN],
                                    uint8_t *count, TypedMatch *what = nullptr) {
	*count = 0;
	if (key == '\0' || line == nullptr || line[0] == '\0')
		return false;

	// The engine sees letters as MapVirtualKey gives them, which is capitals,
	// and the digit row as its digits.
	size_t n = 0;
	keys[n++] = key;
	for (const char *p = line; *p != '\0'; ++p) {
		if (n == KEYBOARD_CHEAT_STRING_LEN)
			return false;   // longer than any cheat
		char c = *p;
		if (c >= 'a' && c <= 'z')
			c = static_cast<char>(c - 'a' + 'A');
		if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')))
			return false;   // no cheat has anything but letters and a digit
		keys[n++] = c;
	}

	char copy[KEYBOARD_CHEAT_STRING_LEN];
	for (size_t i = 0; i < KEYBOARD_CHEAT_STRING_LEN; ++i)
		copy[i] = buffer[i];
	for (size_t i = 0; i + 1 < n; ++i) {
		PushCheatChar(copy, keys[i]);
		if (MatchTyped(t, copy).Any())
			return false;
	}
	PushCheatChar(copy, keys[n - 1]);
	const TypedMatch m = MatchTyped(t, copy);
	// The line is n - 1 keys; the chat key sits at copy[n - 1].
	if (!m.Any() || m.shortest < n)
		return false;
	*count = static_cast<uint8_t>(n);
	if (what)
		*what = m;
	return true;
}

// ---- the decision -------------------------------------------------------------

struct CheatContext {
	bool    inSession = false;   // welcomed, and still connected
	// Holds the session's sky: the host, or the running mission's owner
	// (coopiii/sky.h). The only thing it decides is where a sky cheat runs.
	bool    isHost    = false;
	uint8_t rule      = CHEAT_RULE_SHARED;
};

enum class CheatVerdict : uint8_t {
	// No session. The engine's own function runs, untouched.
	Vanilla,
	// Run the handler here and tell nobody: the cheat is the typist's own, or
	// what it changes already travels by its usual path.
	RunHere,
	// Run the handler here, then put what it left behind on the wire for
	// everybody else to arrive at (CHEAT_ROUTE_EVERYONE). Also the host's own
	// sky cheat: Client sees the route and sends S_WorldState at once instead.
	RunHereAndSend,
	// Do not run it here. The host runs it and our sky follows the host's on
	// the next S_WorldState. Running it here as well would only show the new
	// sky until that packet, which carries the old one, puts it back.
	SendToHost,
	// The server's CheatRule says no.
	Refused,
};

inline CheatVerdict PlanTypedCheat(const CheatContext &ctx, uint8_t id) {
	if (!ctx.inSession)
		return CheatVerdict::Vanilla;
	if (!CheatAllowed(ctx.rule, id))
		return CheatVerdict::Refused;
	switch (CheatRouteOf(id)) {
	case CHEAT_ROUTE_HOST:
		return ctx.isHost ? CheatVerdict::RunHereAndSend : CheatVerdict::SendToHost;
	case CHEAT_ROUTE_EVERYONE:
		return CheatVerdict::RunHereAndSend;
	default:
		return CheatVerdict::RunHere;
	}
}

// Why a cheat was refused, for the one log line that says so.
inline const char *CheatRefusalReason(uint8_t rule) {
	return rule == CHEAT_RULE_OFF
	           ? "the server has cheats switched off (cheats = off)"
	           : "it changes the world and the server only allows cheats about "
	             "the player who types them (cheats = personal)";
}

// ---- the state a cheat leaves behind ------------------------------------------

// CTimer::ms_fTimeScale as a state byte, or false when it is not one the two
// time cheats could have produced - a mission's own slow motion, say. Exact
// comparisons on purpose: the handlers only ever multiply by 2.0 and 0.5,
// which are exact in binary floating point, so a scale that is not one of
// these five was not made by them.
inline bool TimeScaleToCheatState(float scale, uint8_t &state) {
	constexpr float kScales[] = {0.25f, 0.5f, 1.0f, 2.0f, 4.0f};
	for (uint8_t i = 0; i < 5; ++i)
		if (scale == kScales[i]) {
			state = i;
			return true;
		}
	return false;
}

// What this machine's engine holds for a routed cheat, in the state byte's
// terms. `timeScale`, `givePedsWeapons` and `fastTime` are the three globals
// as the engine has them. False when there is no comparable state: the skies
// and the riot are applied unconditionally, and a time scale nobody's cheat
// made is not ours to step.
struct EngineCheatState {
	float timeScale       = 1.0f;
	bool  givePedsWeapons = false;
	bool  fastTime        = false;
};

inline bool CurrentCheatState(uint8_t id, const EngineCheatState &engine,
                              uint8_t &state) {
	switch (id) {
	case CHEAT_WEAPONS_FOR_ALL:
		state = engine.givePedsWeapons ? 1 : 0;
		return true;
	case CHEAT_FAST_WEATHER:
		state = engine.fastTime ? 1 : 0;
		return true;
	case CHEAT_FAST_TIME:
	case CHEAT_SLOW_TIME:
		return TimeScaleToCheatState(engine.timeScale, state);
	case CHEAT_MAYHEM:
		state = 1;   // nothing to read: the handler is the whole state
		return true;
	default:
		state = 0;
		return true;
	}
}

// ---- the receiving side ---------------------------------------------------------

// How to bring this machine to what the typist's machine was left at, by
// calling the engine's own handlers rather than writing the globals: which
// handler, and how many times. `count` 0 means already there.
struct CheatSteps {
	uint8_t handler = CHEAT_COUNT;   // a CheatId; its CHEAT_SITES row
	uint8_t count   = 0;
	bool    refused = false;         // there is no way from here to there
};

inline CheatSteps PlanRoutedCheat(uint8_t id, uint8_t state,
                                  const EngineCheatState &engine) {
	CheatSteps steps;
	if (id >= CHEAT_COUNT || !IsValidCheatState(id, state)) {
		steps.refused = true;
		return steps;
	}
	switch (id) {
	case CHEAT_MAYHEM:
	case CHEAT_SUNNY:
	case CHEAT_CLOUDY:
	case CHEAT_RAINY:
	case CHEAT_FOGGY:
		// Idempotent: the threat table is written, not added to, and
		// ForceWeatherNow is three stores.
		steps.handler = id;
		steps.count   = 1;
		return steps;
	case CHEAT_WEAPONS_FOR_ALL:
	case CHEAT_FAST_WEATHER: {
		// A toggle, so it runs only if we are the other way.
		uint8_t now = 0;
		CurrentCheatState(id, engine, now);
		steps.handler = id;
		steps.count   = now == state ? 0 : 1;
		return steps;
	}
	case CHEAT_FAST_TIME:
	case CHEAT_SLOW_TIME: {
		uint8_t now = 0;
		if (!CurrentCheatState(id, engine, now)) {
			steps.refused = true;
			return steps;
		}
		// Each fast step doubles and each slow step halves, and neither ever
		// overshoots within 0.25..4, so the distance is the number of calls.
		if (state > now) {
			steps.handler = CHEAT_FAST_TIME;
			steps.count   = static_cast<uint8_t>(state - now);
		} else {
			steps.handler = CHEAT_SLOW_TIME;
			steps.count   = static_cast<uint8_t>(now - state);
		}
		return steps;
	}
	default:
		steps.refused = true;   // a local cheat never arrives on the wire
		return steps;
	}
}

// ---- the two exceptions inside the local cheats ---------------------------------

// GESUNDHEIT heals FindPlayerVehicle as well as the player, and that is the
// car the local player is *in*, passenger seat included. A passenger healing
// somebody else's car would be an observer writing the condition of a car
// another machine is deciding, until that machine's next snapshot took it
// back. So in a session the car half is kept only for the driver.
inline bool HealthCheatMayRepairCar(bool inSession, bool inVehicle, bool weDrive) {
	return !inSession || !inVehicle || weDrive;
}

// ScanForThreats for a ped CoopIII built answers "nothing". Such a ped is a
// copy of somebody the session owns elsewhere, and whatever it would do about
// a threat - flee, fight - its owner's machine decides and the stream shows.
inline bool MayScanForThreats(bool isRemotePlayer, bool isAmbientReplica) {
	return !isRemotePlayer && !isAmbientReplica;
}

// ---- the engine half (game/cheats.cpp) -------------------------------------------

// Detours CPad::AddToPCCheatString and CPed::ScanForThreats. The first only
// goes in when the running code passes CheckCheatImage: every row retail's or
// a rewrite KNOWN_CHEAT_ROWS has. Neither is fatal: without the first, cheats
// behave as they always have; without the second, a riot can move replicas on
// this machine until their owner's stream puts them back. Also reads the
// running code's own table (TypedCheatTable), which the chat key goes by
// whatever the check says, and the detour by when it is in.
bool InstallCheatHooks();
void RemoveCheatHooks();

// The chat line's half of the two rules above (game/chat.cpp). `vk` is the
// chat key: the character the engine's key handler would push for it, from
// the same MapVirtualKeyA(vk, 2) call it makes (0x00583D90), or 0.
char EngineCheatCharFor(int vk);
// Is the chat key about to finish a cheat in the engine's own buffer?
bool ChatKeyWouldFinishCheat(char key);
// If `line`, typed after the chat key, finishes a cheat, hands the chat key
// and the line to CPad::AddToPCCheatString one key at a time - the door every
// typed cheat comes through, so a session's rule and routing apply to it
// exactly as to one typed with the line shut - and returns true.
bool FinishCheatFromChatLine(char key, const char *line);

// CoopIII's own cheats. The engine's buffer is looked at after every key the
// game's key handler has had (game/chat.cpp, straight after the window
// procedure), and after the keys FinishCheatFromChatLine hands it: one that
// has changed and now ends in one of COOP_CHEATS is queued for
// game/tpto.cpp, which drains it once a frame. Nothing is hooked for it, so
// it works whether or not the detour above is in.
void NoticeTypedKeys();
uint8_t DrainCoopCheats(CoopCheatHit *out, uint8_t max);

// SetCheatSession, DrainLocalCheats and ApplyRoutedCheat.
void AddCheatsToBridge(WorldBridge &bridge);

} // namespace coopiii::game
