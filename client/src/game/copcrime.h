// Hurting a policeman another machine hosts.
//
// Every place the engine hurts a pedestrian picks the event with the same
// test on the victim, `cmp dword [victim+32Ch],6` (addresses.h,
// PED_EVENT_SITES), and when the local player did it RegisterEvent hands the
// event to CEventList::ReportCrimeForEvent. Somebody else's policeman is a
// CCivilianPed made as PEDTYPE_CIVMALE here (population.cpp,
// SpawnAmbientReplica), so every one of those tests says civilian: 30 chaos
// and at most one star for a shot where single player gives 80 and two.
//
// The replica stays a civilian. A ped of type 6 is treated as a CCopPed all
// over the engine: CWorld::CallOffChaseForArea's ped sweep (0x004B5B55) hands
// one to CCopPed::ClearPursuit (0x004C28C0), which trusts the byte at +0x544 -
// past the end of a 0x53C CCivilianPed, so whatever the slot's last occupant
// left there - and on a 1 takes a cop off the local player's m_pCops. It
// would also land in ms_nNumCop and pick up the police threat table. So
// the host's word is used where the decision is actually made instead: at
// ReportCrimeForEvent, the event is swapped for its police twin when the
// victim is a replica whose host made it PEDTYPE_COP. That covers the
// crime, the queue and both floors the function sets, and nothing else
// about the replica changes.
//
// Whether a policeman saw it is a different question and needs nothing:
// CWanted::WorkOutPolicePresence goes by model, so the replica is a witness.
//
// Only the local player's crimes reach ReportCrimeForEvent, so the host
// (whose real CCopPed takes the forwarded hit from a remote player's ped)
// reports nothing, and a kill is counted once, on the killer's machine, as
// the crime its killing blow was. The engine has no separate kill crime.
#pragma once

#include "addresses.h"

#include <cstdint>

namespace coopiii::game {

// The police twin of a pedestrian event, or the event unchanged.
constexpr int32_t CopEventFor(int32_t event) {
	switch (event) {
	case EVENT_ASSAULT:         return EVENT_ASSAULT_POLICE;
	case EVENT_HIT_AND_RUN:     return EVENT_HIT_AND_RUN_COP;
	case EVENT_SHOOT_PED:       return EVENT_SHOOT_COP;
	case EVENT_PED_SET_ON_FIRE: return EVENT_COP_SET_ON_FIRE;
	default:                    return event;
	}
}

// Is this one of the four events a replica's type can get wrong? Cheap, so
// the hook only goes looking for a replica when it could matter.
constexpr bool IsCivilianPedEvent(int32_t event) {
	return CopEventFor(event) != event;
}

// The event the engine would have registered had the victim been what its
// host says it is.
constexpr int32_t EventAgainstReplica(int32_t event, uint8_t hostPedType) {
	return hostPedType == PEDTYPE_COP ? CopEventFor(event) : event;
}

// ReportCrimeForEvent's switch, event -> crime, read out of the table at
// 0x005F12C4. Index 0 and the no-crime arms are CRIME_NONE.
constexpr int8_t CRIME_FOR_EVENT[EVENT_LAST_REPORTED + 1] = {
    0,    // 0  NULL
    2,    // 1  ASSAULT            HIT_PED
    7,    // 2  RUN_REDLIGHT       RUN_REDLIGHT
    3,    // 3  ASSAULT_POLICE     HIT_COP
    1,    // 4  GUNSHOT            POSSESSION_GUN
    0,    // 5  INJURED_PED
    0,    // 6  DEAD_PED
    0,    // 7  FIRE
    6,    // 8  STEAL_CAR          STEAL_CAR
    10,   // 9  HIT_AND_RUN        RUNOVER_PED
    11,   // 10 HIT_AND_RUN_COP    RUNOVER_COP
    4,    // 11 SHOOT_PED          SHOOT_PED
    5,    // 12 SHOOT_COP          SHOOT_COP
    0,    // 13 EXPLOSION
    13,   // 14 PED_SET_ON_FIRE    PED_BURNED
    14,   // 15 COP_SET_ON_FIRE    COP_BURNED
    15,   // 16 CAR_SET_ON_FIRE    VEHICLE_BURNED
};

constexpr int32_t CrimeForEvent(int32_t event) {
	return event >= 0 && event <= EVENT_LAST_REPORTED ? CRIME_FOR_EVENT[event] : CRIME_NONE;
}

// ReportCrimeNow's chaos per crime at full sensitivity, off the floats its
// arms load (0x005F7790.. and 18.0 at 0x005F771C). POSSESSION_GUN's arm is
// the bare tail.
constexpr int16_t CHAOS_FOR_CRIME[CRIME_LAST + 1] = {
    0,     // 0  NONE
    0,     // 1  POSSESSION_GUN
    5,     // 2  HIT_PED
    45,    // 3  HIT_COP
    30,    // 4  SHOOT_PED
    80,    // 5  SHOOT_COP
    15,    // 6  STEAL_CAR
    10,    // 7  RUN_REDLIGHT
    5,     // 8  RECKLESS_DRIVING
    5,     // 9  SPEEDING
    18,    // 10 RUNOVER_PED
    80,    // 11 RUNOVER_COP
    400,   // 12 SHOOT_HELI
    20,    // 13 PED_BURNED
    80,    // 14 COP_BURNED
    20,    // 15 VEHICLE_BURNED
    500,   // 16 DESTROYED_CESSNA
};

constexpr int32_t ChaosForCrime(int32_t crime) {
	return crime >= 0 && crime <= CRIME_LAST ? CHAOS_FOR_CRIME[crime] : 0;
}

// The level ReportCrimeForEvent raises the player to on its way out, seen or
// not: 1 for hitting a policeman, 2 for shooting one.
constexpr int32_t StarFloorForEvent(int32_t event) {
	return event == EVENT_ASSAULT_POLICE ? 1 : event == EVENT_SHOOT_COP ? 2 : 0;
}

bool InstallCopCrimeHook();
void RemoveCopCrimeHook();

} // namespace coopiii::game
