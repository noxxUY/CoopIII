// What game/crowdrange.cpp needs from the engine: where the local player is
// for the population's purposes, the traffic generator's police decision, and
// the adrenaline pill's clock. Read out of the retail 1.0 gta3.exe with dumpbin
// /disasm on 2026-09-25; re3's World.cpp, CarCtrl.cpp and PlayerPed.cpp were
// the map for the names only. tools/clienttest/crowdrange.cpp reads every
// byte claimed below back out of the exe when it has one.
#pragma once

#include "addresses.h"

#include <cstddef>
#include <cstdint>

namespace coopiii::game {

// ---- where the population is centred -------------------------------------------
//
// __cdecl CVector *FindPlayerCentreOfWorld(int32 player). Both generators call
// it with PlayerInFocus pushed and pop it themselves (`push eax / call / ...
// pop ecx` at 0x00416628 and 0x004F4A3F), and read x, y, z off the result:
//
//   004A1170  cmp byte [0095CD8A],0 / je        CReplay::Mode playing?
//   004A117B  mov eax,6FAD2Ch / ret             the camera's position
//   004A1181  mov ebx,[esp+8] / imul ebx,13Ch   Players[player]
//   004A118B  mov eax,[ebx+9412F4h]             m_pRemoteVehicle
//             test / je / add eax,34h / ret     its matrix position
//   004A11A0  call FindPlayerVehicle / add 34h  or the player's car's
//   004A11B3  mov eax,[ebx+9412F0h] / add 34h   or the ped's
//
// **The last arm does not null-check the ped**: with no player it returns
// 0x34, so every caller here asks FindPlayerPed first.
constexpr uintptr_t FindPlayerCentreOfWorld = 0x004A1170;

// ---- the traffic generator's two calls ---------------------------------------
//
// CCarCtrl::GenerateRandomCars (0x00416580) is the only caller of
// GenerateOneRandomCar in the image, twice: the fifty-car burst after a
// level change and the one-a-frame call.
//
//   004165B3  call 004165F0                     in the `cmp bx,32h` loop
//   004165C8  call 004165F0                     the ordinary frame
constexpr uintptr_t GENERATE_ONE_RANDOM_CAR_CALLS[2] = {0x004165B3, 0x004165C8};

// CCarCtrl::LastTimeLawEnforcerCreated, uint32 on the engine clock. Read twice
// in the police decision below and written once, at 0x00417D2B, with
// m_snTimeInMilliseconds when the car just made was of class 11h (COPS).
constexpr uintptr_t CCarCtrl__LastTimeLawEnforcerCreated = 0x008F5FF0;

// The police decision, GenerateOneRandomCar after both gates, edi = the local
// player's CWanted:
//
//   0041675D  mov ebx,[edi+18h] / cmp 1 / jle   m_nWantedLevel > 1
//   00416767  movzx eax,[edi+12h]               m_MaximumLawEnforcerVehicles
//             cmp [008F1B38],eax / jge          NumLawEnforcerCars under it
//   00416773  mov al,[edi+11h] / mov cl,[edi+10h]
//             cmp cl,al / jae                   m_CurrentCops < m_MaxCops
//   0041677F  cmp 3 / jg -> police              more than three stars
//   00416784  cmp ebx,2 / jle                   three stars:
//   00416789  [008F5FF0] + 1388h / cmp [00885B48] / ja -> police   5 s since
//   0041679B  [008F5FF0] + 1F40h / cmp [00885B48] / jbe -> not     else 8 s
//   004167AD  call 004181F0                     the police model
//
// crowdrange.h, PoliceCarDue, is this written out.

// ---- the adrenaline pill -----------------------------------------------------
//
// The only store of 1/3 into CTimer::ms_fTimeScale in the image, in the
// player's own per-frame update (PlayerPed.cpp:467-490 is the map):
//
//   004F117E  cmp byte [ebp+57Ch],0 / je        m_bAdrenalineActive
//   004F118B  mov eax,[ebp+580h] / cmp now      m_nAdrenalineTime
//   004F11A7  mov [008F2C20],3F800000h          ran out: 1.0 again
//   004F11ED  mov [008F2C20],3EAAAAABh          still on: 1/3
//   004F11F7  ... [eax+24h] = 40000000h          walk/run anims at speed 2
//
// Ten bytes, none of them a flag write: the `je` after it tests the `cmp` at
// 0x004F11E8, so the store can be taken out without moving anything else.
constexpr uintptr_t ADRENALINE_SLOWDOWN_STORE = 0x004F11ED;
constexpr uint8_t   ADRENALINE_SLOWDOWN_BYTES[10] = {0xC7, 0x05, 0x20, 0x2C, 0x8F,
                                                     0x00, 0xAB, 0xAA, 0xAA, 0x3E};

// ---- the cops-on-foot gate's two calls (docs/wanted.md 6.1) --------------------
//
// CPopulation::AddToPopulation (addresses.h) makes a cop on foot only while
// `ms_nNumCop < m_MaxCops` (0x004F4A94), and a cop replica is a CCivilianPed
// counted in ms_nNumCivMale, so in `shared` every wanted machine made its own
// six beside everybody else's. Both callers pass four floats and pop them:
//
//   004F3AB5  call 004F4A00 / add esp,10h        CPopulation::Update, per frame
//   004F3B72  call 004F4A00 / add esp,10h        GeneratePedsAtStartOfGame,
//                                                the `cmp bx,32h` loop
//
// Both have folded the per-type counts into ms_nTotalPeds (0x0095CB50) a few
// instructions before, so raising ms_nNumCop for the call alone moves the cop
// gate and nothing else. ms_nNumCop is read nowhere else in AddToPopulation.
constexpr uintptr_t ADD_TO_POPULATION_CALLS[2] = {0x004F3AB5, 0x004F3B72};

// ---- building a cop (protocol.h, C_CopHandover) ---------------------------------
//
// __thiscall CCopPed::CCopPed(eCopType), ret 4, from COMMAND_CREATE_CHAR's
// PEDTYPE_COP arm (0x0043BAAE: push 558h / call CPed::operator new / push
// copType / call 004C11B0) and CPopulation::AddPed's (0x004F5395, the same):
//
//   004C11B8  push 6 / call 004C41C0              CPed::CPed(PEDTYPE_COP)
//   004C11C9  mov dword [eax],5F82A4h             the vtable
//   004C11D3  mov [eax+550h],ebx                  m_nCopType
//   004C11E4  jmp [eax*4+005F8268h]               0 -> model 1, 1 -> model 3,
//                                                 2 -> model 2, 3 -> model 4
//   004C1360  mov byte [eax+544h],0               m_bIsInPursuit
//   004C13DB  ret 4
//
// and ~CCopPed (0x004C13E0) re-stamps the vtable and calls ClearPursuit
// (0x004C28C0) before ~CPed, so a cop taken away lets go of the CWanted.
constexpr uintptr_t CCopPed__ctor       = 0x004C11B0;
constexpr uintptr_t CCopPed__vtable     = 0x005F82A4;
constexpr size_t    SIZEOF_COP_PED      = 0x558;
constexpr size_t    COP_PED_COP_TYPE    = 0x550;

// __thiscall CPed *CVehicle::SetUpDriver(), and SetupPassenger(int n), ret 4.
// The engine's own way of putting the right ped in a car it generates:
//
//   005520C3  mov eax,[ebx+1A4h] / test / je      pDriver already there: done
//   005520D0  cmp byte [ebx+1F4h],1 / jne         only a RANDOM_VEHICLE
//   005520DA  call 004F5800                       CPopulation::AddPedInCar(this)
//   005520EB  mov [eax+310h],ebx / RegisterReference
//   0055210B  mov byte [eax+314h],1               bInVehicle
//   00552118  mov dword [eax+224h],2Ch            PED_DRIVING
//
// SetupPassenger is the same into pPassengers[n] (+0x1A8 + 4n), and ends with
// `inc byte [ebp+1C8h]`, m_nNumPassengers. AddPedInCar switches on the car's
// model (`sub eax,61h / cmp eax,33h / jmp [eax*4+5FA9A0h]`): 116 -> CCopPed
// type 0, 117 -> 2, 107 -> 1, 122 and 123 -> 3 (game/wanted.h,
// CopTypeForCarModel), then CPopulation::AddPed, which calls CWorld::Add.
constexpr uintptr_t CVehicle__SetUpDriver    = 0x005520C0;
constexpr uintptr_t CVehicle__SetupPassenger = 0x00552160;

// ---- a pedestrian speaking (protocol.h, C_PedSpeech) ----------------------------
//
// __thiscall void CPed::ServiceTalking(), reached from CPed::ProcessControl
// and from nowhere else - two calls, both `mov ecx,<ped> / call 004E5870`:
//
//   004C8D1C  call 004E5870                       the dead ped's branch, behind
//                                                 ServiceTalkingWhenDead (0x004E5850)
//   004CB916  call 004E5870                       every other ped, every frame
//
// Inside it, the only DMAudio.PlayOneShot (0x004E596D) is followed by
//
//   004E5972  mov [ebp+520h],esi                  m_lastSoundStart = now
//   004E59EA  mov [ebp+528h],bx                   m_lastQueuedSound = the sound
//   004E59F1  mov word [ebp+52Ah],0A7h            m_queuedSound = SOUND_NO_SOUND
//
// and none of the three is written on the way out otherwise, so a changed
// m_lastSoundStart across the call is a line played, and +0x528 says which.
constexpr uintptr_t CPed__ServiceTalking       = 0x004E5870;
constexpr uintptr_t SERVICE_TALKING_CALLS[2]   = {0x004C8D1C, 0x004CB916};
constexpr size_t    PED_LAST_SOUND_START       = 0x520;
constexpr size_t    PED_LAST_QUEUED_SOUND      = 0x528;
constexpr size_t    PED_QUEUED_SOUND           = 0x52A;
constexpr uint16_t  SOUND_NO_SOUND             = 0xA7;

// __thiscall void CPed::Say(uint16 sound), ret 4 (0x004E5A59 and every other
// exit). For a ped that is not the player it gives up when the camera is more
// than 3 m under him or moving fast, then queues the sound if it outranks the
// one queued (`cmp bx,[ebp+52Ah] / jae`, 0x004E5B10) and its CommentWaitTime
// row allows it - a row read as `shl esi,4 / [esi+5F8EC0h]`, the sound id
// unbounded, which is why only PedSpeechSoundValid ids are ever passed.
constexpr uintptr_t CPed__Say = 0x004E5A10;

// ---- is the spot free: the generators' own test ----------------------------------
//
// __cdecl void CWorld::FindObjectsKindaColliding(CVector const &pos, float
// radius, bool 2dOnly, int16 *count, int16 max, CEntity **list, bool
// buildings, bool vehicles, bool peds, bool objects, bool dummies): eleven
// dwords, every caller `add esp,2Ch`. It walks the sector lists and counts
// what the sphere touches, so it only ever sees what is built here.
//
// Both generators ask it before they make anything, and nothing else:
//
//   00416B81  GenerateOneRandomCar, the spawn point:   radius [005EC8DC],
//             2D, count at [esp+13Eh], max 2, no list, vehicles and peds;
//             `cmp word [esp+13Eh],0 / jne 00417D3E`, give up
//   00417C28  the new car, radius + 20 (testForCollision): vehicles; a hit
//             deletes the car it has just built
//   00417C7F  the new car, its bounding radius: vehicles; the same
//   004EE2E3  CPedPlacement::IsPositionClearForPed (0x004EE2C0), radius
//             0.75 ([005FA03C]), vehicles and peds, `cmp word [esp+6],0`;
//             its one caller is CPopulation::AddToPopulation, at 0x004F5036
//
// None of the four passes a list, so the count is the whole answer.
constexpr uintptr_t CWorld__FindObjectsKindaColliding = 0x004B2A30;
constexpr uintptr_t CPedPlacement__IsPositionClearForPed = 0x004EE2C0;
constexpr uintptr_t AddToPopulation_IsPositionClearCall = 0x004F5036;
constexpr uintptr_t SPAWN_SPOT_TESTS[4] = {0x00416B81, 0x00417C28, 0x00417C7F, 0x004EE2E3};

} // namespace coopiii::game
