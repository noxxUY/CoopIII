// The map's parked cars, rolled the same on every machine of a session.
//
// docs/protocol.md 1.23.4 is the design. What a reader of this file needs:
//
// Every car generator has a fixed model, place and heading, the map's own and
// the same everywhere. What each machine rolled for itself is the rest of the
// car: its paint, its extra components, whether its alarm is set and whether
// its doors are locked. Two players standing at one generator each saw their
// own car there, a red Kuruma on one screen and a blue one on the other, and
// the one locked on one screen opened on the other.
//
// Not the model, which is the generator's (CCarGenerator::DoInternalProcessing
// reads [ebp] and nothing else for it, 0x00542711 and 0x005429B6). The rest
// comes from two places, and each needs its own answer:
//
//   - the colour is CVehicleModelInfo::ChooseVehicleColour, a round robin over
//     the model's own table plus a tiebreak against whatever the local player
//     is driving (addresses.h, "the respray colour is not random"). No seed
//     reaches it. So the colour is picked here, from the session's seed and
//     the generator, off the same table, and written after the constructor -
//     the renderer reads m_currentColour1/2 every frame. Only where the
//     generator asks for a roll: DoInternalProcessing writes its own two
//     colours when both are set (0x005428D8 / 0x00542B36).
//   - the extras, the alarm and the door lock are rand(): ChooseComponent's
//     rolls inside the constructor, then two percentage rolls after CWorld::Add
//     (0x00542AA0, 0x00542AEA). rand() is the CRT's, and its state lives in
//     the thread's CRT data at +8 (RAND_PROLOGUE below). So for the length of
//     the one call, the state is the session's number for that generator, and
//     it is put back afterwards: the rest of the game draws exactly what it
//     would have drawn.
//
// What is left different, on purpose: WHETHER a generator has a car. That is
// each machine's own traffic budget (CCarCtrl::NumParkedCars against 10,
// 0x00542700), its own timers and its own blockage test, and deciding it for
// another machine would be spending that machine's budget. Two players who
// arrive together are both inside the same generators' range, so in practice
// they get the same cars; the one that is out of budget on one machine is
// missing there, not different.
//
// A car rolled before the seed arrived (the load before the connect) keeps
// what it rolled, until the generator makes another.
#pragma once

#include <cstdint>

namespace coopiii {
struct WorldBridge;
}

namespace coopiii::game {

// ---- addresses -----------------------------------------------------------------
//
// CCarGenerator::Process (0x00542BB0) calls DoInternalProcessing once, as
// `mov ecx,ebx / call 005426E0` at 0x00542BE1, and only while the generator's
// handle is -1. Nothing else in the image calls it. DoInternalProcessing
// itself is `__thiscall`, no arguments, plain `ret`.
constexpr uintptr_t CARGEN_INTERNAL_CALL = 0x00542BE3;

// CGeneral::GetRandomNumber (addresses.h, 0x005A41D0) is the CRT's rand(), and
// this is all of it that matters here:
//
//   005A41D0  53                push ebx
//   005A41D1  6A 01             push 1
//   005A41D3  E8 A8 0D 02 00    call 005C4F80          the thread's CRT data
//   005A41D8  59                pop ecx
//   005A41D9  8B 58 08          mov ebx,[eax+8]        its rand state
//   005A41DC  69 DB 6D 4E C6 41 imul ebx,ebx,41C64E6Dh
//   005A41E2  81 C3 39 30 00 00 add ebx,3039h
//   ...                         (the data again) mov [eax+8],ebx
//   005A41FB  8B 40 08          mov eax,[eax+8]
//   005A41FE  C1 E8 10          shr eax,10h
//   005A4201  25 FF 7F 00 00    and eax,7FFFh
//
// 0x005C4F80 is __cdecl with one argument, TlsGetValue on the CRT's slot and,
// given a non-zero argument, a first-time set-up of the thread's data. It is
// called here exactly as rand calls it, and only on the game thread, which has
// drawn thousands of numbers by the time a generator runs.
constexpr uintptr_t CRT_GET_THREAD_DATA = 0x005C4F80;
constexpr size_t    CRT_RAND_STATE      = 0x08;
constexpr uint8_t   RAND_PROLOGUE[22]   = {0x53, 0x6A, 0x01, 0xE8, 0xA8, 0x0D, 0x02, 0x00,
                                           0x59, 0x8B, 0x58, 0x08, 0x69, 0xDB, 0x6D, 0x4E,
                                           0xC6, 0x41, 0x81, 0xC3, 0x39, 0x30};

// ---- the engine half, game/cargen.cpp --------------------------------------------

// Takes the call. Not fatal: without it every machine rolls its own parked
// cars, which is how it was.
bool InstallCarGenSeed();
void RemoveCarGenSeed();
void AddCarGenToBridge(WorldBridge &bridge);

// ---- the parts that do not need a running game -----------------------------------

// One generator's number for this session. The same seed and generator give the
// same number everywhere; neighbouring generators get unrelated ones.
inline uint32_t ParkedCarRoll(uint32_t seed, uint32_t generator) {
	uint32_t h = seed ^ ((generator + 1u) * 0x9E3779B9u);
	h ^= h >> 16;
	h *= 0x7FEB352Du;
	h ^= h >> 15;
	h *= 0x846CA68Bu;
	h ^= h >> 16;
	return h;
}

// Does the engine roll this generator's colour, or write the generator's own?
// DoInternalProcessing writes its own only when neither is -1.
inline bool GeneratorRollsColour(int16_t colour1, int16_t colour2) {
	return colour1 == -1 || colour2 == -1;
}

// Which row of the model's colour table. `numColours` is the model info's
// m_numColours, at most eight; 0 means the model has none and the engine's
// own 0/0 stands.
inline uint8_t ParkedColourRow(uint32_t roll, uint8_t numColours) {
	if (numColours == 0)
		return 0;
	return static_cast<uint8_t>((roll >> 8) % numColours);
}

// What rand() does to its state, as the retail code above does it. For the
// test that one seed gives one sequence; the engine's own code is what runs.
inline uint16_t CrtRand(uint32_t &state) {
	state = state * 0x41C64E6Du + 0x3039u;
	return static_cast<uint16_t>((state >> 16) & 0x7FFF);
}

} // namespace coopiii::game
