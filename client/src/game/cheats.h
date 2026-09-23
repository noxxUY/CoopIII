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
inline bool CheatSiteMatches(const CheatSite &site, const char *buffer) {
	for (uint8_t i = 0; i < site.length; ++i) {
		if (site.reversed[i] != buffer[i])
			return false;
		if (site.reversed[i] == '\0')
			return true;
	}
	return true;
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

// ---- the decision -------------------------------------------------------------

struct CheatContext {
	bool    inSession = false;   // welcomed, and still connected
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

// Detours CPad::AddToPCCheatString and CPed::ScanForThreats, and checks the
// 23 strings in the image against CHEAT_SITES first. Neither is fatal: without
// the first, cheats behave as they always have; without the second, a riot
// can move replicas on this machine until their owner's stream puts them back.
bool InstallCheatHooks();
void RemoveCheatHooks();

// SetCheatSession, DrainLocalCheats and ApplyRoutedCheat.
void AddCheatsToBridge(WorldBridge &bridge);

} // namespace coopiii::game
