// The medic and the fire truck's water cannon, as game/emergency.cpp needs
// them. Read out of the retail 1.0 gta3.exe on 2026-09-25 with dumpbin
// /disasm; re3's EmergencyPed.cpp, Accident.cpp, WaterCannon.cpp and
// CAutomobile::FireTruckControl were the map for the names only.
// tools/clienttest/emergency.cpp reads every byte claimed below back out of
// the exe when it has one.
#pragma once

#include "addresses.h"

#include <cstddef>
#include <cstdint>

namespace coopiii::game {

// ---- the medic ---------------------------------------------------------------
//
// CEmergencyPed::ProcessControl switches on m_nPedType ([ebx+32Ch], minus 10h,
// table 0x005F835C): PEDTYPE_EMERGENCY calls MedicAI at 0x004C3064 when
// CPed::IsPedInControl (0x004CE6C0) says so or the ped is PED_DRIVING
// (`cmp [ebx+224h],2Ch`), PEDTYPE_FIREMAN calls FiremanAI at 0x004C3078. Both
// are __thiscall and take nothing else; neither is reached from anywhere else.
constexpr uintptr_t CEmergencyPed__MedicAI   = 0x004C30A0;
constexpr uintptr_t CEmergencyPed__FiremanAI = 0x004C3CE0;   // for the record

// CEmergencyPed's own members past CPed's 0x53C bytes. MedicAI's READY arm
// stores the accident's victim into the first (`mov eax,[esi] / mov
// [ebx+53Ch],eax` at 0x004C3385) and every arm ends in a store to the second
// (`mov dword [ebx+540h],...`).
namespace offs {
constexpr size_t EMERGENCY_REVIVED_PED = 0x53C;   // CPed *m_pRevivedPed
constexpr size_t EMERGENCY_STATE       = 0x540;   // EmergencyPedState
} // namespace offs

// The whole of a medic's revive, the PERFORM_CPR arm once the look timer runs
// out, with edi and every [ebx+53Ch] the patient:
//
//   004C3AF2  [edi+156h] shr 5 / and 1 / jne out   bBodyPartJustCameOff: no
//   004C3B03  mov [edi+2C0h],42C80000h             m_fHealth = 100.0f
//   004C3B13  mov [eax+224h],0                     m_nPedState = PED_NONE
//   004C3B23  mov [eax+228h],5                     m_nLastPedState = WANDER_PATH
//   004C3B33  call 004D0F20                        CPed::SetGetUp()
//   004C3B3E  [ebp+51h] and 0FEh / or 1            bUsesCollision = true
//   004C3B4E  push 2 / call 004C5A30               SetMoveState(PEDMOVE_WALK)
//   004C3B5B  call 004C5D80                        RestartNonPartialAnims()
//   004C3B66  [edx+157h] and 0EFh                  bIsPedDieAnimPlaying = false
//   004C3B7A  [ecx+15Bh] and 0FEh                  bKnockedUpIntoAir = false
//   004C3B8E  mov [eax+34Ch],0                     m_pCollidingEntity = nil
//
// The call at 0x004C3B33 is the only one of CPed::SetGetUp's five callers
// inside MedicAI, and the only place in the image a medic stands anybody up:
// the call site CoopIII takes to learn that a revive happened.
constexpr uintptr_t MEDIC_REVIVE_GETUP_CALL = 0x004C3B33;

namespace offs {
constexpr size_t  PED_FLAGS_D             = 0x157;
constexpr uint8_t PED_DIE_ANIM_PLAYING    = 0x10;   // PED_FLAGS_D bit 4
constexpr size_t  PED_FLAGS_H             = 0x15B;
constexpr uint8_t PED_KNOCKED_UP_INTO_AIR = 0x01;   // PED_FLAGS_H bit 0
constexpr uint8_t PED_BODY_PART_OFF       = 0x20;   // PED_FLAGS_C bit 5
constexpr size_t  PED_COLLIDING_ENTITY    = 0x34C;  // CEntity *m_pCollidingEntity
} // namespace offs

constexpr float    MEDIC_REVIVE_HEALTH     = 100.0f;
constexpr uint32_t MEDIC_REVIVE_LAST_STATE = PEDSTATE_WANDER_PATH;

// ---- the accidents a medic answers -------------------------------------------
//
// gAccidentManager is 20 CAccidents of 12 bytes: m_pVictim, then
// m_nMedicsAttending and m_nMedicsPerformingCPR. GetNextFreeAccident
// (0x004565A0) walks them `add edx,0Ch / cmp eax,14h` and ReportAccident
// writes the three at 0x004566E0 (victim), [esi+8] and [esi+4]. Every
// caller of either loads `mov ecx,87FD10h`.
constexpr uintptr_t gAccidentManager       = 0x0087FD10;
constexpr size_t    NUM_ACCIDENTS          = 20;
constexpr size_t    SIZEOF_CACCIDENT       = 12;
constexpr size_t    ACCIDENT_VICTIM        = 0x00;

// __thiscall void CAccidentManager::ReportAccident(CPed *), ret 4. Reached
// from CAccidentManager::Update (0x00456710, called at 0x0048C984 in
// CGame::Process) off the EVENT_INJURED_PED event and from nowhere else. It
// refuses, in this order: IsPlayer (0x004565DE), CharCreatedBy == MISSION_CHAR
// (`cmp byte [ebx+160h],2` at 0x004565E7), bRenderScorched ([ebx+52h] bit 4),
// bBodyPartJustCameOff ([ebx+156h] bit 5), !bAllowMedicsToReviveMe
// ([ebx+15Ah] bit 1), bIsInWater ([ebx+122h] bit 3), a victim it already has,
// a ped on a physical surface ([ebx+2ECh]) and one with something solid in
// the line from two metres below him down (CWorld::ProcessVerticalLine at
// 0x004566B7); then GetNextFreeAccident, the victim, RegisterReference and
// m_lastAccident (`mov [ebx+328h],esi`).
constexpr uintptr_t CAccidentManager__ReportAccident = 0x004565D0;

// __thiscall CAccident *CAccidentManager::FindNearestAccident(CVector pos,
// float *dist), `ret 10h`. Skips a victim that is MISSION_CHAR (`cmp byte
// [edi+160h],2 / je` at 0x004567A9) and one whose m_fHealth is not 0
// (0x004567B2). Five callers: CCarAI's ambulance stop (0x00414C45),
// CCarCtrl's ambulance dispatch (0x0041FCEA), and MedicAI three times - the
// medic in the ambulance deciding to get out, and the READY and
// DETERMINE_NEXT_STATE arms choosing a patient. Only MedicAI's are CoopIII's.
constexpr uintptr_t CAccidentManager__FindNearestAccident = 0x00456760;
constexpr uintptr_t MEDIC_FIND_ACCIDENT_CALLS[3] = {0x004C3192, 0x004C336C, 0x004C3545};

// A replica is MISSION_CHAR (population.cpp, SpawnAmbientReplica); both gates
// above read the one byte.
constexpr uint8_t CHAR_CREATED_BY_RANDOM_BYTE = 1;

// ---- the fire truck's water cannon -------------------------------------------
//
// CAutomobile::ProcessControl's model switch, `cmp eax,61h / jne` at
// 0x00531FEE, calls FireTruckControl at 0x00531FF7 - its only caller.
constexpr int16_t   FIRETRUCK_MODEL             = 97;

// FireTruckControl's two arms: the car FindPlayerVehicle (0x004A10C0) returns,
// whose pad's fire button aims and sprays (0x0052259A..0x005227E2), and any
// other car in STATUS_PHYSICS (`[ebx+50h] shr 3 / cmp 3` at 0x005227E7),
// whose AI turns the cannon to CFireManager::FindFurthestFire_NeverMindFireMen
// (0x00479430) and sprays while ((time >> 10) & 3) != 0 (0x005229B4). Both
// end on one call:
//
//   00522B18  push ecx               dir
//   00522B25  push eax / push ebx    pos, the car
//   00522B27  call 00522470 / add esp,0Ch
//
// CWaterCannons::UpdateOne(uint32 id, CVector *pos, CVector *dir), __cdecl,
// with the car as the id. The only call to UpdateOne in the image, so the
// only way anything sprays.
constexpr uintptr_t FIRE_TRUCK_CANNON_CALL   = 0x00522B27;
constexpr uintptr_t CWaterCannons__UpdateOne = 0x00522470;

// CWaterCannons::aCannons: three CWaterCannons of 0x19C, the array's own
// constructor call at 0x00522B40 (`push 3 / push 19Ch / ... push 8F2CA8h`).
// m_nId at +0, m_nCur (int16) at +4, m_nTimeCreated at +8, sixteen positions
// at +0Ch, sixteen velocities at +0CCh, sixteen used flags at +18Ch.
constexpr uintptr_t CWaterCannons__aCannons  = 0x008F2CA8;
constexpr size_t    NUM_WATER_CANNONS        = 3;
constexpr size_t    SIZEOF_CWATERCANNON      = 0x19C;

// __thiscall void CWaterCannon::Update_OncePerFrame(int16 index), from
// CWaterCannons::Update (0x00522510) alone. After moving its points it does
// the only two things the water ever does:
//
//   00521C74  call 00479DB0   gFireManager.ExtinguishPoint(one random live
//                             point, 3.0f): every ongoing CFire within three
//                             metres of it goes out
//   00521C8B  call 005220B0   PushPeds(), one frame in four
//
// Nothing in it, or in PushPeds, reads the vehicle pool: in retail the water
// cannon never touches a car.
constexpr uintptr_t CWaterCannon__Update_OncePerFrame = 0x00521B80;
constexpr uintptr_t WATER_CANNON_EXTINGUISH_CALL      = 0x00521C74;
constexpr uintptr_t CFireManager__ExtinguishPoint     = 0x00479DB0;
constexpr uintptr_t WATER_CANNON_PUSH_PEDS_CALL       = 0x00521C8B;
constexpr uintptr_t CWaterCannon__PushPeds            = 0x005220B0;

// PushPeds walks the ped pool ([008F2C60h], 0x5F0 a slot) and, for a ped
// within 5 m^2 of a live point, does exactly this and moves on to the next:
//
//   00522343  call CPed::GetLocalDirection(&(1,0))   the side it is hit from
//   0052234C  [ebp+154h] and 0FEh                    bIsStanding = false
//   00522378  call CPhysical::ApplyMoveForce(0,0,2*step)
//   0052237D  m_vecMoveSpeed.x, .y ([ebp+78h], [ebp+7Ch]) toward the jet
//   005223C1  call CPed::SetFall(2000, 19h + side, 0)
//   005223D0  call CFire::Extinguish on [ebp+4B4h], if the ped burns
//
// Four calls, each the only one of its kind in PushPeds, so each is a call
// site CoopIII can take for a ped this machine does not own.
constexpr uintptr_t PUSH_PEDS_SIDE_CALL       = 0x00522343;
constexpr uintptr_t PUSH_PEDS_FORCE_CALL      = 0x00522378;
constexpr uintptr_t PUSH_PEDS_FALL_CALL       = 0x005223C1;
constexpr uintptr_t PUSH_PEDS_EXTINGUISH_CALL = 0x005223D0;

// ---- firemen on foot ---------------------------------------------------------
//
// FiremanAI (0x004C3CE0..0x004C3EB0) calls FindNearestFire three times,
// SetSeek, SetMoveState, SetIdle, Say, SetWanderPath and GetRandomNumber, and
// touches a fire once: `dec dword [eax+28h]`, m_nFiremenPuttingOut, at
// 0x004C3E66. It never calls CFire::Extinguish and never writes a fire's
// m_nExtinguishTime, so a fireman on foot puts nothing out in retail - he runs
// to the fire and stands there. His run and his stand are his host's ped rows
// like any pedestrian's.

} // namespace coopiii::game
