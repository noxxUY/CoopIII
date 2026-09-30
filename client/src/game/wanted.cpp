// The engine half of the wanted level. wanted.h has the design; this is the
// part that touches game memory, and it is four instructions' worth of it.
//
// Two operations and nothing else. There is no hook here, no detour, no
// suppression and nothing about the police - docs/wanted.md §4.2 is the
// measurement that says the police need none of it, because a cop ped is a
// RANDOM_CHAR and a police car a RANDOM_VEHICLE and game/population.cpp has
// been replicating both since the day it shipped.

#include "wanted.h"

#include "addresses.h"
#include "log.h"

namespace coopiii::game {

namespace {

void *PlayerPed() {
	return Func<void *(__cdecl *)()>(FindPlayerPed)();
}

// The local player's CWanted, or null when there is no player ped.
//
// Guarded rather than assumed. FindPlayerPed is
// `CWorld::Players[PlayerInFocus].m_pPed` and returns null in the menus,
// during a load and in the gap between a death and a respawn - and the two
// callers below run from the send tick, which is exactly the part of the
// frame that does not know which of those is happening.
void *LocalWanted() {
	void *const ped = PlayerPed();
	if (!ped)
		return nullptr;
	return Field<void *>(ped, offs::PLAYER_PED_WANTED);
}

// Said once. A CWanted that is null while a player ped exists would mean the
// class layout is wrong, which is worth a line even though every read below
// survives it.
bool g_saidNoWanted = false;

bool SampleLocalWantedLevel(uint8_t &level) {
	void *const wanted = LocalWanted();
	if (!wanted) {
		if (PlayerPed() && !g_saidNoWanted) {
			g_saidNoWanted = true;
			Log("wanted: the player ped has no CWanted at +%X - the wanted "
			    "level will not be synced (and this will not be said again)",
			    static_cast<unsigned>(offs::PLAYER_PED_WANTED));
		}
		return false;
	}

	int32_t raw = Field<int32_t>(wanted, offs::WANTED_LEVEL);
	// The engine's own range, not a guess about it. A value outside it means
	// the offset is wrong, and clamping is what stops that becoming a level
	// written back into the engine and a bad number on the wire.
	if (raw < 0)
		raw = 0;
	if (raw > WANTED_LEVEL_CEILING)
		raw = WANTED_LEVEL_CEILING;
	level = static_cast<uint8_t>(raw);
	return true;
}

// Through CPlayerPed::SetWantedLevel, the function ALTER_WANTED_LEVEL (269)
// and CLEAR_WANTED_LEVEL (272) both call - not by writing m_nWantedLevel.
//
// Writing the field directly would set the stars and leave m_nChaos where it
// was, so CWanted::UpdateWantedLevel would put the old level straight back
// the next time anything touched it, and the queued crimes would still be
// there. The engine's own entry point does ClearQdCrimes, sets the chaos to
// the bracket, and recomputes m_MaxCops, m_MaximumLawEnforcerVehicles and
// m_RoadblockDensity - which are what the two police generators actually read
// (addresses.h, CPopulation::AddToPopulation and
// CCarCtrl::GenerateOneRandomCar).
void WriteLocalWantedLevel(uint8_t level) {
	void *const ped = PlayerPed();
	if (!ped)
		return;
	if (level > WANTED_LEVEL_CEILING)
		level = WANTED_LEVEL_CEILING;
	using SetFn = void(__thiscall *)(void *, int32_t);
	Func<SetFn>(CPlayerPed__SetWantedLevel)(ped, static_cast<int32_t>(level));
}

} // namespace

void InstallWantedBridge(WorldBridge &b) {
	b.SampleLocalWantedLevel = &SampleLocalWantedLevel;
	b.WriteLocalWantedLevel  = &WriteLocalWantedLevel;
}

} // namespace coopiii::game
