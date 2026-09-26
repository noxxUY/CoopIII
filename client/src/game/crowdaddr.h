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

} // namespace coopiii::game
