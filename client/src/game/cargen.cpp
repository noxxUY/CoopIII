// The engine half of game/cargen.h.
#include "cargen.h"

#include "addresses.h"
#include "leadcheck.h"
#include "../client.h"
#include "../log.h"

#include <windows.h>

#include <cstring>

namespace coopiii::game {

namespace {

bool     g_taken     = false;
bool     g_seedValid = false;
uint32_t g_seed      = 0;
// Whether rand() is the code RAND_PROLOGUE has on record. Asked once, when the
// call is taken; without it the colour still comes from the seed and the rest
// is left to the engine.
bool     g_randKnown = false;

bool g_saidSeeded   = false;
bool g_saidNoRand   = false;

using InternalFn = void(__thiscall *)(void *);

bool Redirect(uintptr_t site, uintptr_t from, uintptr_t to) {
	if (!RelCallAt(Ptr<uint8_t>(site), site, from))
		return false;
	DWORD old = 0;
	if (!VirtualProtect(reinterpret_cast<void *>(site), 5, PAGE_EXECUTE_READWRITE, &old))
		return false;
	const int32_t rel = static_cast<int32_t>(to - (site + 5));
	std::memcpy(reinterpret_cast<void *>(site + 1), &rel, sizeof rel);
	VirtualProtect(reinterpret_cast<void *>(site), 5, old, &old);
	FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void *>(site), 5);
	return true;
}

// Which generator this is, or -1 for anything that is not one of the live ones.
int32_t GeneratorIndex(const void *generator) {
	const uintptr_t base = CTheCarGenerators__CarGeneratorArray;
	const uintptr_t at   = reinterpret_cast<uintptr_t>(generator);
	if (at < base || (at - base) % offs::SIZEOF_CARGENERATOR != 0)
		return -1;
	const uint32_t index = static_cast<uint32_t>((at - base) / offs::SIZEOF_CARGENERATOR);
	const int32_t  live  = Global<int32_t>(CTheCarGenerators__NumOfCarGenerators);
	return live > 0 && index < static_cast<uint32_t>(live) ? static_cast<int32_t>(index) : -1;
}

// The rand() state of this thread, or null when the code is not what the
// header has on record.
uint32_t *RandState() {
	if (!g_randKnown)
		return nullptr;
	void *const data = Func<void *(__cdecl *)(int32_t)>(CRT_GET_THREAD_DATA)(1);
	if (!data)
		return nullptr;
	return &Field<uint32_t>(data, CRT_RAND_STATE);
}

// The colour off the model's own table, the row the seed picks.
void PaintParkedCar(void *car, uint32_t roll) {
	const int32_t model = Field<int16_t>(car, offs::MODEL_INDEX);
	void *const   info  = model >= 0 ? VehicleModelInfo(static_cast<uint32_t>(model)) : nullptr;
	if (!info)
		return;
	const uint8_t count = Field<uint8_t>(info, offs::MODELINFO_NUM_COLOURS);
	if (count == 0 || count > 8)
		return;
	const uint8_t row = ParkedColourRow(roll, count);
	Field<uint8_t>(car, offs::VEH_COLOUR1) = Field<uint8_t>(info, offs::MODELINFO_COLOURS1 + row);
	Field<uint8_t>(car, offs::VEH_COLOUR2) = Field<uint8_t>(info, offs::MODELINFO_COLOURS2 + row);
}

void __fastcall GenerateParkedCar(void *generator, void * /*edx*/) {
	const int32_t index = g_seedValid ? GeneratorIndex(generator) : -1;
	if (index < 0) {
		Func<InternalFn>(CCarGenerator__DoInternalProcessing)(generator);
		return;
	}

	const uint32_t roll  = ParkedCarRoll(g_seed, static_cast<uint32_t>(index));
	uint32_t *const state = RandState();
	const uint32_t saved = state ? *state : 0;
	if (state)
		*state = roll;
	Func<InternalFn>(CCarGenerator__DoInternalProcessing)(generator);
	if (state)
		*state = saved;

	// Process only calls in while the handle is -1, so a handle now is the car
	// this call made.
	const int32_t handle = Field<int32_t>(generator, offs::CARGEN_VEHICLE_HANDLE);
	if (handle < 0)
		return;
	void *const car = Func<void *(__cdecl *)(int32_t)>(CPools__GetVehicle)(handle);
	if (!car)
		return;
	if (GeneratorRollsColour(Field<int16_t>(generator, offs::CARGEN_COLOUR1),
	                         Field<int16_t>(generator, offs::CARGEN_COLOUR2)))
		PaintParkedCar(car, roll);

	if (!g_saidSeeded) {
		g_saidSeeded = true;
		Log("cargen: parked car on generator %d rolled from the session's seed "
		    "(model %d, colours %u/%u)%s",
		    index, static_cast<int>(Field<int16_t>(car, offs::MODEL_INDEX)),
		    static_cast<unsigned>(Field<uint8_t>(car, offs::VEH_COLOUR1)),
		    static_cast<unsigned>(Field<uint8_t>(car, offs::VEH_COLOUR2)),
		    state ? "" : "; its extras, alarm and lock are this machine's own");
	}
}

void SetParkedSeedImpl(bool valid, uint32_t seed) {
	g_seedValid = valid;
	g_seed      = seed;
}

} // namespace

bool InstallCarGenSeed() {
	g_randKnown = std::memcmp(Ptr<uint8_t>(CGeneral__GetRandomNumber), RAND_PROLOGUE,
	                          sizeof RAND_PROLOGUE) == 0 &&
	              RelCallAt(Ptr<uint8_t>(CGeneral__GetRandomNumber + 3),
	                        CGeneral__GetRandomNumber + 3, CRT_GET_THREAD_DATA);
	if (!g_randKnown && !g_saidNoRand) {
		g_saidNoRand = true;
		Log("cargen: rand() at 0x%08X is not the code on record; a parked car's "
		    "colour follows the session, its extras, alarm and lock stay this "
		    "machine's own",
		    static_cast<unsigned>(CGeneral__GetRandomNumber));
	}
	if (!g_taken)
		g_taken = Redirect(CARGEN_INTERNAL_CALL, CCarGenerator__DoInternalProcessing,
		                   reinterpret_cast<uintptr_t>(&GenerateParkedCar));
	if (g_taken)
		Log("cargen: took the car generators' call at 0x%08X; parked cars follow the "
		    "session's seed", static_cast<unsigned>(CARGEN_INTERNAL_CALL));
	else
		Log("cargen: FAILED to take the call at 0x%08X; every machine rolls its own "
		    "parked cars", static_cast<unsigned>(CARGEN_INTERNAL_CALL));
	return g_taken;
}

void RemoveCarGenSeed() {
	if (g_taken && Redirect(CARGEN_INTERNAL_CALL, reinterpret_cast<uintptr_t>(&GenerateParkedCar),
	                        CCarGenerator__DoInternalProcessing))
		g_taken = false;
	g_seedValid = false;
}

void AddCarGenToBridge(WorldBridge &bridge) {
	bridge.SetParkedSeed = &SetParkedSeedImpl;
}

} // namespace coopiii::game
