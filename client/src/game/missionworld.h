// What the owner's mission does to the world around everybody, and says out
// loud, as far as a participant's machine has to know the engine to follow it
// (mission-audit.md R15, docs/protocol.md 1.29). The instructions themselves
// are on game/replay.h's list; this is what mission.cpp has to read, write or
// call around them.
//
// Every address and layout here was read out of the retail 1.0 gta3.exe
// (reference/bin, 2026-09-25), and tools/clienttest/missions.cpp checks each
// one against the image when COOPIII_GTA3_EXE names one. The witness is beside
// each.
#pragma once

#include "addresses.h"

#include <cstddef>
#include <cstdint>

namespace coopiii::game::world {

// ---- the instructions ----------------------------------------------------------------

namespace op {
constexpr uint16_t CLEAR_AREA                  = 0x0395;
constexpr uint16_t SET_PED_DENSITY_MULTIPLIER  = 0x03DE;
constexpr uint16_t SET_CAR_DENSITY_MULTIPLIER  = 0x01EB;
constexpr uint16_t SET_ZONE_CAR_INFO           = 0x0152;
constexpr uint16_t SET_ZONE_PED_INFO           = 0x015C;
constexpr uint16_t SET_GANG_WEAPONS            = 0x0237;
constexpr uint16_t SET_THREAT_FOR_PED_TYPE     = 0x03F1;
constexpr uint16_t CLEAR_THREAT_FOR_PED_TYPE   = 0x03F2;
constexpr uint16_t LOAD_MISSION_AUDIO          = 0x03CF;
constexpr uint16_t HAS_MISSION_AUDIO_LOADED    = 0x03D0;
constexpr uint16_t PLAY_MISSION_AUDIO          = 0x03D1;
constexpr uint16_t SET_MISSION_AUDIO_POSITION  = 0x03D7;
constexpr uint16_t CLEAR_MISSION_AUDIO         = 0x040D;
constexpr uint16_t ADD_PAGER_MESSAGE           = 0x014D;
constexpr uint16_t PRINT_STRING_IN_STRING      = 0x0384;
constexpr uint16_t ADD_BLIP_FOR_CHAR_OLD       = 0x0162;
constexpr uint16_t ADD_SPHERE                  = 0x03BC;
constexpr uint16_t REMOVE_SPHERE               = 0x03BD;
constexpr uint16_t DRAW_CORONA                 = 0x024F;
constexpr uint16_t LOAD_SCENE                  = 0x03CB;
constexpr uint16_t START_CREDITS               = 0x0434;
constexpr uint16_t STOP_CREDITS                = 0x0435;
constexpr uint16_t RESTART_CRITICAL_MISSION    = 0x0255;
} // namespace op

// ---- the streets --------------------------------------------------------------------

// float. SET_PED_DENSITY_MULTIPLIER's handler (0x0044F730) is `fld
// [ScriptParams] / fstp [005FA56Ch]`, SET_CAR_DENSITY_MULTIPLIER's (0x004426FA)
// the same into 0x005EC8B4 (addresses.h has both with their readers). Neither
// is in a save, and nothing sets them but the script: a participant's machine
// keeps what they were before the owner's mission changed them, and puts them
// back when it ends, or when its own game starts over under it.
constexpr uintptr_t PED_DENSITY = CPopulation__PedDensityMultiplier;
constexpr uintptr_t CAR_DENSITY = CCarCtrl__CarDensityMultiplier;

// CTheZones::ZoneInfoArray: CZoneInfo[100], 0x3A bytes each. CTheZones::Init
// (0x004B5ECF) walks it from 0x00714400 in steps of 3Ah and stops at `cmp
// cx,64h`; SetZoneCarInfo (0x004B6A50, what 0152's handler calls at
// 0x0043F346) picks its entry with `imul edi,edi,3Ah / add edi,714400h`.
// Plain numbers, nothing pointing anywhere, and in a save: a participant's
// machine keeps a copy from before the owner's mission's first zone
// instruction ran here, for when the mission never got to put them back.
constexpr uintptr_t ZONE_INFO_ARRAY = 0x00714400;
constexpr size_t    ZONE_INFO_SIZE  = 0x3A;
constexpr size_t    ZONE_INFOS      = 100;
constexpr size_t    ZONE_INFO_BYTES = ZONE_INFO_SIZE * ZONE_INFOS;

// CGangs::Gang: CGangInfo[9], 0x10 bytes each. The array's constructor and
// destructor loops (0x004C4160, 0x004C4190) are `push 9 / push 10h / ...
// push 6EDF78h`. SetGangWeapons (0x004C4030, what 0237's handler calls at
// 0x00443F47) is `movsx ecx,[esp+4] / shl ecx,4 / add ecx,6EDF78h`, then the
// two weapons into +8 and +0Ch.
constexpr uintptr_t GANGS         = 0x006EDF78;
constexpr size_t    GANG_SIZE     = 0x10;
constexpr size_t    GANG_COUNT    = 9;
constexpr size_t    GANG_WEAPON_1 = 0x08;
constexpr size_t    GANG_WEAPON_2 = 0x0C;

// CPedType::ms_apPedType (addresses.h): one CPedType* per ePedType, 23 of
// them, m_threats at +18h. SET_THREAT_FOR_PED_TYPE's handler (0x00588677) is
// `mov ebp,[eax*4+00941594h] / mov eax,[ebp+18h] / or eax,[006ED464h]`, and
// CLEAR_THREAT_FOR_PED_TYPE's (0x005886A7) the same with `not / and`.
constexpr size_t PED_TYPES = 23;

// ---- what is drawn ------------------------------------------------------------------

// CTheScripts::ScriptSphereArray: 16 of 0x18 bytes. AddScriptSphere
// (0x0044FB30) finds a free one with `cmp byte [eax+727D60h],0 ... add eax,18h
// ... cmp ecx,10h`, then writes 1 at +0, the id at +4, the place at +8 and
// the radius at +14h. The handle is the slot in its low word and a count
// kept at +2 in its high (GetActualScriptSphereIndex, 0x0044FA80). The id is
// the script's address plus its ip, which for our runner is the same for
// every sphere, and CTheScripts::DrawScriptSpheres hands it to the 3D markers
// as the marker's id: two of ours would share one marker. So the participant
// writes an id of its own over it.
constexpr uintptr_t SCRIPT_SPHERES    = 0x00727D60;
constexpr size_t    SCRIPT_SPHERE     = 0x18;
constexpr size_t    SCRIPT_SPHERE_MAX = 16;
constexpr size_t    SPHERE_IN_USE     = 0x00;   // bool
constexpr size_t    SPHERE_ID         = 0x04;   // uint32
// Ours, with a top bit no script's `this + ip` has.
constexpr uint32_t  SHOWN_SPHERE_ID   = 0xA0000000u;

inline int32_t SphereSlot(int32_t handle) {
	if (handle == -1)
		return -1;
	const int32_t slot = handle & 0xFFFF;
	return slot < static_cast<int32_t>(SCRIPT_SPHERE_MAX) ? slot : -1;
}

// CCoronas::RegisterCorona(id, r, g, b, alpha, const CVector &, size,
// drawDistance, type, flare, reflection, LOScheck, drawStreak, angle), cdecl:
// DRAW_CORONA's handler (0x004447E9) pushes fourteen and calls it at
// 0x004448B3, then `add esp,38h`. It passes 255 for alpha, 150.0 for the draw
// distance ([005EF1C8h]), 1 for the reflection, 0 and 0, and 0.0 for the
// angle ([005EF17Ch]). A corona is registered for one frame at a time and
// fades out when it is not registered again.
constexpr uintptr_t CCoronas__RegisterCorona = 0x004FA080;
constexpr float     CORONA_DRAW_DISTANCE     = 150.0f;
using RegisterCoronaFn = void(__cdecl *)(uint32_t id, uint8_t r, uint8_t g, uint8_t b, uint8_t alpha,
                                         const float *pos, float size, float drawDistance, uint8_t type,
                                         uint8_t flare, uint8_t reflection, uint8_t losCheck,
                                         uint8_t drawStreak, float angle);
// Ours, as the blue markers' are (mission.cpp, SHOWN_MARKER_ID).
constexpr uint32_t SHOWN_CORONA_ID = 0xC0000000u;

// CWorld::FindGroundZForCoord(float x, float y), cdecl, the result in st0:
// what DRAW_CORONA (0x00444835), CLEAR_AREA (0x0044D89C) and the critical
// restart (0x00444A3F) call for a z at or below -100, two pushes, `fstp`,
// two pops.
constexpr uintptr_t CWorld__FindGroundZForCoord = 0x004B3A80;
using FindGroundZFn = float(__cdecl *)(float x, float y);

} // namespace coopiii::game::world
