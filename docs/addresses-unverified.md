# Located but NOT verified

These came out of the same IDA analysis pass as `client/src/game/addresses.h`,
but nobody ever went back and checked them. They are leads, not facts. Nothing
here may be copied into `addresses.h` until it has been checked against
`reference/re3` by someone who tries to disprove it.

The split exists so a guess and a proof do not get to look alike. Resist the
urge to "just try one". A wrong address doesn't fail cleanly; it corrupts an
unrelated function and kills the game somewhere else entirely
(`client/src/game/verify.h` explains why that matters).

To promote a symbol: confirm the address against re3's source for the same
function, check the disassembly actually matches what re3 says it does, then
move it into `addresses.h` and delete the row here.

All of it came out of one pass on 2026-09-21, against GTA III PC
v1.0 retail, MD5 85414BF9EB414D00AD81062360F0DB1F.

## REFUTED: do not use these

A refutation is worth as much as a confirmation, so these sit at the top where
nobody can miss them.

| Claim | Verdict | Evidence |
|---|---|---|
| `CStreaming::HasModelLoaded` exists as a callable function | **REFUTED** | re3 declares it inline; the compiler inlined it at 38+ sites. `addresses.h` inlines the comparison instead. |
| `CRecordDataForChase::StoreInfoForCar` is at `0x00435140` | **REFUTED**, the real address is `0x00435000` | `0x00435140` is the *last byte* of `or byte ptr [esp+5],0Ch`, which starts at `0x0043513C`. Calling it would execute from the middle of an instruction. The region end in the old row (`0x0043525A`) was right (the `ret` is at `0x00435259`), so what got lost was a function *start*, not a bad range. Disassembling from `0x00435000` gives re3 Record.cpp:331-348 statement for statement: `127.0f * GetRight().x` first, then forward, then `pos`, then `16383.5f *` the three move-speed components, then wheel/gas/brake, then the handbrake bit. This is the third hand-derived address in this analysis pass to be flat wrong. |

`CRecordDataForChase::RestoreInfoForCar` at `0x00435330`, from the same row
pair, is correct: it opens `fld [ebp+0x3C] / [ebp+0x38] / [ebp+0x34]`
(`CVector oldPos = pCar->GetPosition()`) then `lea eax,[ebp+4] / push pState /
push eax`, which is `RestoreInfoForMatrix(pCar->GetMatrix(), pState)`, re3's
first two statements. `RestoreInfoForMatrix` itself is `0x00435260`.

## Already promoted (ignore the rows below for these)

Verified 2026-09-21 and now live in `client/src/game/addresses.h`. The ped rows
are still listed further down; the vehicle ones have been deleted from their
tables, so the section counts below no longer match their headings.
`addresses.h` is the source of truth either way.

| Symbol | How it was confirmed |
|---|---|
| `CPed::m_nPedState` `0x224`, `m_nLastPedState` `0x228`, `m_nMoveState` `0x22C` | `CPed::SetStoredState` (`0x004C5DB0`) reads all three in re3's statement order (Ped.cpp:614-623) |
| `CPed::m_fHealth` `0x2C0` | `CPed::SetDie` does `mov dword [ebx+0x2C0],0` between `ClearAll` and the `PED_DRIVING` test, matching re3 Ped.cpp:6318-6321 exactly |
| `CPed::bInVehicle` `0x314` | `cmp byte [ebx+0x314],0` in `SetDie`'s tail, where re3 tests `bInVehicle` |
| `CPed::m_fRotationCur` `0x2DC`, `m_fRotationDest` `0x2E0` | 33 and 55 stores plus 29 loads across `.text`, the profile of a heading the ped AI touches constantly; adjacent, as re3 Ped.h:444-445 declares them |
| `CPhysical::m_vecMoveSpeed` `0x78`, `m_vecTurnSpeed` `0x84` | arithmetic from re3: CPlaceable is vtable(4)+CMatrix(0x48), CEntity ends at 0x64, then the CPhysical members in declaration order |
| `CPlaceable::m_matrix` `0x04`, position `0x34`, `CEntity::m_modelIndex` `0x5C` | same arithmetic, and the RwMatrix layout it implies is self-consistent |
| `FindPlayerPed` `0x004A1150` | disassembles to `movzx PlayerInFocus` / `imul` by the CPlayerInfo stride / indexed load / `ret`, and is the call target at the head of `CPed::SetDie` |
| `CPed::SetStoredState` `0x004C5DB0`, `CPed::SetDie` `0x004D37D0` | each matches its re3 source statement for statement; they also call each other exactly where re3 does |
| `CPed::m_weapons` `0x35C`, `sizeof(CWeapon)` `0x18` | proved four times: `CPed::CPed` array-constructs 13 elements of 0x18 at +0x35C; `GiveWeapon`, `GetWeaponSlot` and `SetCurrentWeapon` each compute `type * 0x18 + 0x35C` independently |
| `CPed::m_currentWeapon` `0x498` | `CPed::SetCurrentWeapon` (`0x004CFA60`) writes `mov byte [ebp+498h],bl` between RemoveWeaponModel and AddWeaponModel, exactly where re3 assigns it. `GiveWeapon`'s `inc byte [esi+499h]` is the adjacent `m_maxWeaponTypeAllowed` |
| `CPed::m_pVehicleAnim` `0x1D8` | `CPed::CPed` nils it at `[eax+1D8h]`, between `m_animGroup` (0x1D4) and `m_vecAnimMoveDelta` (0x1DC) |
| `CPed::m_nPedType` `0x32C` | `CPed::CPed` writes its `pedType` argument to `[ecx+32Ch]` |
| `CPed::GiveWeapon` `0x004CF9B0` | the whole function is re3 Ped.cpp:4700-4720, including the 99999 ammo cap (`cmp eax,1869Fh`) and the OUT_OF_AMMO → READY reset |
| `CFont::InitPerFrame` `0x00500BE0` | held. It is re3 Font.cpp:105-112 instruction for instruction: `mov eax,[0095CC04h] / push 1Eh / call 0051EB70 / mov [008F31B4h],eax`, then the same two lines with `push 0Fh` for `Sprite[1]` and `Sprite[2]`, i.e. `Details.bank = CSprite2d::GetBank(30, Sprite[0].m_pTexture)` and two banks of 15. Promoted with the whole of the HUD font and sprite surface it belongs to; see the `---- the HUD ----` section of `addresses.h` |

### The `CVehicle` layout, verified 2026-09-21 (Area E)

All 22 vehicle candidates listed below were checked against the binary and all
22 survived. They are now in `client/src/game/addresses.h` with the proof for
each, and their rows have been deleted from the `pedlayout` table. None of them
was arithmetic from re3's declarations: every one is written or read by a named
function in the retail image, and most by three or more.

| Symbol | Value | Proved by |
|---|---|---|
| `CVehicle::pHandling` | `0x128` | `SET_CAR_CRUISE_SPEED` (opcode 173, `0x0043CCEC`): `mov eax,[ecx+0x128]` then `fmul dword [eax+0x88]` against `60.0f`, giving `60.0f * car->pHandling->Transmission.fMaxCruiseVelocity` (Script.cpp:3600). `+0x88` is exactly `Transmission.fMaxCruiseVelocity` in a `tHandlingData` whose total is re3's `0xD8`. |
| `CVehicle::AutoPilot` | `0x12C` | The inlined `CAutoPilot` ctor inside `CVehicle::CVehicle` writes eleven members at `0x12C`-`0x198` in re3's constructor order, ending with `m_pTargetCar` at `0x198`, the last of `0x70` bytes. `CREATE_CAR` writes seven more; `CCarCtrl::JoinCarWithRoadSystem` opens on two of them. |
| `CVehicle::m_currentColour1/2` | `0x19C` / `0x19D` | `CHANGE_CAR_COLOUR` (553, `0x00443B16`) writes both as bytes after the `>= 256` check and the `"CHANGE_CAR_COLOUR - Colours must be less than %d"` debug call, matching re3 Script3.cpp:470-475 exactly. Also read by `GET_CAR_COLOURS` (1011) and copied by `LoadVehiclePool`. |
| `CVehicle::pDriver` | `0x1A4` | `GET_DRIVER_OF_CAR` (1132, `0x0058B166`): `mov eax,[eax+0x1A4]`, null-check, `GetPedPool()->GetIndex`. Also the first thing `CanBeDeleted` reads. |
| `CVehicle::pPassengers` | `0x1A8` | `IS_CAR_PASSENGER_SEAT_FREE` (1073): `cmp dword [eax+esi*4+0x1A8],0`. `GET_CHAR_IN_CAR_PASSENGER_SEAT` (1074) loads the same. The ctor nils all eight in a loop bounded by `[+0x1CC]`. |
| `CVehicle::m_nNumPassengers` | `0x1C8` | `GET_NUMBER_OF_PASSENGERS` (489): `movzx eax, byte [eax+0x1C8]`. Zeroed by the ctor in re3's order. |
| `CVehicle::m_nNumMaxPassengers` | `0x1CC` | `GET_MAXIMUM_NUMBER_OF_PASSENGERS` (490), `IS_CAR_PASSENGER_SEAT_FREE` (1073), the ctor (`= 8`), the passenger-nil loop bound, and `CVehicle::SetModelIndex`. Five sites. |
| `CVehicle::m_fSteerAngle` | `0x1E8` | `StoreInfoForCar`: `fld 20.0f / fmul [ecx+0x1E8]` → `pState->wheel`. Also `RestoreInfoForCar` and `LoadVehiclePool`. |
| `CVehicle::m_fGasPedal` | `0x1EC` | same three, with `fld 100.0f` → `pState->gas`. |
| `CVehicle::m_fBrakePedal` | `0x1F0` | same three, with `fld 100.0f` → `pState->brake`. |
| `CVehicle::bEngineOn` | `0x1F5` bit 4 (`0x10`) | Set by the ctor (`and al,0EFh / or al,10h` where re3 has `bEngineOn = true`), cleared by `CREATE_CAR` (`and al,0EFh`), copied bit-for-bit by `LoadVehiclePool`. The bit *order* is verified rather than assumed, because `StoreInfoForCar` reads the handbrake out of the same byte as `shr dl,5 / and dl,1`, which is bit 5, the member re3 declares immediately after. |
| `CVehicle::m_fHealth` | `0x200` | `SET_CAR_HEALTH` (548) `fstp [eax+0x200]`, `IS_CAR_HEALTH_GREATER` (389) `fcomp [eax+0x200]`, ctor `= 0x447A0000` (1000.0f), `LoadVehiclePool` `fld`/`fstp`. |
| `CVehicle::m_nCurrentGear` | `0x204` | ctor `mov byte [ebx+0x204],1` (re3: `m_nCurrentGear = 1`), and `LoadVehiclePool` copies it as a *byte*. |
| `CVehicle::m_fChangeGearTime` | `0x208` | ctor `mov dword [eax+0x208],0` on the very next statement, and `LoadVehiclePool` copies it with `fld`/`fstp`, which settles the *type* as float. |
| `CVehicle::m_nDoorLock` | `0x224` | `CHANGE_CAR_LOCK` (309) and `LOCK_CAR_DOORS` (522) both `mov dword [eax+0x224], param`; ctor writes `1` = `CARLOCK_UNLOCKED`; `LoadVehiclePool` copies a full dword. Four sites, all 4-byte. |
| `CVehicle::m_bSirenOrAlarm` | `0x22E` | `SWITCH_CAR_SIREN` (919) writes `1`/`0` as a byte; the ctor zeroes it between `m_nCarHornPattern` (`0x22D`) and `m_nAlarmState` (`0x1A0`), in re3's order. |
| `CVehicle::m_aCollPolys` | `0x230` | The ctor emits a member array construction at `this+0x230` with count `2` and element size `0x28`, a `CStoredCollPoly[2]`, and `2 * 0x28` lands exactly on `m_fSteerInput` at `0x280`. |
| `CVehicle::m_vehType` | `0x284` | `FindPlayerTrain` (`0x004A1120`) calls `FindPlayerVehicle`, then `mov eax,[eax+0x284] / cmp eax,2`, which is `IsTrain()` with `VEHICLE_TYPE_TRAIN == 2` in re3's enum. |
| `sizeof(CVehicle)` | `0x288` | `CAutomobile::CAutomobile` constructs its own first member (`CDamageManager Damage`, which re3 annotates `// 0x288`) at `this+0x288`, right after `CVehicle::CVehicle` returns. |
| `sizeof(CAutomobile)` / pool stride | `0x5A8` | Three ways: `CREATE_CAR` does `push 5A8h` into `CVehicle::operator new`; the vehicle pool's constructor (`0x004A3580`, called from `CPools::Initialise` with 110 entries and stored to `0x009430DC`) does `imul eax, eax, 5A8h`; and `CPool::GetIndex` (`0x00429050`) divides by the magic constant `0x2D4279A3 >> 40`, and that constant is `ceil(2^40 / 1448)` and no other divisor. |
| `FindPlayerVehicle` | `0x004A10C0` | `movzx PlayerInFocus / imul 4Fh / indexed load / cmp byte [ecx+0x314],0 / mov eax,[ecx+0x310]`, matching re3 PlayerInfo.cpp:436-442 with the null check, statement for statement. |

Structural constants promoted alongside them: `VehicleCreatedBy 0x1F4`, the
`0x1F5`/`0x1F7` bitfield bytes, `m_aExtras 0x19E`, `m_nAlarmState 0x1A0`,
`m_nNumGettingIn 0x1C9`, `m_nGettingOutFlags 0x1CB`, `m_fSteerInput 0x280`,
`sizeof(CBoat) 0x484`, `sizeof(CAutoPilot) 0x70`,
`sizeof(CStoredCollPoly) 0x28`, and the `eVehicleCreatedBy` / `eVehicleType` /
`eCarLock` values the engine's own jump tables fix.

### Two places where re3's source is slightly off for this build

Neither affects a promoted offset; recorded so the next person doesn't lose an
afternoon to them.

- `CAutoPilot::m_nTimeToStartMission` and `m_nAntiReverseTimer` look swapped.
  re3 declares `m_nAntiReverseTimer` first (`+0x20`) then
  `m_nTimeToStartMission` (`+0x24`), and its constructor assigns
  `m_nTimeToStartMission = GetTimeInMilliseconds(); m_nAntiReverseTimer =
  m_nTimeToStartMission;`. The retail constructor writes `[+0x14C]` (`+0x20`)
  from the timer and *then* copies it to `[+0x150]` (`+0x24`), so in this build
  the timer-sourced field is at `+0x20` and the declaration order is reversed.
  Both are `uint32` and CoopIII needs neither, so nothing is promoted.
- `CPools::LoadVehiclePool` copies `[+0x216]` twice, not `m_nTimeOfDeath`.
  re3 flags a copy-paste bug there and renders it as `m_nTimeOfDeath` twice
  (`m_nTimeOfDeath` is a `uint32` at `+0x210`). The binary actually copies the
  16-bit field at `+0x216`, `m_nBombTimer`, twice, and never copies
  `m_nTimeOfDeath` at all. The doubled copy is a fingerprint; it identifies the
  function beyond argument.

## gameloop / game-loop (45 symbols)

| Symbol | Address | Kind | Confidence | re3 reference |
|---|---|---|---|---|
| `HOOK_INBOUND (CGame::Process + 5, immediately after CPad::UpdatePads returns)` | `0x0048C855` | function | high | src/core/Game.cpp:1004-1008 (CPad::UpdatePads(); then TheCamera.SetMotionBlurAlpha(0);) |
| `CGame::Process` | `0x0048C850` | function | high | src/core/Game.cpp:1002 |
| `Idle (the per-frame function)` | `0x0048E480` | function | high | src/core/main.cpp:1551 |
| `HOOK_OUTBOUND (Idle's call to DoRWStuffEndOfFrame, at the end of the frame before the present)` | `0x0048E6E4` | function | high | src/core/main.cpp:1741 (DoRWStuffEndOfFrame(); at the end of Idle) |
| `HOOK_OUTBOUND_ALT (after DoRWStuffEndOfFrame returns, i.e. after the present)` | `0x0048E6E9` | function | high | src/core/main.cpp:1741-1744 (between DoRWStuffEndOfFrame(); and if(g_SlowMode) ProcessSlow... |
| `HOOK_OUTBOUND_UNCONDITIONAL (Idle, immediately after CGame::Process returns)` | `0x0048E4A0` | function | high | src/core/main.cpp:1594-1599 (CGame::Process(); then DMAudio.Service();) |
| `CPad::UpdatePads` | `0x00492720` | function | high | src/core/Pad.cpp:1098 |
| `CPad::GetPad` | `0x00492F60` | function | high | src/core/Pad.h (static CPad *GetPad(int32 pad)) |
| `FrontendIdle` | `0x0048E700` | function | high | src/core/main.cpp:1753 |
| `AppEventHandler` | `0x0048E800` | function | high | src/core/main.cpp:1795 |
| `RsEventHandler` | `0x00584A20` | function | high | src/skel/skeleton.cpp (RsEventHandler) / src/skel/win/win.cpp:1033 |
| `WinMain` | `0x00582710` | function | high | src/skel/win/win.cpp (WinMain) |
| `WinMain main message loop (top of the do-while; PeekMessageA)` | `0x00582A10` | function | high | src/skel/win/win.cpp (WinMain's `while(!RsGlobal.quit && !FrontEndMenuManager.m_bStartGame... |
| `call site: RsEventHandler(rsIDLE, TRUE) in WinMain` | `0x00582EFD` | function | high | src/skel/win/win.cpp:1033 |
| `DoRWStuffEndOfFrame` | `0x0048D440` | function | high | src/core/main.cpp:382 |
| `CTimer::Update` | `0x004ACF70` | function | high | src/core/main.cpp:1557 and main.cpp:1758 |
| `cAudioManager::Service (DMAudio.Service)` | `0x0057C7A0` | function | high | src/core/main.cpp:1599 |
| `CStreaming::Update` | `0x004076C0` | function | high | src/core/Game.cpp:1021 |
| `CCutsceneMgr::Update` | `0x00404EE0` | function | medium | src/core/Game.cpp:1014 |
| `CMenuManager::Process` | `0x00485100` | function | high | src/core/Game.cpp:1018 and src/core/main.cpp:1764 |
| `CSprite2d::SetRecipNearClip` | `0x0051EA20` | function | high | src/core/main.cpp:1760, src/core/Game.cpp:1025 |
| `CSprite2d::InitPerFrame` | `0x0051EAE0` | function | high | src/core/main.cpp:1561/1761, src/core/Game.cpp:1026 |
| `CPad::DoCheats` | `0x00492F00` | function | medium | src/core/Game.cpp:1030 |
| `CClock::Update` | `0x00473460` | function | medium | src/core/Game.cpp:1031 |
| `CWeather::Update` | `0x00522C10` | function | medium | src/core/Game.cpp:1032 |
| `CTheScripts::Process` | `0x00439040` | function | medium | src/core/Game.cpp:1035 |
| `RenderMenus` | `0x0048E450` | function | high | src/core/main.cpp:1525 |
| `DoFade` | `0x0048D120` | function | medium | src/core/main.cpp:1731 |
| `Render2dStuffAfterFade` | `0x0048E470` | function | high | src/core/main.cpp:1538 |
| `g_SlowMode / ProcessSlowMode` | `0x0048DD60` | function | medium | src/core/main.cpp:1745-1746 |
| `TheCamera` | `0x006FACF8` | global | high | src/core/Game.cpp:1008 (TheCamera.SetMotionBlurAlpha) |
| `CCamera::m_BlurType` | `0x006FADA0` | field-offset | high | src/core/Game.cpp:1009 |
| `FrontEndMenuManager` | `0x008F59D8` | global | high | src/core/Game.cpp:1018, src/core/main.cpp:1527/1764 |
| `DMAudio (cAudioManager instance)` | `0x0095CDBE` | global | high | src/core/main.cpp:1599 |
| `ControlsManager (CControllerConfigManager)` | `0x008F43A4` | global | high | src/core/Pad.cpp:1100-1104 |
| `Scene.camera` | `0x0072676C` | global | high | src/core/main.cpp:386-389 (RwCameraEndUpdate(Scene.camera)) |
| `Scene.world` | `0x00726768` | global | high | src/core/main.cpp:1613 (SetLightsWithTimeOfDayColour(Scene.world)) |
| `RsGlobal.quit` | `0x008F4378` | global | high | src/core/main.cpp:1768 (`if(RsGlobal.quit) return;`) |
| `gGameState` | `0x008F5838` | global | high | src/skel/win/win.cpp (WinMain's switch(gGameState)) |
| `CTimer::m_snTimeInMilliseconds` | `0x00885B48` | global | high | src/core/main.cpp:1605 |
| `CPointLights::NumLights` | `0x0095CC3E` | global | high | src/core/main.cpp:1592 (CPointLights::InitPerFrame()) |
| `CGame::bDemoMode` | `0x005F4DD0` | global | high | src/core/main.cpp:1605 |
| `gGameState jump table (WinMain)` | `0x0060FCC0` | constant | high | src/skel/win/win.cpp (WinMain state machine) |
| `AppEventHandler jump table` | `0x005F58B8` | constant | high | src/core/main.cpp:1797 (switch(event)) |

## player / player-access (19 symbols, 1 promoted)

| Symbol | Address | Kind | Confidence | re3 reference |
|---|---|---|---|---|
| `CWorld::Players` | `0x009412F0` | global | high | src/core/World.h:61 (static CPlayerInfo Players[NUMPLAYERS]); src/core/PlayerInfo.h:97 (VA... |
| `FindPlayerPed` | `0x004A1150` | function | high | src/core/PlayerInfo.cpp:463-467 |
| `CPlayerInfo::m_pPed` | `0x0` | field-offset | high | src/core/PlayerInfo.h:22 (first member of CPlayerInfo) |
| `FindPlayerVehicle` | `0x004A10C0` | function | high | src/core/PlayerInfo.cpp:436-442 |
| `CWorld::PlayerInFocus` | `0x0095CD61` | global | high | src/core/World.h:60 / src/core/World.cpp:36 (uint8 CWorld::PlayerInFocus) |
| `FindPlayerEntity` | `0x004A10F0` | function | high | src/core/PlayerInfo.cpp:444-452 |
| `FindPlayerTrain` | `0x004A1120` | function | high | src/core/PlayerInfo.cpp:454-461 |
| `FindPlayerCoors` | `0x004A1030` | function | medium | src/core/PlayerInfo.cpp:407-418 |
| `FindPlayerSpeed` | `0x004A1090` | function | medium | src/core/PlayerInfo.h:92 / src/core/PlayerInfo.cpp:421-434 |
| `CPlayerInfo::GetPos` | `0x004A0FE0` | function | high | src/core/PlayerInfo.cpp:395-405 |
| `CPlayerInfo::ArrestPlayer` | `0x004A1330` | function | high | src/core/PlayerInfo.cpp:523-533 |
| `sizeof(CPlayerInfo)` | `0x13C` | constant | high | src/core/PlayerInfo.h:97 VALIDATE_SIZE(CPlayerInfo, 0x13C) |
| `CPlayerInfo::m_WBState` | `0xD8` | field-offset | high | src/core/PlayerInfo.h:39 (int8 m_WBState) |
| `CPlayerInfo::m_nWBTime` | `0xDC` | field-offset | high | src/core/PlayerInfo.h:40 (uint32 m_nWBTime) |
| `CPlayerInfo::m_pRemoteVehicle` | `0x4` | field-offset | high | src/core/PlayerInfo.h:23 |
| `CPed::m_pMyVehicle` | `0x310` | field-offset | high | src/peds/Ped.h (CPed::m_pMyVehicle); used at src/core/PlayerInfo.cpp:439 |
| `CPed::bInVehicle` | `0x314` | field-offset | high | src/peds/Ped.h (CPed::bInVehicle, tested by CPed::InVehicle()); used at src/core/PlayerInf... |
| `CPlaceable matrix translation (GetPosition)` | `0x34` | field-offset | high | src/math/Matrix.h:3-17 (px,py,pz at float index 12..14 = +0x30 inside CMatrix) and src/cor... |
| `CPlayerPed::m_pWanted` | `0x53C` | field-offset | medium | src/peds/PlayerPed.h (first CPlayerPed member, CWanted *m_pWanted); src/peds/Ped.h:959 VAL... |

## pedlayout / ped-vehicle-layout (33 symbols left; 22 vehicle rows promoted, 2 Record rows resolved above)

| Symbol | Address | Kind | Confidence | re3 reference |
|---|---|---|---|---|
| `sizeof(CPed)` | `0x53C` | constant | high | src/peds/Ped.h:959 (VALIDATE_SIZE(CPed,0x53C)); src/peds/PlayerPed.h:12,88; src/core/Pools... |
| `CPed::m_nPedState` | `0x224` | field-offset | high | src/peds/Ped.h:421 (decl), Ped.h:222 (PedState enum); src/peds/Ped.cpp:6320-6331 (SetDie);... |
| `CPed::m_nMoveState` | `0x22C` | field-offset | high | src/peds/Ped.h:423, Ped.h:290 (eMoveState); src/peds/PlayerPed.cpp:1403-1404 |
| `CPed::m_fHealth` | `0x2C0` | field-offset | high | src/peds/Ped.h:436; src/core/Pad.cpp:96-99 (HealthCheat sets m_fHealth = 100.0f) |
| `CPed::m_fArmour` | `0x2C4` | field-offset | high | src/peds/Ped.h:437; src/core/Pad.cpp:220-224 (ArmourCheat) |
| `CPed::m_fRotationCur` | `0x2DC` | field-offset | high | src/peds/Ped.h:444; src/peds/Ped.cpp:5202-5206; src/control/Script2.cpp:495 |
| `CPed::m_fRotationDest` | `0x2E0` | field-offset | high | src/peds/Ped.h:445; src/control/Script2.cpp:486-497 |
| `CPed::m_pMyVehicle` | `0x310` | field-offset | high | src/peds/Ped.h:454 |
| `CPed::bInVehicle` | `0x314` | field-offset | high | src/peds/Ped.h:455; src/peds/Ped.cpp:6321-6326 |
| `CPed::m_weapons` | `0x35C` | field-offset | high | src/peds/Ped.h:478; src/peds/Ped.cpp:4700-4720; src/weapons/WeaponType.h:30 |
| `sizeof(CWeapon)` | `0x18` | constant | high | src/weapons/Weapon.h:17-21,76 (VALIDATE_SIZE(CWeapon,0x18)) |
| `CPed::m_currentWeapon` | `0x498` | field-offset | high | src/peds/Ped.h:480-481; src/peds/Ped.cpp:4733-4744 |
| `CPed::m_nLastPedState` | `0x228` | field-offset | high | src/peds/Ped.h:422; src/peds/Ped.cpp:6347 |
| `CPed::m_nPrevMoveState` | `0x234` | field-offset | medium | src/peds/Ped.h:424-425 |
| `CPed::m_pVehicleAnim` | `0x1D8` | field-offset | high | src/peds/Ped.h:412; src/peds/Ped.cpp:6325-6326 |
| `CPed::m_objective` | `0x164` | field-offset | high | src/peds/Ped.h:395, Ped.h:157-173 (eObjective); src/peds/PlayerPed.cpp:1398 |
| `CPed::pedFlags (bitfield base dword)` | `0x154` | field-offset | high | src/peds/Ped.h:306-386 |
| `CPed::m_nPedType` | `0x32C` | field-offset | medium | src/peds/Ped.h:466; src/peds/PedType.h |
| `CPhysical::m_vecMoveSpeed` | `0x78` | field-offset | high | src/entities/Physical.h:24; src/control/Record.cpp:340-342,361 |
| `CPhysical::m_vecTurnSpeed` | `0x84` | field-offset | high | src/entities/Physical.h:25; src/control/Record.cpp:362 |
| `CPlaceable::m_matrix` | `0x04` | field-offset | high | src/core/Placeable.h:9,26-30,35; src/core/Pad.cpp:122 |
| `CPlaceable::m_matrix position (GetPosition)` | `0x34` | field-offset | high | src/core/Placeable.h:13-19; src/math/Matrix.h:15,39; src/control/Record.cpp:355 |
| `CMatrix right/forward/up (relative to entity base)` | `0x04 / 0x14 / 0x24` | field-offset | high | src/math/Matrix.h:7-17,37-42; src/control/Record.cpp:349-355 |
| `CEntity flags dword (m_type bits 0-2, m_status bits 3-7)` | `0x50` | field-offset | high | src/entities/Entity.h:37-42,101-103; src/core/Pad.cpp:125 |
| `CEntity::m_modelIndex` | `0x5C` | field-offset | high | src/entities/Entity.h:89; src/control/Record.cpp:373; src/control/CarAI.cpp:67 |
| `sizeof(CPlayerPed) / ped pool slot stride` | `0x5F0` | constant | high | src/core/config.h (NUMPEDS); src/peds/PlayerPed.h:88; src/core/Pools.h:15 |
| `CPools::ms_pPedPool` | `0x008F2C60` | global | high | src/core/Pools.h:26,37 |
| `CPools::ms_pVehiclePool` | `0x009430DC` | global | high | src/core/Pools.h:27,38 |
| `CPools::Initialise` | `0x004A1770` | function | high | src/core/Pools.cpp (CPools::Initialise); src/core/config.h |
| `FindPlayerPed` | `0x004A1150` | function | high | src/core/World.cpp (FindPlayerPed); src/core/Pad.cpp:97,222; src/control/CarAI.cpp:69 |
| `CPed::GiveWeapon` | `0x004CF9B0` | function | high | src/peds/Ped.cpp:4700-4720; src/core/Pad.cpp:80-94 |
| `CPed::SetDie` | `0x004D37D0` | function | high | src/peds/Ped.cpp:6303-6348 |
| `CPed::SetStoredState` | `0x004C5DB0` | function | high | src/peds/Ped.cpp (CPed::SetStoredState); src/peds/Ped.cpp:6318 |

## asi / asi-and-compat (27 symbols)

| Symbol | Address | Kind | Confidence | re3 reference |
|---|---|---|---|---|
| `DECISION: ship CoopIII as a .asi, delete the proxy/ target` | `` | constant | high | - |
| `UAL .asi contract: plain x86 DLL, renamed, no exports required` | `` | constant | high | - |
| `UAL search directories on this install: game root, then scripts\, then plugins\` | `` | constant | high | - |
| `scripts/global.ini keys that this UAL build actually honours` | `` | constant | high | - |
| `UAL loads ASIs from its own DllMain on this install (no entry-point deferral)` | `` | constant | high | - |
| `UAL hard-codes Mod Loader as the very first ASI` | `` | constant | high | - |
| `UAL makes the whole gta3.exe image RWX, but only AFTER the root *.asi scan` | `` | constant | high | - |
| `Full verified load order on this installation` | `` | constant | high | - |
| `ddraw.dll is NOT the SilentPatch that patches game code` | `` | constant | high | - |
| `SilentPatchIII locates its patch sites by byte-pattern scanning, not by address` | `` | constant | high | - |
| `HARD COLLISION: GInput already owns the first instruction of CGame::Process` | `0x0048C850` | constant | high | - |
| `RECOMMENDED inbound+outbound hook site: the only call to Idle's CGame::Process` | `0x0048E49B` | constant | high | - |
| `RECOMMENDED one-shot deferred-init hook (install detours after the mod stack settles)` | `0x0048E7F6` | constant | high | - |
| `House convention in this stack: redirect an existing call's rel32, never splice a prologue` | `` | constant | high | - |
| `CLEO III's init hook (one-shot, inside CGame::Initialise)` | `0x0048C26B` | constant | high | - |
| `CLEO III's per-frame work rides inside CTheScripts::Process` | `0x00439040` | constant | high | - |
| `Mod Loader's hook: the CRT's call to WinMain` | `0x005C1F39` | constant | high | - |
| `This exe is detected as GTA III 1.0, not 1.1; a 1.1 target assumption is wrong for this install` | `0x005C1E70` | constant | high | - |
| `CGame::Process` | `0x0048C850` | function | high | src/core/Game.cpp:1002 |
| `Idle` | `0x0048E480` | function | high | src/core/main.cpp:1551 |
| `CPad::UpdatePads` | `0x00492720` | function | high | src/core/Game.cpp:1004 |
| `CTheScripts::Process` | `0x00439040` | function | high | src/core/Game.cpp:1918 |
| `CGame::Initialise(const char* datFile)` | `0x0048BED0` | function | high | src/core/Game.cpp:392 |
| `WinMain` | `0x00582710` | function | medium | - |
| `FrontendIdle` | `0x0048E700` | function | medium | src/core/main.cpp |
| `Where to physically put CoopIII.asi, and the ordering it buys you` | `` | constant | medium | - |
| `What NOT to pattern-scan for, concretely` | `` | constant | medium | - |


## fire (found 2026-09-21, alongside §5.7 phase two)

The fire work proved `gFireManager`, the table geometry, the `CFire` layout,
`Update`, `GetNextFreeFire`, both `StartFire` overloads, `StartScriptFire`,
`ProcessFire`, `Extinguish`, `ReportThisFire`, `CPed::m_pFire` and
`CVehicle::m_pCarFire`; all of those are in `addresses.h` now with the
disassembly that carries each one.

Phase three (2026-09-22) promoted two more out of the list below.
`CPed::IsPedInControl` (`0x004CE6C0`) is in `addresses.h`: its body is three
tests and two of the three fields are ones this file already had -
`m_nPedState` at `0x224` and `m_fHealth` at `0x2C0` - which is what identifies
it rather than its position at a call site. `CPed::IsPlayer` (`0x004D48E0`)
went the same way, reading `m_nPedType` at `0x32C` and comparing it against
0..3: it is the function that decides a burning ped's extinguish time, and
`CShotInfo::Update` uses it to skip the flee block.

These came out of the same pass and are **not** proved. Every one of them was
identified from a single call site or from re3's statement order alone, which
is the way the three refuted addresses in this document were arrived at.

| Claim | Address | Kind | Confidence | How far it got |
|---|---|---|---|---|
| `CFireManager::FindNearestFire(CVector, float*)` | `0x00479340` | function | medium | Only the argument shape: called from `0x004C3D29` with `push edx / push [eax+8] / push [eax+4] / push [eax]`, which is a `CVector` by value plus an out pointer, and re3 declares exactly one such member. The body was never read. |
| `CFireManager::FindFurthestFire_NeverMindFireMen` | `0x00479430` | function | high | The body *was* read and it is the right shape - 40 slots, stride `0x30`, skips script fires, 2D distance, keeps the furthest, returns `&m_aFires[i]`. Its loop bound is one of the three witnesses `addresses.h` cites for `NUM_FIRES`, and that part stands on the bound rather than on the name. The name itself is re3's and unconfirmed. |
| `CPed::RestorePreviousState` | `0x004C5E30` | function | high | The call `CFire::Extinguish` makes on a burning ped immediately before nilling `m_pFire`, which is re3 `Fire.cpp` `Extinguish`'s only ped statement. The body has since been read and every field in it is one this project already had: `CanSetPedState` on `[+224h]`, then `[+314h]`/`[+310h]` -> `m_nPedState = 2Ch` (PED_DRIVING) and `m_nLastPedState = 0`, then a jump table on `[+228h]` (`PED_LAST_STATE`), with the zero arm testing `IsPlayer`, `CharCreatedBy` at `[+160h]` and `m_objective` at `[+164h]`. Not promoted because CoopIII never calls it directly - it reaches it through `CFire::Extinguish`, which is verified. |
| `CPed::SetMoveState(eMoveState)` | `0x004C5A30` | function | medium | Called `__thiscall` with a single pushed argument from two places that are doing the same thing to a ped: `StartFire`'s AI arm pushes 4 (`PEDMOVE_SPRINT`) at `0x004796A9` and `CPed::SetFlee` pushes 3 (`PEDMOVE_RUN`) at `0x004D1DC5`, both immediately after touching `[ped+157h]`. The body was never read, and it is not simply `m_nMoveState = arg` - `PED_MOVE_STATE` is a plain dword at `0x22C` that a caller could write inline, so whatever else it does is unknown. |
| `CPed::SetStoredState` | `0x004C5DB0` | function | medium | The call `CPed::SetFlee` makes immediately before `m_nPedState = 9`, which is where re3 saves `m_nLastPedState`. `CPed::RestorePreviousState` reading `[ped+228h]` (= `PED_LAST_STATE`) is the other half of the same story. Body not read. |
| `CPed::SetIdle` | `0x004D0600` | function | low | Where `RestorePreviousState` goes for a ped whose `m_nLastPedState` is 0 and whose `CharCreatedBy` is `MISSION_CHAR` - i.e. every remote player whose fire goes out. Identified by position in that function and nothing else. Matters because it is one of the few engine paths that writes a remote ped's `m_nPedState` behind CoopIII's back. |
| `CEventList::RegisterEvent` | `0x00475C50`, `0x00475E10` | function | low | Two arities, both `cdecl`-ish with `add esp,14h` after five pushes. `0x00475E10` is what `CFire::ReportThisFire` calls with `(7, x, y, z, 1000)`; `0x00475C50` is what `StartFire` calls with `(0Eh or 0Fh, 1, ped, fleeFrom, 10000)`. CoopIII reaches the first one through `ReportThisFire` and never calls either directly. |
| `CWorld::SetCarsOnFire` / its ped equivalent | `0x004B3D20`-ish, `0x004B3F00`-ish | function | low | Two of the six callers of `StartFire(entity, ...)` sit in these, at `0x004B3E3C` and `0x004B3F9C`. The function *starts* were never located, so the addresses above are the containing region and not entry points. Do not call either. |
| `CShotInfo::Update` | contains `0x0055C232` | function | low | The flamethrower's own caller of `StartFire(entity, ...)`. Same problem: a call site inside it, not its start. |
| `CPed::DoStuffToGoOnFire` | - | function | none | Named by re3 as what `ProcessFire` calls before spreading fire to the player. Never looked for. |
