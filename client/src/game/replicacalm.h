// Two things a remote player's copy used to do on its own that its owner
// never did: step aside when something bumps it, and, sat as a passenger,
// turn its head toward whoever walks or drives past.
#pragma once

#include <cstdint>

namespace coopiii::game {

// CPed::SetEvasiveStep(CEntity *reason, uint8 animType), __thiscall, ret 8.
// It opens `cmp [esi+224h],1Fh` (already PED_STEP_AWAY), then IsPedInControl
// (0x004CE6C0), then only refuses for animType 0 when the ped is not the
// player and bRespondsToThreats (byte C +0x156 bit 1) is clear:
//
//   004D30F3  call 004D48E0 / test al,al / jne 004D3108     IsPlayer
//   004D30FC  mov al,[esi+156h] / shr al,1 / and al,1 / jne  bRespondsToThreats
//   004D3108  test bl,bl / je 004D338C                      animType == 0: return
//
// SpawnRemote clears bRespondsToThreats, so the CCarCtrl calls (type 0,
// 0x004195C2 and its two neighbours) never move a remote player. These six
// pass 1 or 2 and do, every one with the stepping ped in ecx out of ebx:
//
//   004C9288, 004C9335   CPed::ProcessControl, a car coming at the ped (push 1)
//   004EBBA6, 004EBFB2   the ped-on-ped collision block before KillPedWithCar (push 1)
//   004EBC1B, 004EC3D3   the same block (push 2)
constexpr uintptr_t CPed__SetEvasiveStep = 0x004D30C0;
constexpr uintptr_t EVASIVE_STEP_SITES[] = {0x004C9288, 0x004C9335, 0x004EBBA6,
                                            0x004EBC1B, 0x004EBFB2, 0x004EC3D3};

// CPed::LookForSexyPeds / LookForSexyCars, __thiscall, no arguments. Both
// open `call IsPedInControl` then `cmp [ebx+224h],2Ch` (PED_DRIVING) and
// both arm m_lookTimer (+0x4CC) with 4000 and 10000 ms (0FA0h at 0x004D4F04,
// 2710h at 0x004D4F38; the cars' 9C40h price test at 0x004D5006). The
// passenger arm of ProcessControl calls them back to back:
//
//   004CB574  call 004D4DF0    LookForSexyPeds
//   004CB57B  call 004D4F50    LookForSexyCars
constexpr uintptr_t CPed__LookForSexyPeds = 0x004D4DF0;
constexpr uintptr_t CPed__LookForSexyCars = 0x004D4F50;
constexpr uintptr_t LOOK_PEDS_SITE        = 0x004CB574;
constexpr uintptr_t LOOK_CARS_SITE        = 0x004CB57B;

// Call-site redirections, not detours: six calls to CPed::SetEvasiveStep and
// the two look-around calls in CPed::ProcessControl's PED_DRIVING arm. Each
// is only rewritten while it still calls the engine's function. Not fatal.
bool InstallReplicaCalm();
void RemoveReplicaCalm();

} // namespace coopiii::game
