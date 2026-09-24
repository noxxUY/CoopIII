// Addresses inside gta3.exe.
//
// ############################################################
// #  EVERY ADDRESS HERE IS SPECIFIC TO ONE EXACT BUILD.      #
// #  GTA III PC v1.0 retail, MD5 85414BF9EB414D00AD81062360F0DB1F   #
// #  2 383 872 bytes, 2002-05-21. Nothing here is portable.  #
// ############################################################
//
// Why raw addresses instead of the pattern scanner? docs/compat.md points out
// that SilentPatch rewrites code at runtime. That changes *bytes*, not
// *addresses* - a function it patched is still at the same entry point, but a
// byte pattern matching what it rewrote breaks. Verify() below gates on the
// exact image anyway, so the usual argument for patterns (surviving a
// different build) doesn't even apply here; we don't support another build.
// pattern.h in client/src/hook stays around for whenever CoopIII needs to
// support a second exe, or for anchoring something that does move around.
//
// Provenance: everything here came from an IDA pass, then got checked by two
// people working independently from reference/re3. Things that were only
// found, not verified, don't live in this file - see
// docs/addresses-unverified.md. Don't promote something out of that doc
// without doing the verification; a guess and a proof shouldn't look the
// same.
#pragma once

#include <coopiii/protocol.h>

#include <cstddef>
#include <cstdint>

namespace coopiii::game {

// ---- image identity -------------------------------------------------------

constexpr uint32_t IMAGE_BASE = 0x00400000;
constexpr uint32_t IMAGE_SIZE = 2383872;
constexpr char     IMAGE_MD5[] = "85414BF9EB414D00AD81062360F0DB1F";

// The PE's own internal name. Cheap sanity check that does not need hashing.
constexpr char IMAGE_INTERNAL_NAME[] = "gta3_final.exe";

// ---- version --------------------------------------------------------------

// main.cpp:164. Decodes models\coll\peds.col and fills version_name.
constexpr uintptr_t ValidateVersion = 0x0048BAD0;

// char[64], main.cpp:115. Filled at runtime from peds.col, so it's the *data*
// version, not the exe version - reads "Version 1.0" on a stock 1.0 install.
// Fine for logs. Not a substitute for the MD5 check since a player can swap
// peds.col out.
constexpr uintptr_t version_name = 0x0070D990;

// ---- frame loop -----------------------------------------------------------

// Game.cpp:1002. Runs once per frame, before the render phase, on the game
// thread. This is where CoopIII pumps - the world is safe to touch here and
// the frame's simulation hasn't started yet. See docs/protocol.md §1.1 and §5.
constexpr uintptr_t CGame__Process = 0x0048C850;

// Timer.cpp:83. Runs immediately before CGame::Process.
constexpr uintptr_t CTimer__Update     = 0x004ACF70;
constexpr uintptr_t CTimer__Initialise = 0x004ACE60;
constexpr uintptr_t CTimer__Shutdown   = 0x004ACF60;
constexpr uintptr_t CTimer__Suspend    = 0x004AD310;
constexpr uintptr_t CTimer__Resume     = 0x004AD370;
constexpr uintptr_t CTimer__Stop       = 0x004AD480;

// Engine clocks. Read these to match engine behaviour. Never use them to
// timestamp a packet - docs/protocol.md §1.2 goes into why that desyncs.
constexpr uintptr_t CTimer__m_snTimeInMilliseconds           = 0x00885B48;
constexpr uintptr_t CTimer__m_snPreviousTimeInMilliseconds   = 0x008F29E4;
constexpr uintptr_t CTimer__m_snTimeInMillisecondsNonClipped = 0x009412E8;
constexpr uintptr_t CTimer__m_snTimeInMillisecondsPauseMode  = 0x005F7614;
constexpr uintptr_t CTimer__ms_fTimeStep                     = 0x008E2CB4;
constexpr uintptr_t CTimer__ms_fTimeStepNonClipped           = 0x008E2C4C;
constexpr uintptr_t CTimer__ms_fTimeScale                    = 0x008F2C20;
constexpr uintptr_t CTimer__m_FrameCounter                   = 0x009412EC;

// The two pause flags - both bytes. Verified 2026-09-21 because CoopIII
// writes m_UserPause every frame (game/pause.h), and a merely-plausible
// address here means scribbling on a neighbouring global forever.
//
// m_UserPause's proof is its own two setters, one instruction each, sitting
// right next to each other:
//
//   0x004AD490  mov byte [0x0095CD7C], 1 / ret     CTimer::StartUserPause
//   0x004AD4A0  mov byte [0x0095CD7C], 0 / ret     CTimer::EndUserPause
//
// matches re3 Timer.cpp:230 and :237 exactly. CTimer::Update reads that same
// byte three times (0x004AD045, 0x004AD180, 0x004AD256), each as
// `cmp byte [0x0095CD7C], 0` - re3's three GetIsPaused() call sites: both arms
// of the QueryPerformanceCounter branch, plus the
// `ms_fTimeStep < 0.01f && !GetIsPaused()` clamp. CTimer::Initialise clears it
// at 0x004ACE9B, matching re3's `m_UserPause = false`.
constexpr uintptr_t CTimer__StartUserPause                   = 0x004AD490;
constexpr uintptr_t CTimer__EndUserPause                     = 0x004AD4A0;
constexpr uintptr_t CTimer__m_UserPause                      = 0x0095CD7C;
constexpr uintptr_t CTimer__m_CodePause                      = 0x0095CDB1;

// ---- time of day and weather ----------------------------------------------
//
// Verified 2026-09-21. Found the way the ped and vehicle paths were: by
// walking the script opcodes that already do the thing, instead of hunting a
// symbol by name. The dispatcher at 0x00439500 is a chain of
// `cmp dx,<limit> / jge / movsx eax,dx / push eax / call <range handler>`,
// and each range handler is `lea eax,[opcode - base] / cmp eax,<count> / ja
// default / jmp [eax*4 + <table>]`.
//
//   SET_TIME_OF_DAY    192  ->  100..199 table 0x005EEA7C, entry 92
//                               handler 0x0043D1EB
//   FORCE_WEATHER      437  ->  400..499 table 0x005EEFC8, entry 37
//   FORCE_WEATHER_NOW  438                              entry 38
//   RELEASE_WEATHER    439                              entry 39
//
// The 400..499 range handler is 0x00440CB0 and its base opcode is a round
// 400 (`lea edx,[eax + 0FFFFFE70h]`, i.e. eax - 0x190). The 100 table is the
// one the ped spawn already uses, and entry 54 there is still CREATE_CHAR's
// 0x0043BA03 - which is how this pass knew it had the right base before
// trusting anything else it read out of it.
//
// Every rel32 below was resolved with tools/calltarget, not by hand.

// Clock.cpp:74. __cdecl(uint8 h, uint8 m) - both arguments still take a
// 4-byte stack slot; the callee reads the low byte of each.
//
//   0x004733C0  mov  eax, [0x00885B48]        CTimer::GetTimeInMilliseconds()
//               mov  word [0x0095CC7C], 0     ms_nGameClockSeconds = 0
//               mov  [0x009430E4], eax        ms_nLastClockTick
//               mov  al, [esp+4]
//               mov  [0x0095CDA6], al         ms_nGameClockHours
//               mov  al, [esp+8]
//               mov  [0x0095CDC8], al         ms_nGameClockMinutes
//               ret
//
// 0x00885B48 is CTimer::m_snTimeInMilliseconds, recorded above from an
// unrelated pass - the two agree, which is the kind of mutual confirmation
// worth looking for.
//
// Prefer this over writing the two bytes directly. It also rebases
// ms_nLastClockTick, so the engine's own CClock::Update measures the next
// game minute from the correction rather than immediately ticking one off
// whatever was left over.
constexpr uintptr_t CClock__SetGameClock = 0x004733C0;

// Three more witnesses for the same globals, each from a different function:
//
//   CClock::Initialise  0x00473370  `mov byte [0x0095CDA6], 0Ch` and
//                                   `mov byte [0x0095CDC8], 0` - Clock.cpp:25's
//                                   12:00, as a literal. CGame::Initialise
//                                   calls it with `push 3E8h` at 0x0048C289
//                                   (Game.cpp:596), so a game minute really
//                                   is 1000 ms.
//   CClock::Update      0x00473460  `inc byte [0x0095CDC8]`, then
//                                   `cmp byte [0x0095CDC8], 3Ch` / `inc byte
//                                   [0x0095CDA6]` / `cmp byte [0x0095CDA6],
//                                   18h` - 60 and 24, i.e. Clock.cpp:53-66.
//   CClock::StoreClock  0x00473540  reads hours, minutes and seconds in
//                                   declaration order into three stored
//                                   copies, and RestoreClock (0x00473570) is
//                                   its mirror.
//
// GetGameClockMinutesUntil (0x004733F0, Clock.cpp:83) settles which of the
// two bytes is which on its own: it does `imul edx, [0x0095CDA6], 3Ch` and
// adds [0x0095CDC8], so the one multiplied by 60 is the hour.
constexpr uintptr_t CClock__Initialise             = 0x00473370;
constexpr uintptr_t CClock__Update                 = 0x00473460;
constexpr uintptr_t CClock__ms_nGameClockHours     = 0x0095CDA6;   // uint8
constexpr uintptr_t CClock__ms_nGameClockMinutes   = 0x0095CDC8;   // uint8
constexpr uintptr_t CClock__ms_nGameClockSeconds   = 0x0095CC7C;   // uint16
constexpr uintptr_t CClock__ms_nLastClockTick      = 0x009430E4;   // uint32
constexpr uintptr_t CClock__ms_nMillisecondsPerGameMinute = 0x008F2C64;   // uint32

// CWeather's three type globals and the blend position between them. All
// four are pinned by CWeather::Init (0x00522BA0, Weather.cpp:105-118), which
// happens to give each one a different literal:
//
//   mov word [0x0095CC70], 0        NewWeatherType    = WEATHER_SUNNY
//   mov word [0x0095CCEC], 1        OldWeatherType    = WEATHER_CLOUDY
//   mov dword [0x008F2520], 0       InterpolationValue = 0.0f
//   or  word [0x0095CC80], 0FFFFh   ForcedWeatherType = WEATHER_RANDOM (-1)
//
// CWeather::Update confirms old and new a second time and in the other
// direction: at 0x00522C4C it does `mov ax,[0x0095CC70] / mov
// [0x0095CCEC],ax`, which is Weather.cpp:125's `OldWeatherType =
// NewWeatherType` and can only be read that way round.
//
// The type numbering is confirmed against the retail data rather than re3's
// header: WeatherTypesList at 0x005FFBC8 reads 0,0,...,1,1,2,2,1,0 at
// entries 16..21, matching Weather.cpp's table exactly.
constexpr uintptr_t CWeather__NewWeatherType     = 0x0095CC70;   // int16
constexpr uintptr_t CWeather__OldWeatherType     = 0x0095CCEC;   // int16
constexpr uintptr_t CWeather__ForcedWeatherType  = 0x0095CC80;   // int16
constexpr uintptr_t CWeather__InterpolationValue = 0x008F2520;   // float

enum eWeatherType {
	WEATHER_SUNNY  = 0,
	WEATHER_CLOUDY = 1,
	WEATHER_RAINY  = 2,
	WEATHER_FOGGY  = 3,
	WEATHER_TOTAL  = 4,
	WEATHER_RANDOM = -1,   // ForcedWeatherType only: "let the list decide"
};

// Weather.cpp:270-284. All three are two or three plain stores and nothing
// else, which is why game/world.cpp writes the globals rather than calling
// them - there is no registration hiding in here the way there is in the ped
// and vehicle spawn paths. Recorded because they are the proof:
//
//   ForceWeather     0x00523170  mov eax,[esp+4] / mov [Forced],ax / ret
//   ForceWeatherNow  0x00523180  mov ecx,[esp+4] / mov [Old],cx /
//                                mov ax,[Old] / mov [New],ax /
//                                mov [Forced],cx / ret
//   ReleaseWeather   0x005231A0  or word [Forced],0FFFFh / ret
constexpr uintptr_t CWeather__ForceWeather    = 0x00523170;
constexpr uintptr_t CWeather__ForceWeatherNow = 0x00523180;
constexpr uintptr_t CWeather__ReleaseWeather  = 0x005231A0;
constexpr uintptr_t CWeather__Update          = 0x00522C10;

// The one ordering fact the whole feature rests on. CGame::Process calls
// these two back to back (Game.cpp:1031-1032):
//
//   0x0048C8EB  call 0x00473460    CClock::Update
//   0x0048C8F0  call 0x00522C10    CWeather::Update
//
// and CWeather::Update opens with `mov al, [0x0095CDC8]` (the clock's
// minutes) and ends by storing that minute over 60 into InterpolationValue.
// So the blend between the old and new weather types is a function of the
// clock and is recomputed every frame. Sync the clock and the blend follows;
// there is no reason to put it on the wire.
//
// CoopIII's PreFrame runs before CGame::Process, so anything written to
// either from there is what these two read this frame, not next frame.

// ---- trains ---------------------------------------------------------------
//
// Verified 2026-09-23 against the retail image. re3's Train.cpp was the map
// and is right about the shape; everything below was read off the bytes.
//
// CTrain::UpdateTrains is called from exactly one place, CGame::Process, in
// the block the pause flag gates (Game.cpp:1039):
//
//   0x0048C8FA  call 0x0040B3B0    CCollision::Update
//   0x0048C8FF  call 0x0054F3A0    CTrain::UpdateTrains
//   0x0048C904  call 0x0054BEC0    CPlane::UpdatePlanes
//
// resolved with tools/calltarget, and a scan of the whole image for E8/E9
// rel32s finds no other call or jump to it. It is static, takes nothing and
// returns nothing: `push ebx / push esi / mov esi,6FAD2Ch / ... / ret`, with
// 0x006FAD2C being TheCamera + 0x34, the camera's position.
//
// **Where a train is, is a function of CTimer::m_snTimeInMilliseconds and of
// nothing that accumulates.** The El half:
//
//   0x0054F426  mov  edx,[00885B48h]         t = CTimer::m_snTimeInMilliseconds
//   0x0054F430  lea  eax,[edx+ebx]           ebx = 0, then += 10000h per train
//   0x0054F43B  and  eax,1FFFFh
//   0x0054F448  fild qword [esp]             the masked time
//   0x0054F44B  fld  st(4) / fmul [006023B0h]  TotalDurationOfTrack * 1/131072
//   0x0054F466  fcomp [esi+0070D850h]        walk aLineBits for the segment
//   0x0054F490  jmp  [eax*4+006023F8h]       stand / constant / accelerate
//   0x0054F4A0  fstp [eax*4+0064D008h]       EngineTrackPosition[i]
//   0x0054F4E3  fstp [eax*4+00880848h]       EngineTrackSpeed[i]
//   0x0054F546  cmp  cx,2                    two trains
//
// and the subway half is the same code over its own tables: `and eax,3FFFFh`
// at 0x0054F59B, 1/262144 at 0x006023B4, EngineTrackPosition_S at
// 0x0064D018, `cmp dx,4` at 0x0054F6A7. The segment search starts from j = 0
// every call (`xor esi,esi` at 0x0054F453 and 0x0054F5B3). Nothing is read
// that the previous frame wrote. The float constants were read from the
// file: 200/1600/-1000/500 for the camera box the El half is gated on,
// 1000.0, 7.6293945e-06 and 3.8146973e-06.
//
// Two oddities, both harmless to a caller. Each half ends by storing the time
// it read straight back into the global (`mov [00885B48h],edx` at
// 0x0054F55E, `mov [00885B48h],ebx` at 0x0054F6BF), so the call writes
// CTimer's clock with whatever value it found there. And
// ProcessTrainAnnouncements (0x0054F6D0), called at 0x0054F564, flips
// bTrainArrivalAnnounced (0x006022A0) and calls PlayAnnouncement at
// 0x0054F7F0, which in this build is a single `ret`.
//
// A byte scan of .text for the four arrays finds them referenced in these
// three functions and nowhere else: UpdateTrains writes them, the
// announcements read the El positions, and CTrain::ProcessControl reads one
// of each through `mov edx,64D008h` / `mov [esp+1Ch],880848h` (El) or
// 0x0064D018 / 0x0087C7C8 (subway), picked by m_nTrackId at +0x29C.
//
// ProcessControl (vtable slot 8, see below) turns that into a wagon: rear =
// EngineTrackPosition[m_nWagonGroup +0x292] - m_fWagonPosition (+0x288),
// wrapped by TotalLengthOfTrack; the node pair around it; the front 20 units
// further on; the matrix from the two. Its only persistent input is
// m_nCurTrackNode (+0x290), and that is a search cursor, not state: the loop
// at 0x0054F901 advances it `(n + 1) % numTrackNodes` (`idiv` at 0x0054F90A)
// until the segment contains the position, so it lands on the same segment
// whatever it started from. What else it keeps - m_isFarAway (+0x28E,
// `(m_nWagonId + m_FrameCounter) & 0Fh` at 0x0054F841, so a wagon 250 m from
// the camera (62500.0 at 0x006023CC) is repositioned one frame in sixteen) and
// the door state machine at +0x2A0/+0x2A4, the one place it reads CTimer
// (0x0054FD9A) - follows the position and never feeds back into it.
//
// So two machines that feed UpdateTrains the same number have the same
// trains. game/trains.cpp detours it and puts the session's clock in CTimer's
// place for the length of the call.
constexpr uintptr_t CTrain__UpdateTrains = 0x0054F3A0;

// CTrain's constructor (0x0054E2A0, called from InitTrains at 0x0054F1D8 as
// `push 4 / push 7Ch` - PERMANENT_VEHICLE, MI_TRAIN) stamps
// `mov dword [eax],60241Ch` at 0x0054E2BA, and slot 8 of that table
// (0x0060243C) holds 0x0054F800. That slot is the only reference to it in
// the image.
constexpr uintptr_t CTrain__vtable         = 0x0060241C;
constexpr uintptr_t CTrain__ProcessControl = 0x0054F800;

// ---- planes ---------------------------------------------------------------
//
// Verified 2026-09-23 against the retail image, the same way as the trains
// and for the same reason. re3's Plane.cpp was the map; everything below was
// read off the bytes.
//
// CPlane::UpdatePlanes is the call right after UpdateTrains in CGame::Process
// (0x0048C904, see "trains" above). A byte scan of the whole file for E8/E9
// rel32s and for the address as an absolute finds that call and nothing else,
// with UpdateTrains' call at 0x0048C8FF turning up in the same scan as the
// control. Static, no arguments, `push ebx / push esi / push ebp / sub esp,8`,
// and it returns at once while `cmp byte [0095CD5Bh],1` (CReplay::IsPlayingBack)
// holds. The linear listing runs the prologue into the padding before it;
// 0x0054BEC0 is `53 56`.
//
// **Where a plane is, is a function of CTimer::m_snTimeInMilliseconds.** Three
// airliners, one loop over flight.dat:
//
//   0x0054BEFC  mov  edx,[00885B48h]          t = CTimer, read once for the call
//   0x0054BF02  lea  eax,[edx+ecx]            ecx = 0, then += 2AAAAh per plane
//   0x0054BF0D  and  eax,7FFFFh
//   0x0054BF1F  fmul [0060200Ch]              1/524288, TotalDurationOfFlightPath
//                                             (0x0064CFB8) times the masked time
//   0x0054BF36  fcomp [ebp+00734180h]         walk aPlaneLineBits (0x00734168,
//                                             stride 14h) from j = 0 every call
//   0x0054BF6B  jmp  [eax*4+006021C8h]        stand / constant / accelerate
//   0x0054BF4A  fstp [ebx*4+008F5FBCh]        OldPlanePathPosition[i] = ...
//   0x0054BF78  fstp [ebx*4+008F5FC8h]        PlanePathPosition[i]
//   0x0054BFB8  fstp [ebx*4+00941538h]        PlanePathSpeed[i]
//   0x0054C018  cmp  ebx,3
//
// and three Dodos on flight2.dat with no lookup at all: position =
// 50.0 (0x00601F74) * ((t + k * 2AAAAh) & 7FFFFh) * TotalDurationOfFlightPath2
// (0x0064CFC0) / 524288, into PlanePath2Position (0x0064CFC4, 3 floats), the
// speed into 0x008F1A54. Both are periodic in 0x80000 ms, and 2^32 is a
// multiple of that, so the clock's wrap is invisible.
//
// The same write-back the trains have: `mov [00885B48h],edx` at 0x0054C031
// stores the value it read at 0x0054BEFC. edx is not touched in between.
// Those are the only two references to the global in the function, and it
// makes no calls at all, so a value swapped in around it is seen by the
// planes and by nothing else.
//
// **Three things carry over from the previous call, and none of them moves a
// plane.**
//
// 1. OldPlanePathPosition, copied from PlanePathPosition at the top of each
//    airliner's iteration. Its only reader is CPlane::ProcessControl at
//    0x0054CF24 (a byte scan finds exactly those two references), which
//    compares it with LandingPoint (0x008F2C7C) to play the touchdown sound
//    once. A jump of the clock can play that sound once or skip it once.
//
// 2. The two mission Cessnas, and this is the one a caller has to handle.
//    `if (CesnaMissionStatus == 1)` (0x0064CFE8, at 0x0054C0A7) positions the
//    drug-run Cessna from `t - CesnaMissionStartTime` (0x0064CFEC, `sub
//    ebp,[0064CFECh]` at 0x0054C0FD) and lands it once that reaches 128072
//    (`cmp ebp,1F448h`); the drop-off Cessna does the same from 0x0064CFF0 /
//    0x0064CFF4 against 521288 (`cmp edx,7F448h`). Both start times are
//    stamped from CTimer and nothing else: `mov eax,[00885B48h]` then
//    `mov [0064CFECh],eax` at 0x0054E0DC in CreateIncomingCesna, and the same
//    into 0x0064CFF4 at 0x0054E23C in CreateDropOffCesna. A byte scan finds
//    no other writer of either. So a caller that hands UpdatePlanes a
//    different clock must move both start times by the same amount for the
//    call, or a Cessna in flight lands, or waits, on the wrong schedule.
//
// 3. CPlane::ProcessControl's own state: m_nCurPathNode (+0x28C, word), a
//    search cursor exactly like the train's - `inc / cdq / idiv` over
//    NumPathNodes at 0x0054CE6C, looping until the segment contains the
//    position (0x0054CEB7-0x0054CED2), so it lands on the same segment from
//    any start; and m_isFarAway (+0x28A), which puts a plane more than 300 m
//    from the camera on one frame in eight (`(m_randomSeed + FrameCounter) &
//    7` at 0x0054CCCC). Neither feeds the position. The matrix fields are
//    written in that block and never read (every `[ebp+0Ch..3Ch]` there is an
//    fstp), and bIsInSafePosition (+0x51 bit 6) is set on every path out at
//    0x0054DD34, so CWorld::Process never integrates m_vecMoveSpeed into it.
//
// CPlane's constructor is 0x0054B170 (`mov dword [eax],6021DCh`); slot 8 of
// that vtable is ProcessControl, 0x0054C1D0. InitPlanes (0x0054B820) makes
// three MI_AIRTRAIN (8Ch) and three MI_DEADDODO (8Dh), PERMANENT_VEHICLE,
// locked, m_nPlaneId at +0x288.
//
// game/planes.cpp detours UpdatePlanes the way game/trains.cpp detours
// UpdateTrains, and shifts the two Cessna start times with the clock.
constexpr uintptr_t CPlane__UpdatePlanes                 = 0x0054BEC0;
constexpr uintptr_t CPlane__CesnaMissionStartTime        = 0x0064CFEC;   // int32
constexpr uintptr_t CPlane__DropOffCesnaMissionStartTime = 0x0064CFF4;   // int32

// ---- traffic lights -------------------------------------------------------
//
// Verified 2026-09-23 against the retail image. re3's TrafficLights.cpp was
// the map; everything below was read off the bytes.
//
// **A light is a function of CTimer::m_snTimeInMilliseconds and of nothing
// else.** The three functions are whole leaves - no call, no write, no other
// read - and this is all of each (lighttime.h has the thresholds):
//
//   0x00455760  CTrafficLights::LightForCars1
//     mov eax,[00885B48h] / and eax,3FFFh
//     cmp eax,1388h / jae / xor al,al / ret          green  below 5000
//     cmp eax,1770h / jae / mov al,1 / ret           amber  below 6000
//     mov al,2 / ret                                 red    the rest
//   0x00455790  CTrafficLights::LightForCars2
//     same head; cmp 1770h -> al 2, cmp 2AF8h -> al 0, cmp 2EE0h -> al 1, else al 2
//   0x004557D0  CTrafficLights::LightForPeds
//     same head; cmp 2EE0h -> al 2 (don't walk), cmp 3C18h -> al 0 (walk),
//     else al 1 (walk, blinking)
//
// The result is in al and the rest of eax is the masked clock; every caller
// reads al alone. The linear listing runs each prologue into the zero padding
// before it (`add byte ptr [ecx+00885B48h],ah` at 0x0045578F and 0x004557CF);
// the functions start on the `A1`.
//
// A byte scan of the whole file for E8/E9 rel32s and for the three addresses
// as absolutes finds ten calls and nothing else - no pointer to any of them:
//
//   CTrafficLights::ShouldCarStopForLight (0x00455350), once per path link it
//   checks, Cars1 then Cars2 by the link's trafficLightType & 7Fh:
//     0x004553BC / 0x004553E2   the next link
//     0x004554F1 / 0x00455517   the current link
//     0x00455633 / 0x00455659   the previous link
//   each followed by `test al,al`, and every pair behind `test bl,bl / jne`
//   on the alwaysStop argument, so a caller passing true never reads a light.
//   Its three callers: 0x004186D4 pushes 1 (CCarCtrl::PossiblyRemoveVehicle,
//   so the removal test doesn't depend on the light); 0x004191EB pushes 0
//   (SlowCarOnRailsDownForTrafficAndLights at 0x004191E0, which then calls
//   CCarAI::CarHasReasonToStop and makes 0 the car's target speed); 0x0041EA52
//   pushes 0 from the driving-style switch at 0x0041EA48 (styles 0 and 1,
//   stop and slow down for cars).
//
//   CTrafficLights::DisplayActualLight (0x00455800), the only caller being
//   CEntity::ProcessLightsForEntity at 0x00474A89 for MI_TRAFFICLIGHTS:
//     0x00455851 / 0x00455858   Cars1 or Cars2 by FindTrafficLightType
//                               (0x004564A0) == 1, then `mov bl,al`
//     0x00456105                LightForPeds, `cmp al,2`
//   and the one thing about a light that is not a call: the blink of the walk
//   sign is `mov eax,[00885B48h] / and eax,100h` at 0x004562A4, CTimer read
//   directly. That stays on this machine's clock; it is 256 ms of flicker.
//
//   CPed::Wait, WAITSTATE_TRAFFIC_LIGHTS (entry 0 of the jump table at
//   0x005F8AA4, i.e. state 1):
//     0x004D5DE6  once CTimer passes m_nWaitTimer (+0x23C), LightForPeds; on
//                 al == 0 it clears m_nWaitState (+0x238) and calls
//                 SetMoveState(PEDMOVE_WALK) (0x004C5A30). That is the one
//                 decision a light makes that outlasts the frame: a ped that
//                 has set off crosses, whatever the light does next.
//
// Nothing keeps a light between frames. CarHasReasonToStop (0x00415B00) is
// `mov eax,[00885B48h] / mov [ecx+14Ch],eax` - it stamps
// AutoPilot.m_nAntiReverseTimer with this machine's clock, which only this
// machine's AI reads - and the on-rails target speed ramps towards the
// light's answer over a few frames, which is a car accelerating, not a stale
// light.
//
// game/lights.cpp detours all three and answers from lighttime.h on the
// session's clock, or calls the original when there is no session.
constexpr uintptr_t CTrafficLights__LightForCars1 = 0x00455760;
constexpr uintptr_t CTrafficLights__LightForCars2 = 0x00455790;
constexpr uintptr_t CTrafficLights__LightForPeds  = 0x004557D0;

// ---- lift bridge ----------------------------------------------------------
//
// Verified 2026-09-23 against the retail image. re3's Bridge.cpp was the map.
//
// CBridge::Update (0x00413AC0) has one caller, CGame::Process at 0x0048C9CD,
// outside the replay gate. It returns at once unless pLiftPart (0x008E2C94)
// and pWeight (0x008E28BC) were both found by CBridge::Init. Then:
//
//   0x00413AE7  OldState (0x008F2A20) = State (0x008F2A1C)
//   0x00413AE0  CStats::CommercialPassed (0x008F4334) == 0 ->
//               State = 0, lift 25.0, TimeOfBridgeBecomingOperational = 0
//   0x00413B16  if TimeOfBridgeBecomingOperational (0x008F2BC0) == 0, stamp it
//               with `mov eax,[00885B48h]`
//   0x00413B29  mov edx,[00885B48h] / sub edx,[008F2BC0h] / and edx,0FFFFh
//   then cmp 2710h / 9C40h / C350h / EA60h -> State 2 / 3 / 4 / 5, else 1,
//   and the lift height from 25.0 (0x005EC68C) and 1/10000 (0x005EC690).
//
// The epoch is referenced by those four instructions and by nothing else in
// the image (byte scan for 0x008F2BC0): not saved, not loaded, not reset by
// Init. CommercialPassed is written by COMMAND_COMMERCIAL_PASSED's handler
// (`mov dword [008F4334h],1` at 0x00449842, then the Shoreside radio
// announcement), and loaded with the stats in a save.
//
// **The part that isn't cosmetic.** Update ends with two edges and nothing
// else touches the path links:
//
//   State 4 after 3 -> SetLinksBridgeLights(-330, -230, -700, -588, true)
//   State 3 after 2 -> SetLinksBridgeLights(-330, -230, -700, -588, false)
//
// on ThePaths (`mov ecx,8F6754h`), the four floats pushed from 0x005EC688,
// 0x005EC684, 0x005EC680 and 0x005EC67C (-330, -230, -700, -588, read from
// the file). CBridge::Init (0x00413A30) makes the same call with true and
// writes OldLift (0x008F6254) = -1.0f (`mov [008F6254h],0BF800000h`), which
// the next Update always overwrites with a height between 0 and 25.
// CPathFind::SetLinksBridgeLights (0x0042E3B0) is __thiscall, five dwords,
// `ret 14h`; it walks m_numCarPathLinks (+0x45BE8) links of 18h bytes from
// +0x26840 and sets bit 0 of +15h in each whose position is inside the box.
// That bit is what CTrafficLights::ShouldCarStopForBridge (0x00456460) reads
// at 0x0091CFA9 = 0x008F6754 + 0x26840 + 15h: cars stop when the next link
// has it and the current one doesn't. So a bridge that is up on one machine
// and down on another sends traffic different ways, and a clock that jumps
// over an edge leaves the links wrong until the cycle comes round again.
//
// game/liftbridge.cpp detours Update; liftbridgetime.h has the arithmetic.
constexpr uintptr_t CBridge__Update                          = 0x00413AC0;
constexpr uintptr_t CBridge__pLiftPart                       = 0x008E2C94;   // CEntity*
constexpr uintptr_t CBridge__pWeight                         = 0x008E28BC;   // CEntity*
constexpr uintptr_t CBridge__State                           = 0x008F2A1C;   // int32
constexpr uintptr_t CBridge__OldState                        = 0x008F2A20;   // int32
constexpr uintptr_t CBridge__OldLift                         = 0x008F6254;   // float
constexpr uintptr_t CBridge__TimeOfBridgeBecomingOperational = 0x008F2BC0;   // uint32, CTimer ms
constexpr uintptr_t CStats__CommercialPassed                 = 0x008F4334;   // int32
constexpr uintptr_t ThePaths                                 = 0x008F6754;   // CPathFind
constexpr uintptr_t CPathFind__SetLinksBridgeLights          = 0x0042E3B0;
// The box, as the engine pushes it: the addresses of its own four floats.
constexpr uintptr_t CBridge__LinksX1                         = 0x005EC688;   // float, -330
constexpr uintptr_t CBridge__LinksX2                         = 0x005EC684;   // float, -230
constexpr uintptr_t CBridge__LinksY1                         = 0x005EC680;   // float, -700
constexpr uintptr_t CBridge__LinksY2                         = 0x005EC67C;   // float, -588

// ---- the police helicopter ------------------------------------------------
//
// Verified 2026-09-23 against the retail image. re3's Heli.cpp was the map;
// every address, offset and constant below was read off the bytes, and the
// one place the two disagree is noted.
//
// **Who reads CHeli::pHelis.** A byte scan of the whole file for the four
// slot addresses as absolutes (0x0072CF50/54/58/5C) finds them in 0x00549970
// (InitHelis), UpdateHelis, GenerateHeli, the Catalina functions at
// 0x0054A9B0..0x0054AA20, both collision tests and SpecialHeliPreRender - the
// whole of Heli.cpp, from 0x0054999A to 0x0054AE16, and nothing outside it.
// So a CHeli that is not in the array is invisible to every one of them,
// which is what makes a replica possible (game/heli.cpp).
//
// **Who calls them.** An E8/E9 scan for each entry point:
//   UpdateHelis           one call, 0x0048C909, in CGame::Process
//   SpecialHeliPreRender  one call, 0x004A78A0, the tail of CRenderer::PreRender
//   TestBulletCollision   0x0055D93F and 0x0055DB0F (CWeapon::FireInstantHit)
//                         and 0x0056233D (the function after FireSniper, re3's
//                         FireM16_1stPerson); every one `push 4` for damage
//   TestRocketCollision   0x0055B8E2 and 0x0055B9BC, both in
//                         CProjectileInfo::Update, both on a stack copy of the
//                         projectile's position and only when [info] == 8
//   CHeli::CHeli          0x0054A67C / 0x0054A6A1 (GenerateHeli) and 0x00595493
//   ProcessControl        no direct call; slot 8 of the vtable (0x00601ED0)
//
// **The constructor** (0x00547220, __thiscall(int model, uint8 createdBy),
// `ret 8`) calls CVehicle's (0x00550A60), stamps the vtable 0x00601EB0 at
// 0x0054723B, writes m_vehType = 3 at +0x284, calls SetModelIndex through slot
// 3, and then fills the members below in re3's order. mass and turn mass are
// 1e8 (4CBEBC20h), the dust heights -50 (C2480000h), and m_nLastShotTime at
// +0x304 is never written - re3's "BUG" comment is right about the retail
// build. SetStatus writes `and al,7 / or al,40h`, STATUS_HELI (8).
//
// **GenerateHeli** (0x0054A640, cdecl(bool catalina)) is the registration
// CoopIII copies for a replica: CVehicle::operator new(33Ch), the constructor
// with 7Dh (MI_CHOPPER) and 4 (PERMANENT_VEHICLE), SetTranslate 250 m from
// FindPlayerCoors at (rand & 0FFh) * 6.28/256 and 50 m up (0x006019D0), mirrored
// to the other side when that leaves +-2000; SetStatus(4), ABANDONED;
// `or dl,8` into +0x1F5, bIsLocked; the lowest m_nHeliId no slot holds; and
// CWorld::Add. It does NOT set the col model - InitHelis does, once, for
// both helicopter models (0x005499C7/0x005499D7, ms_colModelPed1 at 0x00726CB0),
// which is re3's GTA3_PS2_160 branch rather than the one its #if picks for PC.
//
// **UpdateHelis** (0x005499F0, cdecl, no arguments):
//   0x00549A18  FindPlayerPed()->m_pWanted->NumOfHelisRequired()   (0x004ADC00)
//   0x00549A34  every 15 s (`add eax,3A98h` into 0x008F1A7C), while
//               NumRandomHelis (int16 0x0095CCAA) is short, GenerateHeli into
//               pHelis[0], else pHelis[1], m_heliType = 0
//   0x00549B81  FLY_AWAY (2) and z > 150 (0x00601BC4): CWorld::Remove, the
//               deleting destructor, slot nulled, NumRandomHelis-- for 0 and 1
//   0x00549C4B  SHOT_DOWN (3) and `CTimer::GetTimeInMilliseconds() >
//               m_nExplosionTimer` (unsigned, `jbe` skips): the explosion.
//               AddExplosion(nil, nil, 5, pos, 0); SpawnFlyingComponent 6, 7
//               and 2; CDarkel::RegisterCarBlownUpByPlayer at 0x0054A04F;
//               CWorld::Remove and delete; then
//                 0x0054A0C1  inc [008E2A64h]         CStats::HelisDestroyed
//                 0x0054A0C7  add [008F1B7Ch],2       PeopleKilledByPlayer
//                 0x0054A0CE  add [00880DD4h],2       PedsKilledOfThisType[COP]
//                 0x0054A0DF  add [PlayerInFocus's m_nMoney],0FAh      $250
//                 0x0054A134  m_pWanted->RegisterCrime_Immediately(0Ch, pos,
//                             slot + 4D83h, 0)       CRIME_SHOOT_HELI
//                 0x0054A13E  TestForNewRandomHelisTimer = now + 50000
//   0x0054A17B  SHOT_DOWN and within 7 s of the timer: on the first frame of
//               that window (previous time + 7000 < timer), components 3 and 4,
//               m_fAngularSpeed *= -2.5, `or dl,10h` into +0x52
//               (bRenderScorched), and AddExplosion(nil, nil, 5, pos - 2.5 *
//               forward, 0); every other frame m_fAngularSpeed *= 1.03
//   0x0054A39D  for pHelis[0] and [1] not flying away, each one past
//               NumOfHelisRequired is set to FLY_AWAY
//   0x0054A402  FindPlayerCoors().z < -2 (0x00601BC8): every one not shot
//               down flies away
// The PeopleKilledByPlayer and PedsKilledOfThisType addresses are the ones
// CDarkel::RegisterKillByPlayer bumps (0x00421013, 0x0042103C with base
// 0x00880DBC; +18h is index 6, PEDTYPE_COP), so those two are cross-checked.
// HelisDestroyed is re3's name for the one before them and nothing else here
// reads it.
//
// **NumOfHelisRequired** (0x004ADC00, __thiscall on CWanted): 0 when either of
// the two bits at +0x16 is set (m_bIgnoredByCops, m_bIgnoredByEveryone), else
// a jump table (0x005F781C) on `m_nWantedLevel - 3`: 3 and 4 give 1, 5 and 6
// give 2, anything else 0.
//
// **ProcessControl** (0x00547CC0, __thiscall, plain `ret`) chases
// FindPlayerCoors (first call at 0x00547D1D) and, for m_heliType 0, takes its
// shooting interval from FindPlayerPed()->m_pWanted->m_nWantedLevel at
// 0x00549269: a table at 0x00601E30 gives 999999 below 3 stars, then 10000,
// 5000, 3500 and 2000 ms, halved in a no-police zone (0x00525CA0). It ends
// with CPhysical::RemoveAndAdd, `or al,40h` into +0x51 (bIsInSafePosition),
// CMatrix::UpdateRW and CEntity::UpdateRwFrame (0x00549829..0x00549842) - the
// tail a replica's detour repeats.
//
// **TestBulletCollision** (0x0054AB30, cdecl(line0, line1, bulletPos, int
// damage) -> bool). For each of the four slots: skip a null one and one with
// bBulletProof (+0x53 bit 0); CCollision::DistToLine(line0, line1, &pos)
// (0x0040DC70) < 5.0 (0x00601B74); bulletPos = line0 + (line1 - line0) *
// max(1, dist - 5) / |line1 - line0|; m_nBulletDamage (+0x308) += damage;
// past 400 (190h) for m_heliType 2, Catalina's, or past 700 (2BCh) for any
// other: m_fAngularSpeed = (rand() < 3FFFh ? 1 : 0) * 0.1 - 0.05,
// m_heliStatus = 3, m_nExplosionTimer = CTimer::m_snTimeInMilliseconds +
// 10000. Every hit past the limit starts the ten seconds again.
//
// **TestRocketCollision** (0x0054AA30, cdecl(CVector *pos) -> bool). For each
// slot: skip null and bExplosionProof (+0x52 bit 1); if the squared distance
// is under 64.0 (0x00601E10), the same three writes. No damage count at all.
//
// **SpecialHeliPreRender** (0x0054AE10, cdecl) is `for 4 slots: if non-null,
// call 0x005477F0`, CHeli::PreRenderAlways (__thiscall), which draws the
// searchlight from +0x2AC/+0x2B0/+0x2C4 and the tail light.
//
// It can't take an inline detour: the loop's `jl` at 0x0054AE27 goes back to
// 0x0054AE13, inside the five bytes a jmp would overwrite, so the second slot
// runs the middle of the jmp (`push ds / popad / popfd`) and the game dies on
// the first frame. Its one caller is CRenderer::PreRender, `call 0054AE10` at
// 0x004A78A0, so that call is what gets redirected.
//
// **SpawnFlyingComponent** (0x0054AE50, __thiscall(int node) -> CObject*,
// `ret 4`) clones the node's atomic from m_aHeliNodes (+0x288 + node * 4) into
// a new CObject and CWorld::Adds it; a null node returns at once.
constexpr uintptr_t CHeli__vtable               = 0x00601EB0;
constexpr uintptr_t CHeli__CHeli                = 0x00547220;
constexpr uintptr_t CHeli__ProcessControl       = 0x00547CC0;
constexpr uintptr_t CHeli__PreRenderAlways      = 0x005477F0;
constexpr uintptr_t CHeli__UpdateHelis          = 0x005499F0;
constexpr uintptr_t CHeli__GenerateHeli         = 0x0054A640;
constexpr uintptr_t CHeli__TestRocketCollision  = 0x0054AA30;
constexpr uintptr_t CHeli__TestBulletCollision  = 0x0054AB30;
constexpr uintptr_t CHeli__SpecialHeliPreRender = 0x0054AE10;
constexpr uintptr_t CRenderer__PreRender_SpecialHeliCall = 0x004A78A0;
constexpr uintptr_t CHeli__SpawnFlyingComponent = 0x0054AE50;
constexpr uintptr_t CHeli__pHelis               = 0x0072CF50;   // CHeli *[4]
constexpr uintptr_t CWanted__NumOfHelisRequired = 0x004ADC00;
constexpr uintptr_t CWanted__RegisterCrime_Immediately = 0x004ADA10;   // thiscall, ret 10h
constexpr uintptr_t CCollision__DistToLine      = 0x0040DC70;   // cdecl -> float
constexpr uintptr_t CGeneral__GetRandomNumber   = 0x005A41D0;   // rand()
constexpr uintptr_t CStats__HelisDestroyed      = 0x008E2A64;
constexpr uintptr_t CStats__PeopleKilledByPlayer = 0x008F1B7C;
constexpr uintptr_t CStats__CopsKilled          = 0x00880DD4;   // PedsKilledOfThisType[6]

constexpr uint16_t MI_CHOPPER       = 125;
constexpr int      HELI_SLOTS       = 4;
constexpr size_t   SIZEOF_HELI      = 0x33C;
constexpr uint8_t  HELI_TYPE_RANDOM   = 0;
constexpr uint8_t  HELI_TYPE_CATALINA = 2;
// The nodes UpdateHelis throws off: 3 and 4 in the first half, 6, 7 and 2 in
// the second.
constexpr int      HELI_NODE_TOPROTOR   = 2;
constexpr int      HELI_NODE_BACKROTOR  = 3;
constexpr int      HELI_NODE_TAIL       = 4;
constexpr int      HELI_NODE_SKID_LEFT  = 6;
constexpr int      HELI_NODE_SKID_RIGHT = 7;
constexpr int      EXPLOSION_HELI       = 5;
constexpr int      CRIME_SHOOT_HELI     = 12;
constexpr uint32_t HELI_CRIME_ID_BASE   = 0x4D83;   // + slot
constexpr int32_t  HELI_SHOOT_DOWN_MONEY = 250;
constexpr uint32_t HELI_BULLET_LIMIT          = 700;
constexpr uint32_t HELI_BULLET_LIMIT_CATALINA = 400;
constexpr uint32_t HELI_EXPLODE_AFTER_MS      = 10000;
constexpr float    HELI_BULLET_RADIUS         = 5.0f;
constexpr float    HELI_ROCKET_RADIUS_SQ      = 64.0f;
constexpr float    HELI_FIRST_BLAST_BACK      = 2.5f;
constexpr float    HELI_MOVE_SPEED_TO_MPS     = 50.0f;   // CTimer, 0x004AD232

namespace offs {
constexpr size_t HELI_NODES              = 0x288;   // RwFrame *[8]
constexpr size_t HELI_STATUS             = 0x2A8;   // int8
constexpr size_t HELI_SEARCHLIGHT_X      = 0x2AC;
constexpr size_t HELI_SEARCHLIGHT_Y      = 0x2B0;
constexpr size_t HELI_EXPLOSION_TIMER    = 0x2B4;   // uint32, CTimer ms
constexpr size_t HELI_ROTATION           = 0x2B8;
constexpr size_t HELI_ANGULAR_SPEED      = 0x2BC;
constexpr size_t HELI_SEARCHLIGHT_INTENSITY = 0x2C4;
constexpr size_t HELI_ID                 = 0x2C8;   // int8
constexpr size_t HELI_TYPE               = 0x2C9;   // int8
constexpr size_t HELI_BULLET_DAMAGE      = 0x308;   // uint32
} // namespace offs

// ---- the police helicopter's gun -------------------------------------------
//
// Verified 2026-09-23 against the retail image with dumpbin /disasm. re3's
// Heli.cpp:462-525 and Weapon.cpp:2061-2170 were the map; every number below
// was read off the bytes.
//
// **When it fires.** Inside CHeli::ProcessControl (0x00547CC0), after the
// searchlight is worked out:
//   0x005491E4  m_fSearchLightIntensity < 0.9 (0x00601BB0), or the player
//               more than 7 m from the light (sq distance vs 49.0 at
//               0x00601BB4): m_nShootTimer (+0x300) = now, and nothing fires
//   0x0054922D  otherwise, and past m_nPoliceShoutTimer (+0x330): the
//               "found you" shout, PlayOneShot(m_audioEntityId, 6Bh, 0.0)
//   0x00549260  m_heliType 0: the interval from FindPlayerPed()->m_pWanted's
//               level (0x00549269), table 0x00601E30 = 999999 ms for 0..2
//               stars, 10000, 5000, 3500, 2000 for 3..6; halved when
//               CCullZones::NoPolice (0x00525CA0). Any other type: 1500
//   0x005492DC  m_bIgnoredByCops / m_bIgnoredByEveryone (+0x16 of CWanted):
//               m_nShootTimer = now, nothing fires
//   0x005492F5  the frame the interval runs out: GetIsLineOfSightClear(pos,
//               FindPlayerCoors, buildings only); blocked resets the timer
//   0x00549370  now > m_nShootTimer + interval AND now > m_nLastShotTime
//               (+0x304) + 200 (`add eax,0C8h` at 0x0054938A): one round
//
// So there is no burst object in the engine. Once the interval has run out
// the helicopter fires one round each time 200 ms have passed, on the first
// frame that notices, for as long as the light holds the player - roughly
// five a second - and stops the moment the light slips.
//
// **One round** (0x0054939B..0x0054958C):
//   target = FindPlayerCoors + ((rand & 0FFh) - 128) * 0.02 (0x00601BA4) in x
//            and again in y, then + 3.0 (0x00601BAC) * dir
//   dir    = Normalise(FindPlayerCoors - pos), CVector::Normalise 0x004BA560
//   source = pos + 3.0 * dir, pos being [ebp+34h] (edi, 0x005490D7)
//   FireOneInstantHitRound(&source, &target, 20)   `push 14h`, 0x00549569
//   DMAudio.PlayOneShot(m_audioEntityId, 2Fh, 0.0)  0x00549582
//   m_nLastShotTime = now                            0x0054958C
// `source` is exactly three metres from the helicopter's own position, which
// is how game/heligun.cpp tells which of the two police slots fired without
// needing a detour on ProcessControl.
//
// **FireOneInstantHitRound** (0x00563B00, cdecl(CVector *source, CVector
// *target, int damage), plain `ret`). Three callers in the image: the one
// above, returning to 0x0054956E, and two at 0x005644F0 / 0x00564600 that
// are not the helicopter's. In order:
//   0x00563B56  CParticle::AddParticle(0Ch GUNFLASH, source, zero, nil, 0.0,
//               0, 0, 0, 0)
//   0x00563BBB  CPointLights::AddLight(0, source, zero, 5.0, 1.0, 0.8, 0.0,
//               0, false)
//   0x00563BDD  CWorld::ProcessLineOfSight(source, target, colPoint, victim,
//               1, 1, 1, 1, 1, 1, 0)
//   0x00563C43  CParticle::AddParticle(2Fh HELI_ATTACK, source, (target -
//               source) * 0.15 (0x00603058), ...) - the tracer
//   ped victim not dying: CAnimManager::AddAnimation for the hit reaction,
//               CPed::InflictDamage(nil, 3 UZI, damage, piece, dir) at
//               0x00563D0C, four blood particles when on screen
//   vehicle:    CVehicle::InflictDamage(nil, 3, damage) at 0x00563E0D
//   then a switch on the victim's type (table 0x00603238, type - 1):
//     building  PlayOneShotScriptObject(6Ah, &point) + AddParticle(10h SMOKE,
//               point, (0, 0, 0.01))
//     vehicle   DMAudio.PlayOneShot(victim audio, 38h, 1.0)
//     ped       DMAudio.PlayOneShot(victim audio, 37h, 1.0) + CPed::Say(65h)
//     object    PlayOneShotScriptObject(6Bh, &point)
//     dummy     PlayOneShotScriptObject(6Ch, &point)
//   no victim:  CWaterLevel::GetWaterLevel(target.x, target.y, target.z + 10,
//               &level, false); if water, AddParticle(26h BOAT_SPLASH, (x, y,
//               level), (0, 0, 0.01)) and PlayOneShotScriptObject(6Dh) at that
//               same splash position. re3 plays it at point.point; retail
//               builds the vector at [esp+70h] and passes that.
// It never calls CHeli::TestBulletCollision and never reaches
// CObject::ObjectDamage: the helicopter's gun hurts peds and vehicles only.
//
// **What an observer calls instead.** Everything above except the two
// InflictDamage calls, the hit animation, the blood and Say. The whole
// transitive call graph of the six functions below was walked over the image
// (every E8/E9 target, function bounds from the IDA export) and contains none
// of CPed::InflictDamage, CVehicle::InflictDamage, CObject::ObjectDamage,
// CExplosion::AddExplosion, CHeli::TestBulletCollision, CPed::SetDie,
// CAutomobile::BlowUpCar, CWeapon::Fire or FireOneInstantHitRound. The only
// indirect calls in it are OS imports (0x0061Dxxx), RwEngineInstance's
// malloc/free (+0x130/+0x134 off 0x00661228) and the CRT. The same walk from
// CParticle::Update (0x0050DCF0, called by CGame::Process right before
// gFireManager.Update) finds nothing either, so a tracer particle cannot do
// damage later. The walk from 0x00563B00 itself does find both InflictDamage
// calls, which is the control that says the walk works.
constexpr uintptr_t FireOneInstantHitRound        = 0x00563B00;
constexpr uintptr_t HELI_SHOT_RETURN_ADDRESS      = 0x0054956E;
constexpr uintptr_t CParticle__AddParticle        = 0x0050D140;   // cdecl, 9 args
constexpr uintptr_t CPointLights__AddLight        = 0x00510790;   // cdecl, 13 dwords
constexpr uintptr_t CAudioEngine__PlayOneShot     = 0x0057C840;   // thiscall on DMAudio, ret 0Ch
constexpr uintptr_t PlayOneShotScriptObject       = 0x0057C5F0;   // cdecl(uint8, CVector *)
constexpr uintptr_t CWaterLevel__GetWaterLevel    = 0x005552C0;   // cdecl, 5 args -> bool

constexpr int32_t  PARTICLE_GUNFLASH    = 0x0C;
constexpr int32_t  PARTICLE_SMOKE       = 0x10;
constexpr int32_t  PARTICLE_BOAT_SPLASH = 0x26;
constexpr int32_t  PARTICLE_HELI_ATTACK = 0x2F;
constexpr uint16_t SOUND_WEAPON_SHOT_FIRED = 0x2F;
constexpr uint16_t SOUND_WEAPON_HIT_PED    = 0x37;
constexpr uint16_t SOUND_WEAPON_HIT_VEHICLE = 0x38;
constexpr uint8_t  SCRIPT_SOUND_BULLET_HIT_GROUND_1 = 0x6A;
constexpr uint8_t  SCRIPT_SOUND_BULLET_HIT_GROUND_2 = 0x6B;
constexpr uint8_t  SCRIPT_SOUND_BULLET_HIT_GROUND_3 = 0x6C;
constexpr uint8_t  SCRIPT_SOUND_BULLET_HIT_WATER    = 0x6D;
constexpr float    HELI_SHOT_MUZZLE_OFFSET = 3.0f;    // 0x00601BAC
constexpr float    HELI_TRACER_SPEED_SCALE = 0.15f;   // 0x00603058
constexpr float    HELI_SHOT_LIGHT_RADIUS  = 5.0f;    // 0x006030DC
constexpr float    HELI_SHOT_WATER_PROBE   = 10.0f;   // 0x00603068
constexpr float    HELI_IMPACT_DRIFT_Z     = 0.01f;   // 3C23D70Ah

namespace offs {
constexpr size_t HELI_SHOOT_TIMER        = 0x300;   // uint32, CTimer ms
constexpr size_t HELI_LAST_SHOT_TIME     = 0x304;   // uint32, CTimer ms
constexpr size_t PHYSICAL_AUDIO_ENTITY   = 0x64;    // int32, CVehicle ctor 0x00550F45
} // namespace offs

// ---- startup state machine ------------------------------------------------
//
// gGameState, the variable WinMain (0x00582710) switches on. Verified by
// disassembling the GS_FRONTEND case at 0x00582E23, statement for statement
// against re3's win.cpp:2414-2429:
//
//     lea  eax, [esp+5Ch] / push eax / push [ecx] / call [GetWindowPlacement]
//     cmp  dword [esp+64h], 2          ; wp.showCmd == SW_SHOWMINIMIZED
//     jz   skip
//     push 0 / push 1Bh                ; RsEventHandler(rsFRONTENDIDLE, nil)
//     call 0x00584A20
//     cmp  byte [0x008F5AE9], 0        ; FrontEndMenuManager.m_bMenuActive
//     ...
//     mov  dword [0x008F5838], 8       ; gGameState = GS_INIT_PLAYING_GAME
//
// Also backed by the seven `mov dword [0x008F5838], n` assignments inside
// WinMain, which take exactly the values 1, 2, 4, 6, 7, 8, 9 - the transitions
// re3 makes with a literal. 3 and 5 are reached via `++gGameState` instead, so
// they never show up as an immediate.
//
// Worth knowing as a readiness signal: it's the only "the game is up"
// indicator that doesn't go through CTimer, and CTimer is disputed (see the
// long note in game/frame.cpp). WinMain sets gGameState itself, so GS_FRONTEND
// means its loop is turning and every plugin's DllMain is long finished.
constexpr uintptr_t gGameState = 0x008F5838;

enum eGameState {
	GS_START_UP          = 0,
	GS_INIT_LOGO_MPEG    = 1,
	GS_LOGO_MPEG         = 2,   // WinMain calls CPad::UpdatePads here
	GS_INIT_INTRO_MPEG   = 3,
	GS_INTRO_MPEG        = 4,   // and here
	GS_INIT_ONCE         = 5,
	GS_INIT_FRONTEND     = 6,
	GS_FRONTEND          = 7,   // the menu; FrontendIdle runs every frame
	GS_INIT_PLAYING_GAME = 8,
	GS_PLAYING_GAME      = 9,
};

// FrontendIdle - the frontend's per-frame function, only reached through
// RsEventHandler(rsFRONTENDIDLE). Nothing hooks it yet; it's here because it
// proves CPad::UpdatePads runs on the title screen.
constexpr uintptr_t FrontendIdle        = 0x0048E700;
constexpr uintptr_t AppEventHandler     = 0x0048E800;   // main.cpp's RsEventHandler
constexpr uintptr_t RsEventHandler      = 0x00584A20;   // skeleton's dispatcher
constexpr uintptr_t WinMain_            = 0x00582710;
constexpr uintptr_t FrontEndMenuManager = 0x008F59D8;   // `mov ecx` before Process

// bool. Read by the GS_FRONTEND case to decide whether to start the game.
constexpr uintptr_t CMenuManager__m_bMenuActive = 0x008F5AE9;

// ---- input ----------------------------------------------------------------
//
// The whole input surface, verified 2026-09-21 against the retail image
// rather than trusted off re3's declarations - re3 said where to look, the
// binary said whether it was true. Provenance is below for each fact, since
// anything writing into these structures does it every frame, and a wrong
// offset here means a write into a neighbouring global.
//
// CPad::GetPad (0x00492F60) is sixteen bytes and gives two facts at once:
//
//     mov  eax, [esp+4]          ; __cdecl, static member
//     imul eax, eax, 0FCh        ; sizeof(CPad)   == 0xFC
//     add  eax, 6F0360h          ; CPad::Pads     == 0x006F0360
//     ret
//
// CPad::UpdatePads (0x00492720) is __cdecl/void. It ends `pop edi / pop esi /
// pop ebx / ret`, a plain ret, so the caller cleans nothing. Its body matches
// re3's MASTER, non-FIX_BUGS branch statement for statement:
//
//     GetPad(0)->UpdateMouse();  CapturePad(0);
//     ControlsManager.ClearSimButtonPressCheckers/AffectPadFromKeyBoard/
//                     AffectPadFromMouse();
//     if (!CReplay::IsPlayingBackFromFile()) GetPad(0)->Update(0);
//     GetPad(1)->NewState.Clear();  GetPad(1)->OldState.Clear();
//     OldKeyState = NewKeyState;    NewKeyState = TempKeyState;
//
// The second-to-last line is where three of the addresses below come from:
// the call at 0x0049278B returns Pads[1], and the very next instruction is
// `add eax, 2Ah` before the OldState clear. That pins sizeof(CControllerState)
// at 0x2A and OldState at CPad+0x2A.

constexpr uintptr_t CPad__UpdatePads  = 0x00492720;
constexpr uintptr_t CPad__Update      = 0x00492C70;
constexpr uintptr_t CPad__GetPad      = 0x00492F60;
constexpr uintptr_t CPad__UpdateMouse = 0x00491CA0;   // first call in UpdatePads
constexpr uintptr_t CPad__Pads        = 0x006F0360;

// CPad::NewKeyState / OldKeyState / TempKeyState.
//
// From the two struct assignments that close UpdatePads. Each is compiled as
// `movsd x6` (F[12], 0x18 bytes) + `rep movsd` of 0x80 dwords (VK_KEYS[256],
// 0x200 bytes) + an unrolled 16-bit copy of the 44 named-key members:
//
//   0x004927BA  rep movsd  edi=006F1E88  esi=006E60E8   (both +0x18)
//   0x00492A17  rep movsd  edi=006E60E8  esi=00774E00   (both +0x18)
//
// so the first is OldKeyState = NewKeyState and the second is NewKeyState =
// TempKeyState. The unrolled tail then reads 006E62E8..006E633E and writes
// 006F2088..006F208E onward (NewKeyState+0x218..+0x26E), walking re3's field
// order (ESC, INS, DEL, HOME, END, ..., APPS) and stopping exactly at the end
// of a 0x270-byte struct. All 44 members were checked against that copy.
//
// What matters practically: UpdatePads writes NewKeyState last, so a hook
// running after it can overwrite NewKeyState and nothing that frame undoes
// it. OldKeyState picks up the injected value next frame, so the frontend's
// JustDown edge detection still works.
constexpr uintptr_t CPad__NewKeyState  = 0x006E60D0;
constexpr uintptr_t CPad__OldKeyState  = 0x006F1E70;
constexpr uintptr_t CPad__TempKeyState = 0x00774DE8;

// CPad::NewMouseControllerState and friends, from the two 0x10-byte copies
// that close CPad::UpdateMouse (0x00491CA0):
//
//     OldMouseControllerState = NewMouseControllerState;   ; 8809F0 -> 8472A0
//     NewMouseControllerState = PCTempMouseControllerState; ; 6F1E60 -> 8809F0
//
// Each copy moves seven bytes, then an fld/fstp of a dword at +0x08 and
// another at +0x0C - matches CMouseControllerState exactly as re3 declares
// it, and confirms the two floats sit at +0x08 and +0x0C.
//
// This is the camera. The PC follow camera doesn't read the right stick:
// CCam::Process_FollowPedWithMouse calls CPad::GetMouseX()/GetMouseY(), which
// are literally `return NewMouseControllerState.x/y`. Mouse buttons reach
// NewState through ControlsManager.AffectPadFromMouse(), but mouse movement
// never does. So driving the camera means writing these floats directly.
constexpr uintptr_t CPad__NewMouseControllerState    = 0x008809F0;
constexpr uintptr_t CPad__OldMouseControllerState    = 0x008472A0;
constexpr uintptr_t CPad__PCTempMouseControllerState = 0x006F1E60;

namespace pad {

// CControllerState: 21 int16 in re3's order, no padding. Proved two ways: the
// `add eax,2Ah` above, and CPad::Update's prologue, which unrolls
// `OldState = NewState` as exactly 21 `mov ax,[ebx+n] / mov
// [ebx+n+2Ah],ax` pairs running n = 0x00, 0x02 ... 0x28 and stopping there.
constexpr size_t SIZEOF_CONTROLLERSTATE = 0x2A;
constexpr size_t CONTROLLER_FIELDS      = 21;

// Field indices into that array of 21, in re3 Pad.h's declaration order - the
// same order the unrolled copy above walks.
enum Field {
	LEFT_STICK_X = 0, LEFT_STICK_Y,
	RIGHT_STICK_X,    RIGHT_STICK_Y,
	LEFT_SHOULDER1,   LEFT_SHOULDER2,
	RIGHT_SHOULDER1,  RIGHT_SHOULDER2,
	DPAD_UP,          DPAD_DOWN,      DPAD_LEFT, DPAD_RIGHT,
	START,            SELECT,
	SQUARE,           TRIANGLE,       CROSS,     CIRCLE,
	LEFT_SHOCK,       RIGHT_SHOCK,
	NETWORK_TALK,
};

// CPad member offsets. NEWSTATE/OLDSTATE verified directly; the three PCTemp
// states follow on 0x2A strides. PCTEMP_JOY also gets an independent
// confirmation from the `lea eax,[ebx+7Eh]` CPad::Update uses to reach it
// right after the OldState copy.
constexpr size_t SIZEOF_PAD    = 0xFC;
constexpr size_t NEWSTATE      = 0x00;
constexpr size_t OLDSTATE      = 0x2A;
constexpr size_t PCTEMP_KEY    = 0x54;
constexpr size_t PCTEMP_JOY    = 0x7E;
constexpr size_t PCTEMP_MOUSE  = 0xA8;

// CPad::DisablePlayerControls - a uint8 bitmask, the engine's own way of
// taking controls off the player without touching the menu. 38 sites read it,
// and every movement/steering/weapon accessor in CPad opens with
// `if (ArePlayerControlsDisabled()) return 0`. The menu is unaffected since it
// reads NewState directly instead of going through those accessors.
//
// Arithmetic gets you here too (five 0x2A controller states to 0xD2, then
// Phase, Mode, ShakeDur, ShakeFreq, bHornHistory[5], iCurrHornHistory), and
// the rest of re3's declaration closing exactly on SIZEOF_PAD is a good sign.
// But the actual proof is the write sites below, which also pin the bit
// values:
//
//   0x004048C0  or  byte [+0xDF], 0x80     0x004049D5  and ..., 0x7F   CUTSCENE
//   0x00468F7A  or  byte [+0xDF], 0x01     0x00468342  and ..., 0xFE   CAMERA
//   0x004224B7  or  byte [+0xDF], 0x04     0x00422708  and ..., 0xFB   GARAGE
//     (six of each, which is re3's twelve GARAGE uses)
//   0x0042FB3E  or  byte [+0xDF], 0x40     0x0042F58F  and ..., 0xBF   PHONE
//   0x004A1437  or  byte [+0xDF], 0x20     0x004A1559  and ..., 0xDF   PLAYERINFO
//     (inside CPlayerInfo::MakePlayerSafe and its opposite)
//
// Every named bit in re3's enum has a matching set/clear pair, and nothing
// anywhere writes bit 1, 3 or 4 - re3's PLAYERCONTROL_UNK2, _UNK8 and _UNK10,
// the three it never found a use for. That's what makes one of them safe for
// CoopIII to claim: we can set and clear our own bit without disturbing a
// lock the game put on for its own reasons.
constexpr size_t DISABLE_PLAYER_CONTROLS = 0xDF;

constexpr uint8_t PLAYERCONTROL_ENABLED    = 0x00;
constexpr uint8_t PLAYERCONTROL_CAMERA     = 0x01;
constexpr uint8_t PLAYERCONTROL_GARAGE     = 0x04;
constexpr uint8_t PLAYERCONTROL_PLAYERINFO = 0x20;
constexpr uint8_t PLAYERCONTROL_PHONE      = 0x40;
constexpr uint8_t PLAYERCONTROL_CUTSCENE   = 0x80;
// Ours. Bit 1 (re3's PLAYERCONTROL_UNK2) because the retail image never
// writes it (see above).
constexpr uint8_t PLAYERCONTROL_COOPIII    = 0x02;

static_assert(DISABLE_PLAYER_CONTROLS > PCTEMP_MOUSE + SIZEOF_CONTROLLERSTATE &&
                  DISABLE_PLAYER_CONTROLS < SIZEOF_PAD,
              "DisablePlayerControls is past the five controller states and "
              "inside the CPad");
static_assert((PLAYERCONTROL_COOPIII &
               (PLAYERCONTROL_CAMERA | PLAYERCONTROL_GARAGE |
                PLAYERCONTROL_PLAYERINFO | PLAYERCONTROL_PHONE |
                PLAYERCONTROL_CUTSCENE)) == 0,
              "our bit must not collide with one the game uses");

static_assert(OLDSTATE == NEWSTATE + SIZEOF_CONTROLLERSTATE,
              "OldState follows NewState (add eax,2Ah in UpdatePads)");
static_assert(PCTEMP_JOY == PCTEMP_KEY + SIZEOF_CONTROLLERSTATE,
              "PCTempJoyState follows PCTempKeyState (lea eax,[ebx+7Eh])");
static_assert(PCTEMP_MOUSE == PCTEMP_JOY + SIZEOF_CONTROLLERSTATE,
              "PCTempMouseState follows PCTempJoyState (re3 Pad.h:140-144)");
static_assert(PCTEMP_MOUSE + SIZEOF_CONTROLLERSTATE < SIZEOF_PAD,
              "all five controller states lie inside a 0xFC-byte CPad");
static_assert(SIZEOF_CONTROLLERSTATE == CONTROLLER_FIELDS * 2,
              "CControllerState is 21 int16 and nothing else");

// CMouseControllerState, 0x10 bytes. Offsets read off the copies in
// CPad::UpdateMouse; the two floats are the camera's entire input on PC.
constexpr size_t SIZEOF_MOUSESTATE = 0x10;
constexpr size_t MOUSE_LMB     = 0x00;
constexpr size_t MOUSE_RMB     = 0x01;
constexpr size_t MOUSE_MMB     = 0x02;
constexpr size_t MOUSE_WHEELUP = 0x03;
constexpr size_t MOUSE_WHEELDN = 0x04;
constexpr size_t MOUSE_MXB1    = 0x05;
constexpr size_t MOUSE_MXB2    = 0x06;
constexpr size_t MOUSE_X       = 0x08;   // float, CPad::GetMouseX()
constexpr size_t MOUSE_Y       = 0x0C;   // float, CPad::GetMouseY()

static_assert(MOUSE_Y == MOUSE_X + 4 && MOUSE_Y + 4 == SIZEOF_MOUSESTATE,
              "x and y are the last two dwords of CMouseControllerState");

// CKeyboardState, 0x270 bytes. F[12] then VK_KEYS[256] then 44 named keys,
// every one of which was found in UpdatePads' unrolled copy at the offset
// below. These are what the frontend menu actually reads: CPad::GetUp() is
// `NewKeyState.UP`, GetEnter() is `NewKeyState.EXTENTER`, GetEscapeJustDown()
// is `NewKeyState.ESC && !OldKeyState.ESC`.
constexpr size_t SIZEOF_KEYSTATE = 0x270;
constexpr size_t KEY_F0          = 0x000;   // int16 F[12]
constexpr size_t KEY_VK_KEYS     = 0x018;   // int16 VK_KEYS[256]
constexpr size_t KEY_ESC         = 0x218;
constexpr size_t KEY_INS         = 0x21A;
constexpr size_t KEY_DEL         = 0x21C;
constexpr size_t KEY_HOME        = 0x21E;
constexpr size_t KEY_END         = 0x220;
constexpr size_t KEY_PGUP        = 0x222;
constexpr size_t KEY_PGDN        = 0x224;
constexpr size_t KEY_UP          = 0x226;
constexpr size_t KEY_DOWN        = 0x228;
constexpr size_t KEY_LEFT        = 0x22A;
constexpr size_t KEY_RIGHT       = 0x22C;
constexpr size_t KEY_ENTER       = 0x23C;   // numeric-keypad Enter
constexpr size_t KEY_BACKSP      = 0x254;
constexpr size_t KEY_TAB         = 0x256;
constexpr size_t KEY_EXTENTER    = 0x25A;   // the main Return, CPad::GetEnter()
constexpr size_t KEY_LSHIFT      = 0x25C;
constexpr size_t KEY_SHIFT       = 0x260;
constexpr size_t KEY_APPS        = 0x26E;   // last member

static_assert(KEY_VK_KEYS == KEY_F0 + 12 * 2, "VK_KEYS follows F[12]");
static_assert(KEY_ESC == KEY_VK_KEYS + 256 * 2, "ESC follows VK_KEYS[256]");
static_assert(KEY_APPS + 2 == SIZEOF_KEYSTATE, "APPS is the final member");

} // namespace pad

// ---- pools ----------------------------------------------------------------
//
// docs/protocol.md §1.5: pool handles are per-process. These exist to
// translate a local handle to an entity - never to put a handle on the wire.

constexpr uintptr_t CPools__ms_pPedPool     = 0x008F2C60;
constexpr uintptr_t CPools__ms_pVehiclePool = 0x009430DC;
constexpr uintptr_t CPools__ms_pObjectPool  = 0x00880E28;

constexpr uintptr_t CPools__GetPed        = 0x004A1AA0;
constexpr uintptr_t CPools__GetPedRef     = 0x004A1A80;
constexpr uintptr_t CPools__GetVehicle    = 0x004A1AE0;
constexpr uintptr_t CPools__GetVehicleRef = 0x004A1AC0;
constexpr uintptr_t CPools__GetObject     = 0x004A1B20;
constexpr uintptr_t CPools__GetObjectRef  = 0x004A1B00;
constexpr uintptr_t CPools__Initialise    = 0x004A1770;

// ---- streaming ------------------------------------------------------------
//
// docs/protocol.md §1.6: spawning a remote player is two-phase. Request the
// model, then create the entity on a later frame once it's loaded.

constexpr uintptr_t CStreaming__RequestModel           = 0x00407EA0;
constexpr uintptr_t CStreaming__LoadAllRequestedModels = 0x0040A440;
constexpr uintptr_t CStreaming__RemoveModel            = 0x00408830;
constexpr uintptr_t CStreaming__ms_aInfoForModel       = 0x006C7088;
constexpr uintptr_t CStreaming__ms_numModelsRequested  = 0x008E2C10;

constexpr uintptr_t CModelInfo__ms_modelInfoPtrs = 0x0083D408;

// CStreaming::HasModelLoaded doesn't exist as a callable function - re3
// declares it inline and the compiler inlined it at 38+ sites. An earlier
// pass proposed an address for it; verification proved that wrong. So we
// inline it ourselves. This is the exact comparison the game makes.
constexpr size_t   STREAMING_INFO_STRIDE     = 0x14;   // sizeof(CStreamingInfo)
constexpr size_t   STREAMING_LOADSTATE_OFFS  = 0x08;   // m_loadState
constexpr uint8_t  STREAMING_LOADED          = 1;      // STREAMSTATE_LOADED
constexpr uint32_t STREAM_OFFSET_TXD         = 0x157C; // txd ids start here

inline bool HasModelLoaded(uint32_t modelId) {
	// A model id off the wire indexes the streaming array with nothing else
	// in the way. Model ids end where txd ids begin (MODELINFO_SIZE below).
	if (modelId >= STREAM_OFFSET_TXD)
		return false;
	const auto *state = reinterpret_cast<const uint8_t *>(
	    CStreaming__ms_aInfoForModel + modelId * STREAMING_INFO_STRIDE +
	    STREAMING_LOADSTATE_OFFS);
	return *state == STREAMING_LOADED;
}

// ---- entity layout --------------------------------------------------------
//
// Field offsets, verified against the binary rather than just computed from
// re3. The base-class chain checks out by arithmetic (CPlaceable is
// vtable(4) + CMatrix(0x48, being RwMatrix 64 + attachment 4 + flag 4), so
// CEntity's members start at 0x4C), and the ped offsets were then confirmed
// by disassembling functions whose re3 source is known:
//
//   CPed::SetStoredState (0x004C5DB0) reads [+0x228], [+0x224], [+0x22C] in
//   exactly re3's statement order (Ped.cpp:614-623).
//
//   CPed::SetDie (0x004D37D0) calls SetStoredState, then ClearAll, then does
//   `mov dword [ebx+0x2C0], 0` and immediately tests [+0x224] against
//   PED_DRIVING, matching re3 Ped.cpp:6318-6321 statement for statement.

namespace offs {

// CPlaceable / CEntity / CPhysical, shared by peds, vehicles and objects.
constexpr size_t VTABLE       = 0x00;
// All four are offsets from the ENTITY, not the matrix. MATRIX_RIGHT is 0x04
// and so is MATRIX, because a CMatrix begins with its right row. Watch out:
// `Field(entity + MATRIX, MATRIX_RIGHT)` adds the four twice and lands one
// float into the matrix, shifting every basis vector along - scrambles the
// rotation, leaves the position (an absolute offset) fine, so you get an
// entity sitting exactly where it should but lying on its side. Cost a full
// in-game test round to track down. Use the entity as the base.
constexpr size_t MATRIX       = 0x04;
constexpr size_t MATRIX_RIGHT = 0x04;
constexpr size_t MATRIX_FWD   = 0x14;
constexpr size_t MATRIX_UP    = 0x24;
constexpr size_t POSITION     = 0x34;   // CMatrix::GetPosition()
constexpr size_t RW_OBJECT    = 0x4C;   // CEntity::m_rwObject, the clump
constexpr size_t ENTITY_FLAGS = 0x50;   // m_type bits 0-2, m_status bits 3-7
constexpr size_t MODEL_INDEX  = 0x5C;

// The rest of CEntity's bitfield block, one byte per re3 flag group. Each bit
// below is witnessed by an instruction in the render path (see the rendering
// section at the end of this file) - so the *order* of the groups is measured,
// not just copied out of re3's header:
//
//   byte A (0x51)  bIsStatic bit 2, bIsInSafePosition bit 6
//                  CWorld::Add tests bIsStatic; CWorld::Process's collision
//                  loops are all `if(!bIsInSafePosition)`
//   byte B (0x52)  bIsVisible bit 2  (SetupEntityVisibility `shr al,2/and al,1`)
//   byte C (0x53)  bZoneCulled bit 6, bZoneCulled2 bit 7
//                  IsEntityCullZoneVisible `shr al,6` and `shr al,7`
//   byte D (0x54)  bImBeingRendered bit 2, bDrawLast bit 5,
//                  bNoBrightHeadLights bit 6
//                  SetupEntityVisibility writes bit 6 in the first-person
//                  vehicle case and reads bit 5 for the alpha list
//   byte E (0x55)  bDistanceFade bit 0 (`and al,0FEh` beside the same code)
constexpr size_t ENTITY_FLAGS_A = 0x51;
constexpr size_t ENTITY_FLAGS_B = 0x52;
constexpr size_t ENTITY_FLAGS_C = 0x53;
constexpr size_t ENTITY_FLAGS_D = 0x54;
constexpr size_t ENTITY_FLAGS_E = 0x55;
constexpr size_t SCAN_CODE      = 0x58;   // uint16

// bUsesCollision is bit 0 of byte A - this pins re3's declaration order for
// the group, not just membership in it. CPed::WarpPedIntoCar clears it
// (`and al,0FEh` on [+0x51]) where re3 has bUsesCollision = false, and
// WARP_CHAR_FROM_CAR_TO_COORD sets it again (`and al,0FEh / or al,1`) where
// re3 has it true. Getting in and out of a car is the only place the engine
// touches this on a ped.
constexpr uint8_t ENTITY_USES_COLLISION       = 0x01;   // byte A
constexpr uint8_t ENTITY_IS_STATIC            = 0x04;   // byte A
constexpr uint8_t ENTITY_IS_IN_SAFE_POSITION  = 0x40;   // byte A
constexpr uint8_t ENTITY_IS_VISIBLE           = 0x04;   // byte B
// Byte B bit 1. This is the one flag in the block witnessed by a function
// that writes five of them in a row: COMMAND_SET_CHAR_PROOFS (opcode 683,
// handler 0x0044562C, off dispatcher 0x00439500 -> the 600..699 range handler
// 0x00444B20, table at 0x005EF424 indexed by opcode - 657, entry 26) matches
// re3 Script3.cpp:1443-1453 statement for statement:
//
//     [eax+53h] bit 0   bBulletProof
//     [eax+53h] bit 1   bFireProof
//     [eax+52h] bit 1   bExplosionProof     <- this one
//     [eax+53h] bit 2   bCollisionProof
//     [eax+53h] bit 3   bMeleeProof
//
// That re-confirms the four byte-C proofs this file already had, and pins the
// odd one out: bExplosionProof isn't in byte C with its siblings, it's bit 1
// of byte B, between bWasPostponed and bIsVisible (re3 Entity.h:53-56).
//
// Matters because CPed::InflictDamage tests it for WEAPONTYPE_ROCKETLAUNCHER,
// GRENADE, EXPLOSION and MOLOTOV (re3 PedFight.cpp:2245-2271) and nothing else
// for those - bFireProof doesn't cover an explosion. Leave this bit unset on a
// remote player and an observer gets to decide their damage the moment
// anything nearby blows up.
constexpr uint8_t ENTITY_EXPLOSION_PROOF      = 0x02;   // byte B
// Byte C, in re3's declaration order (Entity.h:65-72): bBulletProof,
// bFireProof, bCollisionProof, bMeleeProof, bOnlyDamagedByPlayer,
// bStreamingDontDelete, bZoneCulled, bZoneCulled2. ENTITY_ZONE_CULLED landing
// on bit 6 is what pins the order to that list.
constexpr uint8_t ENTITY_BULLET_PROOF         = 0x01;   // byte C
constexpr uint8_t ENTITY_FIRE_PROOF           = 0x02;   // byte C
constexpr uint8_t ENTITY_COLLISION_PROOF      = 0x04;   // byte C
constexpr uint8_t ENTITY_MELEE_PROOF          = 0x08;   // byte C
constexpr uint8_t ENTITY_ZONE_CULLED          = 0x40;   // byte C
constexpr uint8_t ENTITY_DRAW_LAST            = 0x20;   // byte D

// bRemoveFromWorld, byte D bit 0. The one flag in the engine that makes
// CWorld::Process delete an entity in the middle of its own walk over the
// moving list, so it is the one flag an owner of a ped cannot afford to
// ignore. CPed::FlagToDestroyWhenNextProcessed (0x004D6570, vtable slot 16)
// opens by writing exactly this bit: `mov al,[ebx+54h] / and al,0FEh /
// or al,1 / mov [ebx+54h],al`, then goes on to the bInVehicle work at +0x314
// and +0x310 that re3 Ped.cpp:7658-7678 describes.
constexpr uint8_t ENTITY_REMOVE_FROM_WORLD    = 0x01;   // byte D

// CPhysical::m_movingListNode, the CPtrNode this entity holds in
// CWorld::ms_listMovingEntityPtrs, or null when it isn't in the list.
//
// Read straight off both halves of the pair: CPhysical::AddToMovingList
// (0x004958F0) ends `mov [ebx+0E8h], eax` with eax the new node, and
// CPhysical::RemoveFromMovingList (0x00495940) opens
// `mov ecx,[ebx+0E8h] / test ecx,ecx / je`.
constexpr size_t MOVING_LIST_NODE = 0xE8;

// CPed::m_pCurrentPhysSurface. IsEntityCullZoneVisible's PED case reads it
// and tests that entity's bZoneCulled2 - the only other way a ped gets culled
// before SetupEntityVisibility ever sees it.
constexpr size_t PED_CURRENT_PHYS_SURFACE = 0x2FC;

// CPhysical::m_entryInfoList - the list of sector lists this entity is filed
// in. Read straight out of CPhysical::Remove (0x004954B0), whose first
// instruction after the prologue is `mov esi,[ebx+0E4h]`, and again out of
// CPhysical::RemoveAndAdd, which recycles its nodes. CWorld::Process's
// cutscene branch tests it the same way (`cmp [ebp+0E4h],0`).
//
// This is the field that tells you whether an entity is in the world at the
// place it thinks it is. Null means filed nowhere, stale means filed where it
// used to be. The entity's own position won't tell you either way.
constexpr size_t ENTRY_INFO_LIST = 0xE4;

// CBaseModelInfo::m_type, the byte SetupEntityVisibility switches on
// (`mov al,[ebx+2Ah] / cmp al,3` for MITYPE_TIME, `cmp al,1` for MITYPE_SIMPLE).
constexpr size_t MODELINFO_TYPE = 0x2A;
constexpr size_t MOVE_SPEED   = 0x78;   // CPhysical::m_vecMoveSpeed
constexpr size_t TURN_SPEED   = 0x84;   // CPhysical::m_vecTurnSpeed

// CMatrix::m_attachment / m_hasRwMatrix, i.e. MATRIX + 0x40 and + 0x44.
// Confirmed two ways: CPlaceable::CPlaceable (0x0049F9A0) zeroes exactly
// these two before constructing the matrix, and CMatrix::~CMatrix
// (0x004B8DB0, reached from CPlaceable::~CPlaceable) tests `[ebx+0x44]` then
// reads `[ebx+0x40]`, relative to the matrix sitting at +4.
constexpr size_t MATRIX_ATTACHMENT    = 0x44;
constexpr size_t MATRIX_HAS_RW_MATRIX = 0x48;

// CPhysical::m_nZoneLevel (int8). Read straight out of the CREATE_CHAR
// handler: `call CTheZones::GetLevelFromPosition / mov [ebx+0x124], al`.
constexpr size_t ZONE_LEVEL = 0x124;

// CPed.
constexpr size_t PED_FLAGS      = 0x154;
constexpr size_t PED_STATE      = 0x224;   // PedState
constexpr size_t PED_LAST_STATE = 0x228;
constexpr size_t PED_MOVE_STATE = 0x22C;   // eMoveState
constexpr size_t PED_HEALTH     = 0x2C0;
constexpr size_t PED_ARMOUR     = 0x2C4;
constexpr size_t PED_ROT_CUR    = 0x2DC;   // heading is a scalar yaw (§1.7)
constexpr size_t PED_ROT_DEST   = 0x2E0;
constexpr size_t PED_MY_VEHICLE = 0x310;
constexpr size_t PED_IN_VEHICLE = 0x314;

// The objective pair, from CPed::SetObjective itself (0x004D83E0). Past its
// DyingOrDead guard it compares [ebx+0x168] against the new objective, then
// [ebx+0x164] again - re3's `m_prevObjective` and `m_objective`, in that
// order. CPed::WarpPedIntoCar only reads the second one.
constexpr size_t PED_OBJECTIVE      = 0x164;
constexpr size_t PED_PREV_OBJECTIVE = 0x168;

// m_carInObjective. WarpPedIntoCar writes the car here and registers a
// reference to it with the same two instructions it just used for
// m_pMyVehicle, four stores apart, which is what identifies it.
constexpr size_t PED_CAR_IN_OBJECTIVE = 0x170;

// The three fields COMMAND_CREATE_CHAR writes the instant the constructor
// returns, all out of the handler itself (0x0043BB25 onwards) - which is why
// they're verified here and not sitting in the unverified doc:
//
//   mov byte [ebx+0x160], 2        CharCreatedBy = MISSION_CHAR
//   and byte [ebx+0x156], 0xFD     bRespondsToThreats    = false
//   and byte [ebx+0x15A], 0xFD     bAllowMedicsToReviveMe = false
//
// These also confirm PED_FLAGS independently: re3's CPed bitfield block is
// nine bytes of `uint32 x:1` starting at 0x154, so MSVC lays it out as three
// dwords ending at 0x15F with CharCreatedBy right after at 0x160.
// bRespondsToThreats is bit 1 of the third byte, bAllowMedicsToReviveMe is
// bit 1 of the seventh, and both land exactly where the handler writes them.
constexpr size_t PED_FLAGS_C          = 0x156;   // bRespondsToThreats = bit 1
constexpr size_t PED_FLAGS_G          = 0x15A;   // bAllowMedics       = bit 1
constexpr size_t PED_CHAR_CREATED_BY  = 0x160;   // uint8

constexpr uint8_t PED_RESPONDS_TO_THREATS = 0x02;
constexpr uint8_t PED_ALLOW_MEDICS        = 0x02;
constexpr uint8_t PED_FADE_OUT            = 0x80;   // bFadeOut, same byte as G

constexpr size_t SIZEOF_PED        = 0x53C;
constexpr size_t SIZEOF_PLAYER_PED = 0x5F0;   // ped pool slot stride

// ---- CPed: animation, weapons and aim -------------------------------------
//
// Verified 2026-09-21 (milestone M1) by disassembling the retail image. Each
// group below names the function that proves it - none of this is arithmetic
// off re3's declarations alone.
//
// CPed::SetModelIndex (0x004C52A0, vtable slot 3 of CCivilianPed's table,
// read straight out of 0x005F819C) matches re3 Ped.cpp:325-335 statement for
// statement and carries five of these at once:
//
//     mov  eax,[ebx+4Ch]                       GetClump()      -> RW_OBJECT
//     lea  edx,[ebx+1A4h] / push edx           m_pFrames       -> PED_FRAMES
//     call 004C5330 -> mov [ecx+330h],eax      m_pedStats      -> PED_STATS
//     fld  [eax+20h] / fstp [ebx+2E4h]         m_headingRate   -> PED_HEADING_RATE
//     mov  eax,[modelInfo+34h] / mov [ebx+1D4h],eax
//                                              m_animGroup     -> PED_ANIM_GROUP
//     push 3 / push [ebx+1D4h] / push clump / call CAnimManager::AddAnimation
//     ... [clumpData+0Ch] = ebx+1DCh           m_vecAnimMoveDelta
//
// CPed::CPed (0x004C41C0, the first call CCivilianPed::CCivilianPed makes)
// pins the rest by construction order, matching re3 Ped.cpp:59 onward:
//
//     mov  [eax+1DCh],0 / [edx+1E0h],0      m_vecAnimMoveDelta = {0,0}
//     lea  ecx,[eax+1F0h] / push eax
//       / call 004ED010                     m_pedIK(this)      -> PED_IK
//     add  eax,35Ch / push 0Dh / push 18h
//       / call <array ctor>                 m_weapons[13], stride 0x18
//     mov  [ebx+218h],0 / [eax+21Ch],0      m_actionX/m_actionY, i.e. the
//                                           first members after CPedIK's 0x28
//
// Along the way it also writes m_fHealth at 0x2C0 as 42C80000h (100.0f) and
// m_stPathNodeStates[10] as ten 8-byte elements at 0x260, both agreeing with
// offsets this file already had from other functions.

// CPedIK, 0x28 bytes. CPedIK::CPedIK (0x004ED010) is eleven instructions -
// `mov [eax],edx`, then the dwords at 0x24, 0x04, 0x08 ... 0x20 zeroed -
// matching re3 PedIK.cpp's constructor in its own order: m_ped, m_flags, then
// the four LimbOrientation{yaw,pitch} pairs.
constexpr size_t PED_IK                = 0x1F0;   // CPedIK m_pedIK
constexpr size_t PED_IK_HEAD_YAW       = 0x1F4;
constexpr size_t PED_IK_TORSO_YAW      = 0x1FC;   // relative to m_fRotationCur
constexpr size_t PED_IK_TORSO_PITCH    = 0x200;
constexpr size_t PED_IK_UPPERARM_YAW   = 0x204;
constexpr size_t PED_IK_LOWERARM_YAW   = 0x20C;
constexpr size_t PED_IK_FLAGS          = 0x214;   // int32
constexpr size_t SIZEOF_PEDIK          = 0x28;

// CPedIK::m_flags bits. Bit 2 (AIMS_WITH_ARM) is witnessed directly - both
// CPed::SetAimFlag and CPed::ClearAimFlag end on `or`/`and` of
// `dword [ebx+214h]` with 4, on either side of the weapon-flag test.
constexpr int PEDIK_GUN_POINTED_OK    = 1;
constexpr int PEDIK_LOOKAROUND_HEAD   = 2;
constexpr int PEDIK_AIMS_WITH_ARM     = 4;

// Animation. m_animGroup selects the walking *style*. See docs/protocol.md
// §1.8.1 for why it must not be used to look up an arbitrary AnimationId.
constexpr size_t PED_ANIM_GROUP        = 0x1D4;   // AssocGroupId, int32
// m_pVehicleAnim: the association for a get-in/get-out animation, null unless
// one is playing. Gets a second confirmation from
// WARP_CHAR_FROM_CAR_TO_COORD, which tests it and, when set, writes -1000.0f
// into that association's blendDelta (ANIM_BLEND_DELTA) to tear it down in a
// single frame.
constexpr size_t PED_VEHICLE_ANIM      = 0x1D8;   // CAnimBlendAssociation*
constexpr size_t PED_ANIM_MOVE_DELTA   = 0x1DC;   // CVector2D
constexpr size_t PED_FRAMES            = 0x1A4;   // AnimBlendFrameData*[]
constexpr size_t PED_STATS             = 0x330;   // CPedStats*
constexpr size_t PED_HEADING_RATE      = 0x2E4;

// Weapons. Proved four separate times - matters here because a wrong stride
// indexes straight out of the object:
//
//   CPed::CPed              array-constructs 13 elements of 0x18 at +0x35C
//   CPed::GiveWeapon        `lea ebx,[type*8] / lea ebx,[ebx+ebx*2]` = type*0x18,
//                           then `cmp [esi+ebx+35Ch],ebp` - HasWeapon(type) is
//                           `m_weapons[type].m_eWeaponType == type`
//   CPed::GetWeaponSlot     the same index arithmetic, independently
//   CPed::SetCurrentWeapon  `movsx edx,byte [ebp+498h] / lea edx,[edx+edx*2]
//                           / mov eax,[ebp+edx*8+35Ch]` - GetWeapon()
//
// m_currentWeapon is a byte, and it's the index into m_weapons - so the
// engine's own eWeaponType doubles as the slot number. SetCurrentWeapon
// writes it with `mov byte [ebp+498h],bl` between the two weapon-model calls,
// right where re3 assigns it. GiveWeapon's `inc byte [esi+499h]` is the
// m_maxWeaponTypeAllowed right after it.
constexpr size_t PED_WEAPONS           = 0x35C;   // CWeapon[13]
constexpr size_t PED_STORED_WEAPON     = 0x494;   // eWeaponType
constexpr size_t PED_CURRENT_WEAPON    = 0x498;   // uint8, index into m_weapons
constexpr size_t PED_MAX_WEAPON_TYPE   = 0x499;   // uint8
constexpr size_t SIZEOF_WEAPON         = 0x18;
constexpr size_t NUM_WEAPON_SLOTS      = 13;      // WEAPONTYPE_TOTAL_INVENTORY_WEAPONS

// CWeapon members, from GiveWeapon: `add [esi+ebx+368h],eax` is
// `m_nAmmoTotal += ammo` (0x368-0x35C = 0x0C), and
// `cmp dword [esi+ebx+360h],3` / `mov dword [...],0` is
// `if (m_eWeaponState == WEAPONSTATE_OUT_OF_AMMO) = WEAPONSTATE_READY`.
constexpr size_t WEAPON_TYPE           = 0x00;
constexpr size_t WEAPON_STATE          = 0x04;
constexpr size_t WEAPON_AMMO_IN_CLIP   = 0x08;
constexpr size_t WEAPON_AMMO_TOTAL     = 0x0C;
constexpr size_t WEAPON_TIMER          = 0x10;

// Aim. CPed::SetAimFlag (0x004C6960) and CPed::ClearAimFlag (0x004C6A50)
// mirror each other and between them witness every field here:
//
//   SetAimFlag:   [154h] |= 80h        bIsAimingGun = true
//                 [155h] &= ~1         bIsRestoringGun = false
//                 fstp [4BCh]          m_fLookDirection = angle
//                 [4CCh] = 0           m_lookTimer = 0
//                 [4B8h] = 0           m_pLookTarget = nil
//                 [30Ch] = 0           m_pSeekTarget = nil
//   ClearAimFlag: [154h] &= ~80h, [155h] |= 1, [214h] &= ~AIMS_WITH_ARM
//
// m_pSeekTarget at 0x30C also cross-confirms m_pMyVehicle at 0x310, already
// in this file from a different function.
constexpr size_t PED_FLAGS_A           = 0x154;   // bIsAimingGun    = bit 7
constexpr size_t PED_FLAGS_B           = 0x155;   // bIsRestoringGun = bit 0
constexpr size_t PED_SEEK_TARGET       = 0x30C;   // CEntity*
constexpr size_t PED_POINT_GUN_AT      = 0x49C;   // CEntity*
constexpr size_t PED_LOOK_TARGET       = 0x4B8;   // CEntity*
constexpr size_t PED_LOOK_DIRECTION    = 0x4BC;   // float, world yaw
constexpr size_t PED_WEP_MODEL_ID      = 0x4C0;   // int32, -1 = none
constexpr size_t PED_LOOK_TIMER        = 0x4CC;

constexpr uint8_t PED_IS_AIMING_GUN    = 0x80;   // byte A, bit 7
constexpr uint8_t PED_IS_RESTORING_GUN = 0x01;   // byte B, bit 0

// bIsShooting, byte C bit 6. Witnessed directly rather than counted out of
// re3's declaration order, which matters now that CoopIII writes it every
// frame instead of only reading it. CWeapon::Fire's join point, once a shot
// has actually gone off (0x0055C787 onward):
//
//     mov  al,[ebp+50h] / and al,7 / cmp al,3      shooter->IsPed()
//     mov  al,[ebp+156h] / and al,0BFh / or al,40h / mov [ebp+156h],al
//                                                  bIsShooting = true
//     call 0x004D48E0                              CPed::IsPlayer
//     mov  eax,[ebp+64h] / push 2Fh / call ...     DMAudio.PlayOneShot(
//                                                    m_audioEntityId,
//                                                    SOUND_WEAPON_SHOT_FIRED)
//     cmp  dword [edi+8],0 / jle / dec [edi+8]     if (m_nAmmoInClip > 0)--
//
// That matches re3 Weapon.cpp:251-268 statement for statement. It also gives
// ENTITY_TYPE_PED == 3 and CPed::m_audioEntityId at +0x64 for free, and the
// call to CPed::IsPlayer (an address already in this file from
// CPed::ClearAimFlag) is the cross-check that this really is that code.
constexpr uint8_t PED_IS_SHOOTING      = 0x40;   // byte C, bit 6
constexpr size_t  PED_AUDIO_ENTITY_ID  = 0x64;   // CPhysical::m_audioEntityId
constexpr uint8_t ENTITY_TYPE_PED      = 3;      // bits 0-2 of ENTITY_FLAGS

static_assert(PED_IK_FLAGS == PED_IK + 0x24 && PED_IK + SIZEOF_PEDIK == 0x218,
              "CPedIK is m_ped, four LimbOrientations, m_flags, then m_actionX");
static_assert(PED_IK_TORSO_PITCH == PED_IK_TORSO_YAW + 4,
              "LimbOrientation is {yaw, pitch} (re3 PedIK.h:5-9)");
static_assert(PED_IK_TORSO_YAW == PED_IK_HEAD_YAW + 8 &&
                  PED_IK_UPPERARM_YAW == PED_IK_TORSO_YAW + 8 &&
                  PED_IK_LOWERARM_YAW == PED_IK_UPPERARM_YAW + 8,
              "head, torso, upper arm, lower arm on 8-byte strides");
static_assert(PED_ANIM_MOVE_DELTA == PED_VEHICLE_ANIM + 4 &&
                  PED_VEHICLE_ANIM == PED_ANIM_GROUP + 4,
              "m_animGroup, m_pVehicleAnim, m_vecAnimMoveDelta (re3 Ped.h:411-414)");
static_assert(PED_IK == PED_ANIM_MOVE_DELTA + 8 + 12,
              "m_pedIK follows m_vecAnimMoveDelta (8) and m_vecOffsetSeek (12)");
static_assert(PED_CURRENT_WEAPON == PED_WEAPONS + NUM_WEAPON_SLOTS * SIZEOF_WEAPON + 4,
              "m_currentWeapon follows m_weapons[13] and m_storedWeapon");
static_assert(PED_MAX_WEAPON_TYPE == PED_CURRENT_WEAPON + 1,
              "m_maxWeaponTypeAllowed is the byte after m_currentWeapon");
static_assert(WEAPON_TIMER + 4 < SIZEOF_WEAPON,
              "every CWeapon member lies inside its 0x18-byte slot");
static_assert(PED_FLAGS_A == PED_FLAGS && PED_FLAGS_B == PED_FLAGS + 1,
              "bytes A and B of the same CPed bitfield block as C and G");
static_assert(PED_LOOK_DIRECTION == PED_LOOK_TARGET + 4 &&
                  PED_WEP_MODEL_ID == PED_LOOK_DIRECTION + 4,
              "m_pLookTarget, m_fLookDirection, m_wepModelID (re3 Ped.h:483-485)");
static_assert(PED_SEEK_TARGET + 4 == PED_MY_VEHICLE,
              "m_pMyVehicle follows m_pSeekTarget - a cross-check on both");
static_assert(PED_LOOK_TIMER < SIZEOF_PED && PED_WEP_MODEL_ID < SIZEOF_PED,
              "every field above lies inside a CPed");

// Structural relationships straight from re3's declarations. Cheap to keep,
// and they catch the failure mode that matters most: a transposed digit in an
// offset, which would otherwise write a float into the neighbouring member
// and look like a netcode bug.
static_assert(POSITION == MATRIX + 0x30,
              "CMatrix::GetPosition is the RwMatrix pos vector at +0x30");
static_assert(MATRIX_FWD == MATRIX_RIGHT + 0x10 && MATRIX_UP == MATRIX_FWD + 0x10,
              "RwMatrix axes are 12-byte vectors on 16-byte strides");
static_assert(TURN_SPEED == MOVE_SPEED + 0x0C,
              "m_vecTurnSpeed follows m_vecMoveSpeed (re3 Physical.h:24-25)");
static_assert(PED_LAST_STATE == PED_STATE + 4,
              "m_nLastPedState follows m_nPedState (re3 Ped.h:421-422)");
static_assert(PED_ARMOUR == PED_HEALTH + 4,
              "m_fArmour follows m_fHealth (re3 Ped.h:436-437)");
static_assert(PED_ROT_DEST == PED_ROT_CUR + 4,
              "m_fRotationDest follows m_fRotationCur (re3 Ped.h:444-445)");
static_assert(PED_IN_VEHICLE == PED_MY_VEHICLE + 4,
              "bInVehicle follows m_pMyVehicle (re3 Ped.h:454-455)");
static_assert(PED_ROT_CUR < SIZEOF_PED && PED_IN_VEHICLE < SIZEOF_PED,
              "every ped field lies inside the object");
static_assert(SIZEOF_PLAYER_PED > SIZEOF_PED,
              "CPlayerPed extends CPed, and the pool is strided for the larger");
static_assert(MATRIX_ATTACHMENT == MATRIX + 0x40 && MATRIX_HAS_RW_MATRIX == MATRIX + 0x44,
              "CMatrix is a 0x40-byte RwMatrix, then m_attachment, then m_hasRwMatrix");
static_assert(RW_OBJECT == MATRIX_HAS_RW_MATRIX + 4,
              "CEntity::m_rwObject is the first member after CPlaceable");
static_assert(PED_CHAR_CREATED_BY == PED_FLAGS + 0x0C,
              "CharCreatedBy follows three dwords of CPed bitfields (re3 Ped.h)");
static_assert(PED_FLAGS_C == PED_FLAGS + 2 && PED_FLAGS_G == PED_FLAGS + 6,
              "flag bytes C and G of the CPed bitfield block");
static_assert(ZONE_LEVEL < PED_FLAGS,
              "m_nZoneLevel is a CPhysical member, so it precedes CPed's own");

// ---- CVehicle -------------------------------------------------------------
//
// Verified 2026-09-21 (Area E) the same way the ped layout was - by finding
// functions whose re3 source is known and matching them instruction for
// instruction. Nothing here is arithmetic off re3's declarations alone.
// Every offset below is written or read by a named function in the retail
// image, and most get confirmed by three or more independent ones.
//
// The four functions that carry most of the proof:
//
//   CVehicle::CVehicle (0x00550A60) runs re3 Vehicle.cpp:50-125 statement for
//   statement, including the inlined CAutoPilot constructor. It writes
//   m_nCurrentGear=1, m_fChangeGearTime=0, m_fHealth=1000.0f,
//   m_nNumMaxPassengers=8, m_nDoorLock=CARLOCK_UNLOCKED, m_bSirenOrAlarm=0,
//   nils pDriver and all eight pPassengers, and constructs m_aCollPolys as
//   2 x 0x28 bytes at +0x230.
//
//   COMMAND_CREATE_CAR's handler (0x0043C476) matches re3 Script.cpp:3420-3466
//   statement for statement, on both the boat and the car branch.
//
//   CPools::LoadVehiclePool (the field-copy run at 0x004A1D3C-0x004A1F01)
//   matches re3 Pools.cpp:175-208 field by field, in declaration order, and it
//   even reproduces re3's noted copy-paste bug, copying the same 16-bit field
//   at +0x216 twice. That's what identifies the function beyond doubt.
//
//   CRecordDataForChase::StoreInfoForCar (0x00435000, see the note in
//   docs/addresses-unverified.md, the address recorded for it was wrong)
//   matches re3 Record.cpp:331-348 and packs the controls into a byte each.

constexpr size_t VEH_HANDLING           = 0x128;   // tHandlingData*
constexpr size_t VEH_AUTOPILOT          = 0x12C;   // CAutoPilot, 0x70 bytes
constexpr size_t VEH_COLOUR1            = 0x19C;   // uint8
constexpr size_t VEH_COLOUR2            = 0x19D;   // uint8
// int8[2]. Written in exactly one place in the engine, CVehicle::SetModelIndex
// at 0x00551185/0x00551190, from CVehicleModelInfo::ms_compsUsed. It is a
// record of a choice already made and writing it changes nothing on screen -
// see "a vehicle's extra components" below, which is why.
constexpr size_t VEH_EXTRAS             = 0x19E;
constexpr size_t VEH_ALARM_STATE        = 0x1A0;   // int16
constexpr size_t VEH_DRIVER             = 0x1A4;   // CPed*
constexpr size_t VEH_PASSENGERS         = 0x1A8;   // CPed*[8]
constexpr size_t VEH_NUM_PASSENGERS     = 0x1C8;   // uint8
constexpr size_t VEH_NUM_GETTING_IN     = 0x1C9;   // int8
constexpr size_t VEH_GETTING_OUT_FLAGS  = 0x1CB;   // int8
constexpr size_t VEH_NUM_MAX_PASSENGERS = 0x1CC;   // uint8
constexpr size_t VEH_STEER_ANGLE        = 0x1E8;   // float, radians
constexpr size_t VEH_GAS_PEDAL          = 0x1EC;   // float, -1..1
constexpr size_t VEH_BRAKE_PEDAL        = 0x1F0;   // float, 0..1
constexpr size_t VEH_CREATED_BY         = 0x1F4;   // uint8, see below
constexpr size_t VEH_FLAGS_A            = 0x1F5;   // first bitfield byte
constexpr size_t VEH_FLAGS_C            = 0x1F7;   // third bitfield byte
constexpr size_t VEH_HEALTH             = 0x200;   // float, 1000 = full
constexpr size_t VEH_CURRENT_GEAR       = 0x204;   // uint8
constexpr size_t VEH_CHANGE_GEAR_TIME   = 0x208;   // float
constexpr size_t VEH_DOOR_LOCK          = 0x224;   // eCarLock, 4 bytes
constexpr size_t VEH_HORN_TIMER         = 0x22C;   // uint8, see "the horn" below
constexpr size_t VEH_HORN_PATTERN       = 0x22D;   // uint8
constexpr size_t VEH_SIREN_OR_ALARM     = 0x22E;   // bool
constexpr size_t VEH_COLL_POLYS         = 0x230;   // CStoredCollPoly[2]
constexpr size_t VEH_STEER_INPUT        = 0x280;   // float
constexpr size_t VEH_TYPE               = 0x284;   // eVehicleType, 4 bytes

constexpr size_t SIZEOF_VEHICLE           = 0x288;
constexpr size_t SIZEOF_AUTOMOBILE        = 0x5A8;   // vehicle pool stride
constexpr size_t SIZEOF_BOAT              = 0x484;
constexpr size_t SIZEOF_AUTOPILOT         = 0x70;
constexpr size_t SIZEOF_STORED_COLL_POLY  = 0x28;
constexpr size_t VEH_MAX_PASSENGERS       = 8;

// CAutoPilot members CoopIII needs, as absolute CVehicle offsets. Each one is
// written by CREATE_CAR's car branch at exactly the place re3 writes it
// (Script.cpp:3451-3456), and again by the inlined CAutoPilot constructor.
constexpr size_t AUTOPILOT_CURRENT_LANE      = 0x157;   // int8
constexpr size_t AUTOPILOT_NEXT_LANE         = 0x158;   // int8
constexpr size_t AUTOPILOT_DRIVING_STYLE     = 0x159;   // uint8
constexpr size_t AUTOPILOT_CAR_MISSION       = 0x15A;   // uint8
constexpr size_t AUTOPILOT_TEMP_ACTION       = 0x15B;   // uint8
constexpr size_t AUTOPILOT_MAX_TRAFFIC_SPEED = 0x160;   // float
constexpr size_t AUTOPILOT_CRUISE_SPEED      = 0x164;   // uint8

// The first CVehicle bitfield byte, in re3's declaration order. Proved four
// times over - more than it sounds, since three of the four pin a different
// bit of the same byte, so the bit order is verified rather than assumed.
//
//   CVehicle::CVehicle clears bits 3,0,1,2 then sets bit 4 (bEngineOn = true)
//   and bit 7 (bFreebies = true), in re3's exact statement order.
//   CREATE_CAR sets bit 3 (bIsLocked = true) and clears bit 4 (bEngineOn).
//   StoreInfoForCar reads the handbrake as `shr dl,5 / and dl,1` - bit 5.
//   LoadVehiclePool copies bits 0,3,4,5,6,7 in re3's declaration order.
constexpr uint8_t VEH_IS_LAW_ENFORCER      = 0x01;
constexpr uint8_t VEH_IS_AMBULANCE_ON_DUTY = 0x02;
constexpr uint8_t VEH_IS_FIRETRUCK_ON_DUTY = 0x04;
constexpr uint8_t VEH_IS_LOCKED            = 0x08;
constexpr uint8_t VEH_ENGINE_ON            = 0x10;
constexpr uint8_t VEH_HANDBRAKE_ON         = 0x20;
constexpr uint8_t VEH_LIGHTS_ON            = 0x40;
constexpr uint8_t VEH_FREEBIES             = 0x80;

// Bit 2 of the third bitfield byte. CREATE_CAR does `and al,0FBh / or al,4` on
// [+0x1F7] where re3 has `car->bHasBeenOwnedByPlayer = true`, and the
// constructor clears the same bit where re3 has it false.
constexpr uint8_t VEH_HAS_BEEN_OWNED_BY_PLAYER = 0x04;

static_assert(VEH_AUTOPILOT == VEH_HANDLING + 4,
              "CAutoPilot follows tHandlingData* (re3 Vehicle.h:112-113)");
static_assert(VEH_COLOUR1 == VEH_AUTOPILOT + SIZEOF_AUTOPILOT,
              "m_currentColour1 is the first member after CAutoPilot");
static_assert(VEH_COLOUR2 == VEH_COLOUR1 + 1 && VEH_EXTRAS == VEH_COLOUR2 + 1,
              "colour1, colour2, m_aExtras[2] are consecutive bytes");
static_assert(VEH_PASSENGERS == VEH_DRIVER + 4,
              "pPassengers follows pDriver (re3 Vehicle.h:119-120)");
static_assert(VEH_NUM_PASSENGERS == VEH_PASSENGERS + VEH_MAX_PASSENGERS * 4,
              "m_nNumPassengers follows the eight passenger pointers");
static_assert(VEH_GAS_PEDAL == VEH_STEER_ANGLE + 4 &&
                  VEH_BRAKE_PEDAL == VEH_GAS_PEDAL + 4,
              "steer, gas, brake are three consecutive floats");
static_assert(VEH_FLAGS_A == VEH_CREATED_BY + 1,
              "the bitfields begin in the byte after VehicleCreatedBy");
static_assert(VEH_FLAGS_C == VEH_FLAGS_A + 2,
              "third byte of the CVehicle bitfield block");
static_assert(VEH_CHANGE_GEAR_TIME == VEH_CURRENT_GEAR + 4,
              "m_fChangeGearTime is the next aligned float after m_nCurrentGear");
static_assert(VEH_STEER_INPUT == VEH_COLL_POLYS +
                                     2 * SIZEOF_STORED_COLL_POLY,
              "m_fSteerInput follows m_aCollPolys[2] (2 x 0x28 at +0x230)");
static_assert(VEH_TYPE == VEH_STEER_INPUT + 4 &&
                  VEH_TYPE + 4 == SIZEOF_VEHICLE,
              "m_vehType is the final CVehicle member");
static_assert(VEH_TYPE < SIZEOF_VEHICLE && VEH_HANDLING > ZONE_LEVEL,
              "every CVehicle field lies inside the object, after CPhysical's");
static_assert(SIZEOF_AUTOMOBILE > SIZEOF_VEHICLE && SIZEOF_BOAT > SIZEOF_VEHICLE,
              "CAutomobile and CBoat extend CVehicle");
static_assert(SIZEOF_AUTOMOBILE > SIZEOF_BOAT,
              "the pool is strided for the largest subclass, which is CAutomobile");
static_assert(AUTOPILOT_CRUISE_SPEED - VEH_AUTOPILOT == 0x38 &&
                  AUTOPILOT_MAX_TRAFFIC_SPEED - VEH_AUTOPILOT == 0x34,
              "m_nCruiseSpeed and m_fMaxTrafficSpeed inside CAutoPilot");
static_assert(AUTOPILOT_CRUISE_SPEED < VEH_AUTOPILOT + SIZEOF_AUTOPILOT,
              "every CAutoPilot field lies inside the 0x70-byte sub-object");
static_assert((VEH_ENGINE_ON & VEH_IS_LOCKED) == 0 &&
                  (VEH_ENGINE_ON | VEH_HANDBRAKE_ON) == 0x30,
              "bEngineOn is bit 4, between bIsLocked (3) and bIsHandbrakeOn (5)");

// ---- the horn ---------------------------------------------------------------
//
// m_nCarHornTimer (+0x22C) and m_nCarHornPattern (+0x22D), the two bytes in
// front of m_bSirenOrAlarm. Every byte-sized access to either offset in the
// image was listed with a scan and read; there are twenty-seven and these are
// all of them that matter:
//
//   CVehicle::CVehicle      0x00550EDA / 0x00550EE5   both = 0
//   CAutomobile::ProcessControl, status switch on [+50h]>>3, table 0x00600A24:
//     entries 4 (ABANDONED), 5 (WRECKED) and 11 (PLAYER_DISABLED) are
//     0x00531B54 / 0x00531B7E / 0x00531B68, which set the brake to 0.2 / 0.05
//     / 1.0 and all fall into `mov byte [ebp+22Ch],0` at 0x00531BAC.
//   the horn block, 0x00533FF2: status 0 (PLAYER) only.
//     Mr Whoopee (model 0x71) toggles the siren byte off the pad's
//     bHornHistory (0x006F0439, index 0x006F043E) and leaves the timer alone.
//     A siren car (call 0x00552200) writes 1 only after three frames of held
//     key (0x00534169), otherwise 0, and toggles the siren on a tap.
//     Anything else except the Yardie Lobo (0x87) and with bCheat3
//     (0x0095CD66) clear writes 1 or 0 straight from CPad::GetHorn
//     (0x00493350, ecx = Pads[0] 0x006F0360) at 0x0053419F / 0x005341A8.
//     Every other status calls ReduceHornCounter (0x005308C0: `cmp byte
//     [ecx+22Ch],0 / je / dec byte [ecx+22Ch] / ret`) at 0x005341B5.
//   CAutomobile::PlayCarHorn 0x0053C450, CAutomobile vtable slot 34 (the only
//     pointer to it in the image is 0x00600CA4 = 0x00600C1C + 0x88): does
//     nothing if the timer is running, else draws rand() & 7 and sets 45
//     (0x2D) on 0-1 (0x0053C46C) and on 2-3 (0x0053C48B, after the driver's
//     Say), so four draws in eight. The traffic AI's honk, from
//     PlayHornIfNecessary (0x0053C4B0). CCarAI::UpdateCarAI (0x00413E50, called
//     only from the SIMPLE and PHYSICS arms, 0x00531A68 / 0x00531B3E) also
//     writes 45 for a siren car on a random byte match (0x0041584D) and 0 in
//     three places.
//   cAudioManager::ProcessVehicleHorn 0x0056C200, reached from
//     ProcessVehicle at 0x00569C23 for any car in road-noise range, with no
//     status test on the way. Skipped for Mr Whoopee and for a siren-switching
//     car with its siren on. For status PLAYER any non-zero timer sounds; for
//     every other status it clamps the timer to 44 (0x2C), re-picks the
//     pattern when the timer reads exactly 44, and sounds only where
//     `byte [0x00606AB8 + pattern*44 + (44 - timer)]` is set. That table is
//     in game/horn.h, and it is why a replica cannot be given the player's 1.
//   cAudioManager::ProcessVehicleSirenOrAlarm 0x0056C420: for a siren model
//     (0x0056C3C0) it returns at 0x0056C4C7 when the status is 4, ABANDONED,
//     before anything reads the timer or queues the siren. game/siren.h has
//     the rest and the detour that gets a replica past it.
//   CCarCtrl::SlowCarDownForPedsSectorList 0x00419300, called from
//     ScanForPedDanger 0x00418F40, which CAutomobile::ProcessControl calls at
//     0x00534B14 if !bWarnedPeds - AFTER the status switch and the horn block.
//     The horn read at 0x0041974E sits behind `status == 0` (0x0041967E),
//     `pedState != 9` and `CharCreatedBy == 1` (0x0041968F): a player's car,
//     a random ped. It is what makes pedestrians flee (SetFlee 2000 ms, then
//     run) when the player honks at them.
//   CPed::SetEvasiveStep 0x004D30C0 reads it at 0x004D31CA, and
//     CPed::SetEvasiveDive 0x004D33A0 at 0x004D33EC. Both only after their
//     early returns, which for the scan's animType 0 include
//     !bRespondsToThreats (byte [+156h] bit 1) in both.
static_assert(VEH_HORN_PATTERN == VEH_HORN_TIMER + 1 &&
                  VEH_SIREN_OR_ALARM == VEH_HORN_PATTERN + 1,
              "m_nCarHornTimer, m_nCarHornPattern, m_bSirenOrAlarm are three "
              "consecutive bytes (re3 Vehicle.h:188-190)");

// cVehicleParams, the audio's per-vehicle argument, built on ProcessVehicle's
// stack: `mov [esp+8],ebx` at 0x00569A5C with ebx the CVehicle, and
// ProcessVehicleSirenOrAlarm reads it back with `mov ebp,[ebx+8]` at
// 0x0056C443.
constexpr size_t AUDIO_PARAMS_VEHICLE = 0x08;   // CVehicle *

} // namespace offs

// World.cpp - CWorld::Players[CWorld::PlayerInFocus].m_pPed. Confirmed by
// disassembly: movzx PlayerInFocus, imul by the CPlayerInfo stride, indexed
// load, ret. Matches re3's one-liner exactly.
constexpr uintptr_t FindPlayerPed     = 0x004A1150;

// PlayerInfo.cpp:436-442, forty bytes long. Verified 2026-09-21: same
// PlayerInFocus / imul 0x4F / indexed load prologue as above, then
// `cmp byte [ecx+0x314],0` (bInVehicle) and `mov eax,[ecx+0x310]`
// (m_pMyVehicle) - re3's `if(ped && ped->InVehicle()) return ped->m_pMyVehicle`
// with the null check in front, statement for statement. The function right
// after it, FindPlayerTrain (0x004A1120), calls this one and then tests
// `[eax+0x284] == 2`, which is where offs::VEH_TYPE and VEHICLE_TYPE_TRAIN
// were first pinned.
constexpr uintptr_t FindPlayerVehicle = 0x004A10C0;

constexpr uintptr_t CPed__SetStoredState = 0x004C5DB0;
// CPed::SetDie has moved to the damage/death block at the end of the combat
// section, where it is called from and where its proof is written out.

// ---- ped lifecycle --------------------------------------------------------
//
// Found by walking the game's own ped-creation path rather than guessing at
// a pool allocator - guessing is what corrupts a pool silently:
//
//   CRunningScript::ProcessCommands (0x00439500) dispatches `cmp dx,200 /
//   call` into ProcessCommands100To199 (0x0043AEA4), whose jump table at
//   0x005EEA7C is indexed by opcode-100. COMMAND_CREATE_CHAR is opcode 154
//   (re3 ScriptCommands.h), so entry 54 -> 0x0043BA03.
//
//   That handler opens with CollectParameters(&m_nIp, 5) and a switch over
//   model ids 1..6 (re3 Script.cpp:3168-3198 exactly), then allocates and
//   constructs. COMMAND_DELETE_CHAR (opcode 155) is the matching teardown.
//
// So this is the sequence the engine itself runs, not an approximation of it.

// operator new for peds: takes the size, returns pool memory or null.
// sizeof(CCivilianPed) == sizeof(CPed) == 0x53C, which the handler itself
// states with `push 53Ch / call 0x004C5220` in the civilian branch. The
// function is twelve bytes and ignores the size:
//     mov ecx,[0x008F2C60]   ; CPools::ms_pPedPool
//     call 0x004D80D0        ; CPool<CPed,CPlayerPed>::New
//     ret
// and 0x004D80D0 opens `inc dword [ecx+0x0C] / mov ebx,[ecx+0x0C] /
// mov edx,[ecx+8] / cmp ebx,edx` - re3's `if(++m_allocPtr == m_size)`.
constexpr uintptr_t CPed__operator_new = 0x004C5220;

// CCivilianPed::CCivilianPed(ePedType, int32 modelId), __thiscall.
// Resolved with tools/calltarget, not by hand - hand arithmetic put this
// 0x80 too high the first time, landing mid-instruction in the previous
// function's epilogue and crashing the game with c0000005. Use the tool.
constexpr uintptr_t CCivilianPed__ctor = 0x004BFF30;

// CWorld::Add(CEntity*). Without this the ped exists but isn't in the world.
// Its prologue is `mov al,[ebx+0x50] / and al,7`, which independently
// confirms ENTITY_FLAGS and m_type occupying bits 0-2 of it.
constexpr uintptr_t CWorld__Add = 0x004AE930;

// CWorld::RemoveReferencesToDeletedObject(CEntity*), called immediately
// before `delete ped` in DELETE_CHAR (re3 Script.cpp:3243-3244). Its
// prologue reads ms_pPedPool, which is exactly what a sweep like this needs.
constexpr uintptr_t CWorld__RemoveReferencesToDeletedObject = 0x004B3BF0;

// The deleting destructor is vtable slot 0, called with a 1 flag: the handler
// does `mov ecx,edi / mov esi,[ecx] / push 1 / call [esi]` (FF 16).
constexpr size_t VTABLE_DELETING_DTOR = 0;

// CTheZones::GetLevelFromPosition(CVector const*), __cdecl, level in AL.
// From the handler: `lea eax,[esp+0x98] / push eax / call 0x004B6910 /
// mov [ebx+0x124],al` (re3 Script.cpp:3217). Setting m_nZoneLevel isn't
// cosmetic: CPopulation::MoveCarsAndPedsOutOfAbandonedZones only considers
// peds whose m_nZoneLevel is LEVEL_GENERIC, and teleports the ones it can't
// delete to a regeneration point.
constexpr uintptr_t CTheZones__GetLevelFromPosition = 0x004B6910;

// eLevelName. LEVEL_IGNORE is re3 Game.h:4, flagged there as "only used in
// CPhysical's m_nZoneLevel" - it means "never cull me by level". The local
// player ped carries it, so remote players do too.
constexpr int8_t LEVEL_IGNORE     = -1;
constexpr int8_t LEVEL_GENERIC    = 0;
constexpr int8_t LEVEL_INDUSTRIAL = 1;
constexpr int8_t LEVEL_COMMERCIAL = 2;
constexpr int8_t LEVEL_SUBURBAN   = 3;

// CPopulation::ms_nTotalMissionPeds. CREATE_CHAR does `inc dword [0x008F5F70]`
// and DELETE_CHAR does `dec dword [0x008F5F70]` - same global, which is what
// identifies it. Nothing in re3 reads it, but we keep it balanced anyway: an
// unbalanced engine counter is exactly the kind of thing that causes trouble
// somewhere else entirely, later.
constexpr uintptr_t CPopulation__ms_nTotalMissionPeds = 0x008F5F70;

// ---- vtable identities ----------------------------------------------------
//
// These two constants are how CoopIII tells a live remote ped apart from one
// the engine already destroyed - and the reason Area B's "invisible ped" was
// actually a dead ped, not a broken constructor.
//
// MSVC re-stamps the current class's vtable at the top of every destructor,
// so a fully destroyed object ends up holding the vtable of its base-most
// class. For a CCivilianPed, that base is CPlaceable.
//
// CPlaceable's vtable is 0x005F6A28. This is proved, not guessed - the
// constant appears in exactly two instructions in the whole 2.3 MB image:
//
//   0x0049F9A0  CPlaceable::CPlaceable, writes it, zeroes m_attachment
//               (+0x44) and m_hasRwMatrix (+0x48), then CMatrix::SetScale(1.0)
//   0x0049F9E0  CPlaceable::~CPlaceable, writes it, then CMatrix::~CMatrix
//               on +4; it is the scalar destructor called by the deleting
//               destructor at 0x0049FBD0, which is slot 0 of that same vtable
//
// That deleting destructor's `flags & 1` path calls the *global* operator
// delete (0x005A07E0), not CPed::operator delete. So destroying a ped through
// a stale CPlaceable vtable hands a ped-pool pointer to the CRT heap - which
// is the heap-block walk that faulted at 0x005BD347.
constexpr uintptr_t CPlaceable__vtable = 0x005F6A28;

// CCivilianPed's vtable, written by the constructor at 0x004BFF46
// (`mov dword [eax], 0x005F819C`) before it calls SetModelIndex through slot
// 3. Slot 8 is 0x004BFFE0, the function right after the constructor -
// CCivilianPed::ProcessControl - which cross-checks that this is the derived
// class's table and not CPed's.
constexpr uintptr_t CCivilianPed__vtable = 0x005F819C;

// ePedCreatedBy. CREATE_CHAR writes MISSION_CHAR; CPed::CPed leaves
// RANDOM_CHAR, and every engine sweep that deletes a ped gates on that
// difference - CPed::CanBeDeleted() (CPopulation::ManagePopulation,
// MoveCarsAndPedsOutOfAbandonedZones, CWorld::ClearPedsFromArea,
// CWorld::ClearExcitingStuffFromArea) or a direct test
// (CWorld::RemoveFallenPeds). Leave a remote player as RANDOM_CHAR and the
// engine treats them as ambient population, and reaps them.
constexpr int CHAR_CREATED_BY_UNKNOWN = 0;
constexpr int CHAR_CREATED_BY_RANDOM  = 1;
constexpr int CHAR_CREATED_BY_MISSION = 2;

// **Measured 2026-09-22, and it is what makes docs/wanted.md §4.2 true: a
// police ped is RANDOM_CHAR, so game/population.cpp already replicates it.**
//
// Taken from the image rather than from re3's CPopulation::AddPed, because
// "AddPed does not touch CharCreatedBy" is an absence and re3 has been wrong
// about a retail fact six times on this project. An absence is checkable by
// scanning for the instruction that would contradict it, which is what this
// is: every `mov`/`alu byte [reg+160h], imm8` and `mov byte [reg+160h], r8`
// with a disp32 in the whole of .text, 77 of them.
//
// The only *write* CPopulation or CCarCtrl could reach is the constructor's:
//
//   004C42EB  mov byte [eax+160h], 1     inside CPed::CPed (0x004C41C0),
//                                        re3 Ped.cpp:79, and the next field
//                                        it writes is [eax+180h] = 0
//
// Everything else at this offset in those two modules is a *read*:
//
//   004F347B  cmp byte [ecx+160h], 1     CPopulation, "is this ambient?"
//   0041968F  cmp byte [ebx+160h], 1     CCarCtrl, the same question
//
// and every other immediate write in the image is a script opcode handler
// stamping MISSION_CHAR, one of the two handlers that hand a mission ped back
// to the city as RANDOM_CHAR, or the replay system at 0x0058xxxx. There is no
// write anywhere in CPopulation's whole address range.
//
// So a CCopPed leaves CPopulation::AddPed carrying exactly the value an
// ambient civilian leaves it carrying, and nothing between there and
// CWorld::Add distinguishes the two. The live measurement in
// docs/population.md §1.3.1 that says ambient pedestrians replicate therefore
// covers police as well, without a second measurement and without a line of
// code.

// ePedType. COP == 6 is read straight out of the CREATE_CHAR handler's
// `cmp dword [ScriptParams[0]], 6`, which fixes the rest of the enum.
constexpr int PEDTYPE_CIVMALE   = 4;
constexpr int PEDTYPE_CIVFEMALE = 5;
constexpr int PEDTYPE_COP       = 6;

// MI_PLAYER - Claude. Model index 0 (re3 ModelIndices.h), and what a remote
// player wears by default.
//
// Slot 0 is a special model: its geometry loads by name rather than by index.
// Worth writing down that it never changes in GTA III, since the opposite
// looks like the obvious guess. Three things establish it:
//
//   - COMMAND_CREATE_PLAYER is the only place that fills it, with the literal
//     name "player" and STREAMFLAGS_DONT_REMOVE (re3 Script.cpp:2722).
//   - The save loader stores a model name beside the player, and re3's comment
//     on it reads: it could be avoided by just using "player" because in
//     practice it is always true (Pools.cpp:514).
//   - No script command changes a ped's model index at all. LOAD_SPECIAL_MODEL
//     fills the *special character* slots (MI_SPECIAL01..21, 8-Ball, Misty),
//     never slot 0.
//
// So there are no outfits in GTA III - a Claude who looks different in a
// mission is a CCutsceneObject out of cuts.img standing in front of the real
// ped, not the ped wearing something else. CoopIII still doesn't hardcode
// this (the model travels on the wire and a change re-skins the ped), because
// it's a fact about the stock game and not the engine, and a mod might not
// share it.
//
// DONT_REMOVE is also why this model needs no streaming request, and must not
// be given one - it's pinned for the life of the game, and adding our own
// SCRIPTOWNED claim on top would just be one more reason for
// CStreaming::RemoveModel to refuse, if something ever did want to replace it.
constexpr uint16_t MI_PLAYER = 0;

// The fallback for when the player model somehow isn't there - MI_MALE01, an
// ordinary civilian always in the stream. A remote player wearing the wrong
// shirt is a cosmetic bug. A remote player who's invisible is not.
constexpr uint16_t MI_MALE01 = 7;

// ---- vehicle lifecycle ----------------------------------------------------
//
// Walked out of COMMAND_CREATE_CAR the same way the ped path came out of
// COMMAND_CREATE_CHAR: dispatcher 0x00439500 -> ProcessCommands100To199
// (0x0043AEA0) -> jump table 0x005EEA7C entry 65 (CREATE_CAR is opcode 165)
// -> handler 0x0043C476. COMMAND_DELETE_CAR is opcode 166, entry 66, handler
// 0x0043C7D5.
//
// The handler matches re3 Script.cpp:3420-3481 statement for statement. Its
// car branch, in order:
//
//   CollectParameters(&m_nIp, 4)
//   if (CModelInfo::IsBoatModel(model))                  ; call 0x0050BB90
//        ... the CBoat branch. NOT the same shape as the car's, see "boats"
//        below for the whole of it (re-read 2026-09-23) ...
//   if (!IsBikeModel(model))     ; inlined: ms_modelInfoPtrs[model]->+0x58 != 5
//        car = new CAutomobile(model, MISSION_VEHICLE)   ; push 5A8h / new / ctor
//   pos.z += car->GetDistanceFromCentreOfMassToBaseOfModel()   ; 0x004755C0
//   car->SetPosition(pos)                                      ; +0x34/38/3C
//   CTheScripts::ClearSpaceForMissionEntity(pos, car)          ; 0x00454060
//   car->SetStatus(STATUS_ABANDONED)      ; [+0x50] &= 7, |= 0x20
//
//     Corrected 2026-09-21: this line previously read `&= ~7`, which is the
//     opposite of what the handler does and would clear m_type, turning the
//     car into ENTITY_TYPE_NOTHING while leaving the old status. The bytes at
//     0x0043C5xx are `8A 43 50 / 24 07 / 0C 20 / 88 43 50`: mov al,[ebx+50],
//     and al,7 (KEEP m_type, bits 0-2), or al,20h (m_status = 4 in bits 3-7),
//     store. Independently confirmed by the ped diagnostics, where both the
//     local player and a remote ped read 0x23 = type 3 | (4 << 3).
//   car->bIsLocked = true                 ; [+0x1F5] |= 0x08
//   CCarCtrl::JoinCarWithRoadSystem(car)                       ; 0x0041F820
//   car->AutoPilot.m_nCarMission   = MISSION_NONE              ; [+0x15A] = 0
//   car->AutoPilot.m_nTempAction   = TEMPACT_NONE              ; [+0x15B] = 0
//   car->AutoPilot.m_nDrivingStyle = STOP_FOR_CARS             ; [+0x159] = 0
//   car->AutoPilot.m_nCruiseSpeed  = m_fMaxTrafficSpeed = 9.0f ; [+0x160]/[+0x164]
//   car->AutoPilot.m_nCurrentLane  = m_nNextLane = 0           ; [+0x157]/[+0x158]
//   car->bEngineOn = false                ; [+0x1F5] &= ~0x10
//   car->m_nZoneLevel = CTheZones::GetLevelFromPosition(&pos)  ; [+0x124]
//   car->bHasBeenOwnedByPlayer = true     ; [+0x1F7] |= 0x04
//   CWorld::Add(car)                                           ; 0x004AE930
//   handle = CPools::GetVehiclePool()->GetIndex(car)           ; 0x00429050
//
// ############################################################################
// # READ THIS BEFORE WRITING THE SPAWN. Area B's "invisible ped" turned out  #
// # to be the engine deleting an entity CoopIII had constructed but never    #
// # registered. Vehicles have TWO registration gates, not one, and every     #
// # reaping site in the engine tests both:                                   #
// #                                                                          #
// #   !vehicle->bIsLocked && vehicle->CanBeDeleted()                         #
// #                                                                          #
// # CWorld::ClearCarsFromArea, CWorld::ClearExcitingStuffFromArea,           #
// # CCarCtrl::RemoveDistantCars, CPopulation::ManagePopulation and           #
// # CGarages all spell it exactly that way (re3 World.cpp:129/1368,          #
// # CarCtrl.cpp:720-765, Population.cpp:880, Garages.cpp:1388).              #
// #                                                                          #
// # VehicleCreatedBy is the ctor's second argument, so passing               #
// # VEHICLE_CREATED_BY_MISSION covers the CanBeDeleted half. bIsLocked does  #
// # NOT come along with it - it's a separate write the handler makes         #
// # afterwards, and skipping it leaves the other half of every one of those  #
// # tests wide open.                                                        #
// ############################################################################
//
// CVehicle::CanBeDeleted (0x005511B0) is the proof of the second half. It
// opens `cmp byte [ecx+0x1C9],0` then `cmp byte [ecx+0x1CB],0`
// (m_nNumGettingIn || m_nGettingOutFlags), walks pDriver and the eight
// pPassengers testing CharCreatedBy == MISSION_CHAR and PedState against
// PED_DRIVING/PED_DEAD, then switches on VehicleCreatedBy through a
// five-entry table at 0x006026A4 whose MISSION_VEHICLE and PERMANENT_VEHICLE
// slots both land on `xor al,al` - return false. Matches re3 Vehicle.cpp's
// CanBeDeleted exactly, and along the way re-confirms PED_CHAR_CREATED_BY
// (0x160) and PED_STATE (0x224) from the ped section above.

// CVehicle::operator new(size_t), twelve bytes, shaped exactly like
// CPed::operator new: `mov ecx,[0x009430DC] / call CPool::New / ret`. Ignores
// the size argument - the pool strides every slot at SIZEOF_AUTOMOBILE.
constexpr uintptr_t CVehicle__operator_new = 0x00551120;

// __thiscall CAutomobile::CAutomobile(int32 modelId, uint8 createdBy).
// Argument order read off the handler's `push 2 / push modelId / call`, not
// assumed. Its prologue calls CVehicle::CVehicle, stamps the CAutomobile
// vtable, then constructs CDamageManager at this+0x288 - which is the proof
// of offs::SIZEOF_VEHICLE.
constexpr uintptr_t CAutomobile__ctor = 0x0052C6B0;
constexpr uintptr_t CBoat__ctor       = 0x0053E3E0;   // same signature, see "boats"
constexpr uintptr_t CVehicle__ctor    = 0x00550A60;   // (uint8 createdBy)

// CVehicle::SetModelIndex(uint32). Calls CEntity::SetModelIndex, copies
// CVehicleModelInfo::ms_compsUsed into m_aExtras, then sets
// m_nNumMaxPassengers from the model's door count, matching re3 Vehicle.cpp
// exactly. The subclass constructors call it, so a spawn doesn't need it
// directly - it's here as the fifth independent confirmation of +0x1CC.
constexpr uintptr_t CVehicle__SetModelIndex = 0x00551170;

constexpr uintptr_t CVehicle__CanBeDeleted = 0x005511B0;

// ---- boats ----------------------------------------------------------------
//
// Read out of the retail image on 2026-09-23 for docs/roadmap.md M2, "Boats".
// A synced boat used to be built as a CAutomobile wearing a boat's model.
// Everything below is what it takes to build the real thing and what the
// real thing does differently once it exists.
//
// **How the engine tells a boat from a car: by the model info, before there
// is an object, and by m_vehType after.** CModelInfo::IsBoatModel is nine
// instructions and has no null check (the listing desyncs on the padding in
// front of it, so this was decoded from the bytes):
//
//   0050BB90  mov ecx,[esp+4]                    model id
//   0050BB94  mov edx,[ecx*4+0083D408h]          ms_modelInfoPtrs[id]
//   0050BB9D  cmp byte [eax+2Ah],5 / jne false   m_type == MITYPE_VEHICLE
//   0050BBA3  cmp dword [edx+58h],1 / jne false  m_vehicleType == BOAT
//   0050BBA9  mov al,1 / ret
//
// CREATE_CAR's bike test reads the same dword (`mov esi,[eax+58h] / cmp
// esi,5` at 0x0043C5ED), and CCarGenerator::DoInternalProcessing calls
// IsBoatModel too (0x00542739), so it is the engine's one rule, not a script
// quirk. Once built, CBoat::CBoat writes `mov dword [eax+284h],1` at 0x0053E42A
// and CAutomobile::CAutomobile writes 0 there at 0x0052C766, which is what
// every runtime IsBoat() in the engine tests (0x004D8519, 0x004D7F43,
// 0x004E0C3B, 0x004E022B below).
constexpr uintptr_t CModelInfo__IsBoatModel = 0x0050BB90;   // recorded, not called
constexpr uint8_t   MITYPE_VEHICLE          = 5;
namespace offs {
constexpr size_t MODELINFO_VEHICLE_TYPE = 0x58;   // int32 eVehicleType
} // namespace offs

// CBoat::CBoat(int32 modelId, uint8 createdBy), __thiscall, `ret 8` at
// 0x0053E78D. `mov eax,[esp+1Ch] / push eax / call 0x00550A60` passes
// createdBy on to CVehicle::CVehicle and keeps the model in ebp for the
// SetModelIndex through vtable slot 3 at 0x0053E481. The vtable it stamps at
// 0x0053E3FC is 0x00600EA4; slot 29 of it is CBoat::BlowUpCar and slot 0 is
// the deleting destructor 0x005425E0, which runs ~CBoat (0x0053E790) and then
// `call 0x00551150` - CVehicle::operator delete, i.e. `mov ecx,[009430DCh] /
// call 0x00554CA0`, the vehicle pool's Delete. So DELETE_CAR's teardown
// through slot 0 frees a boat into the same pool a car goes back to.
constexpr uintptr_t CBoat__vtable = 0x00600EA4;

// **The pool holds either.** CVehicle::operator new (0x00551120) ignores its
// size - `mov ecx,[009430DCh] / call 0x00554CF0 / ret`, decoded from the
// bytes - and CPool::New at 0x00554CF0 hands back `index * 5A8h + base`
// (0x00554D4E). Every slot is sizeof(CAutomobile), and a 0x484-byte CBoat
// sits in one with 0x124 bytes to spare. CREATE_CAR still pushes 484h for a
// boat and CoopIII does the same.
//
// CREATE_CAR's boat branch, 0x0043C497-0x0043C5DB, which is shorter than the
// car branch in ways that matter:
//
//   push 484h / call CVehicle::operator new
//   push 2 / push model / call CBoat::CBoat              ; MISSION_VEHICLE
//   pos.z += GetDistanceFromCentreOfMassToBaseOfModel()  ; 0x004755C0
//   SetPosition, ClearSpaceForMissionEntity              ; 0x00454060
//   [+50h] and 7 / or 20h                                ; STATUS_ABANDONED
//   [+1F5h] and 0F7h / or 8                              ; bIsLocked
//   [+15Ah] = 0, [+15Bh] = 0                             ; mission, temp action
//   [+160h] = 41A00000h (20.0f), [+164h] = 20            ; max and cruise speed
//   CWorld::Add
//
// No CCarCtrl::JoinCarWithRoadSystem (a boat has no road to join), no
// driving style or lanes, no bEngineOn write, no m_nZoneLevel and no
// bHasBeenOwnedByPlayer. The two gates are the car's two gates.
constexpr float BOAT_SPAWN_CRUISE_SPEED = 20.0f;

// Every `call 0x0053E3E0` in the image, which is also the list of ways a boat
// can come to exist:
//
//   0043C4B2  CREATE_CAR, createdBy 2
//   004A1C13  the save loader, `push 484h` then placement new (0x00551130)
//             and createdBy 1, then `dec [00943118h]` (NumRandomCars) - the
//             real createdBy is copied in from the save afterwards
//   00542760  CCarGenerator::DoInternalProcessing, after IsBoatModel,
//             createdBy 3 (PARKED_VEHICLE), then bIsStatic and bEngineOn off
//   00595460  the replay, which picks the class by model id: 0x78, 0x8E,
//             0x8F, 0x96 (Predator, Speeder, Reefer, Ghost)
//
// None of them is CCarCtrl, so GTA III has no boat traffic, and a boat in
// the street is a parked one from a car generator the script switched on.

// **What a CBoat does with its status, and why a replica's controls are not
// its own.** CBoat::ProcessControl (vtable slot 8, 0x0053EF10) switches on
// the status through the six-entry table at 0x00600E84:
//
//   0 PLAYER     0x0053F2F0  ProcessControlInputs(0) through slot 18
//   2 SIMPLE     0x0053F323  CPhysical::ProcessControl only, then return
//   3 PHYSICS    0x0053F376  CCarCtrl::SteerAIBoatWithPhysics (0x0041E250)
//   4,5 ABANDONED, WRECKED   0x0053F393  steer = 0, bIsHandbrakeOn = 0,
//                            brake = 0.5f, gas = 0; and if the player is more
//                            than 150 units away (0x00600D10), both velocity
//                            vectors zeroed and return
//
// ProcessControlInputs (0x0053EC70) keeps its own accelerate, brake and steer
// at +0x2D8, +0x2DC and +0x2E0 and ends by publishing the two that mean
// anything to the rest of the engine: `fstp [ebp+1E8h]` (m_fSteerAngle) and
// `fstp [ebp+1ECh]` (m_fGasPedal = accelerate) at 0x0053EEF2 / 0x0053EEFE.
// A boat has no gear and no handbrake of its own, and C_VehicleState's steer
// and gas already carry the two fields it publishes.
//
// **A boat has its own fire timer, and nothing but the constructor resets
// it.** The damage block the status switch falls into opens at
// `m_fHealth <= 600.0f` (0x0053F627, constant 0x00600D28) and the fire at
// `m_fHealth < 150.0f` (0x0053F917). Then:
//
//   0053FB04  fadd [ebp+2CCh] / fstp [ebp+2CCh]      timer += step in ms
//   0053FB10  fld [ebp+2CCh] / fcomp [00600D60h]     5000.0f
//   0053FB23  mov eax,[ebp+2D0h] / push eax          m_pSetOnFireEntity
//   0053FB2E  call dword [esi+74h]                   BlowUpCar
//
// The only other writes to [+2CCh] in the image are the constructor's zero
// at 0x0053E6C7 and code outside CBoat. The car's block at 0x00534510 resets
// its timer for a healthy car; this one never does. So "every destruction
// goes through BlowUpCar" further down still holds, but the list of callers
// there is one short, and so is its "exactly ONE place" that turns a raw
// m_fHealth into a wreck. CAutomobile's AUTO_FIRE_BLOWUP_TIMER at +0x530 lies
// past the end of a 0x484-byte boat - inside the pool slot, outside the
// object, and not the boat's timer. A write meant to hold a boat's timer has
// to go to +0x2CC.
namespace offs {
constexpr size_t BOAT_FIRE_BLOWUP_TIMER  = 0x2CC;   // float, ms
constexpr size_t BOAT_SET_ON_FIRE_ENTITY = 0x2D0;   // CEntity*
} // namespace offs
constexpr float BOAT_FIRE_HEALTH = 150.0f;

static_assert(offs::BOAT_FIRE_BLOWUP_TIMER >= offs::SIZEOF_VEHICLE &&
                  offs::BOAT_SET_ON_FIRE_ENTITY + 4 <= offs::SIZEOF_BOAT,
              "both are CBoat's own members");

// **The seat, which is the part that breaks first.** CPed::SetObjective's arm
// for OBJECTIVE_ENTER_CAR_AS_PASSENGER and _AS_DRIVER (14 and 15, both
// 0x004D8507 in the table at 0x005F8EFC) writes m_objective at 0x004D84EC and
// then does this before anything else:
//
//   004D8519  cmp dword [ebp+284h],1 / jne 004D8540   the car is a boat
//   004D8524  call CPed::IsPlayer / jne 004D8540       and we are not the player
//   004D852F  call 0x004D9460                          RestorePreviousObjective
//             ret 8
//
// 0x004D9460 is `m_objective = m_prevObjective, m_prevObjective = 0` (with
// the LEAVE_CAR special case), so for any ped that is not the player the
// objective is undone on the spot and m_carInObjective is never written. The
// engine never puts a pedestrian in a boat by objective. CPed::WarpPedIntoCar
// reads m_objective (0x004D7D94) to pick the seat, so the warp that follows
// takes its no-seat arm. For a replica that is every boat, every time.
//
// WarpPedIntoCar itself handles a boat fine once it gets that far: it writes
// m_pMyVehicle and m_carInObjective with their references, and after the
// seat `cmp [ebx+284h],1` at 0x004D7F43 picks boat animation 0x7A where a car
// gets 0x6F/0x70. SetEnterCar_AllClear has the same boat arm at 0x004E0C3B,
// but it is reached through the objective the same way.

// **Why a boat stops where its driver leaves it.** CPed::ProcessObjective's
// LEAVE_VEHICLE arm tests only for a train (`[edx+284h] == 2` at 0x004DA132,
// then CPed::SetExitTrain at 0x004E3640) and sends everything else, boats
// included, to CPed::SetExitCar at 0x004DA157 - whose first act is
// CanPedExitCar, and CanPedExitCar (0x005523C0) has no m_vehType test in it.
// So a player can only step off a boat that is already inside
// VEH_EXIT_MAX_SPEED_SQ and VEH_EXIT_MAX_TURN.
//
// And a vehicle never gets bIsStatic from CPhysical::ProcessControl. The
// quiet-frame counter "when the engine itself stops believing a thing is
// moving" transcribes is behind a type test at 0x00495F9A: `and al,7 / cmp
// al,4 / je` (an object) or `cmp al,3` plus a flag (a ped), and anything else
// jumps to 0x00496179 past it. CBoat's own code writes neither bIsStatic nor
// m_nStaticFrames. So for a boat, VehicleAtRest is VehicleAtRestNumbers and
// nothing else.

// ---- why a session car can become impossible to get into -------------------
//
// Recorded because it is the whole of the "press F on a car another player
// drove and you walk toward it forever" bug, and because nothing about the
// symptom points at it. CoopIII does not call either of these; what it has to
// do is stop making the first one return false.
//
// __thiscall bool CVehicle::CanPedEnterCar(void), disassembled 2026-09-22:
//
//   005522F0  mov edx,ecx / lea eax,[edx+24h] / three movs        GetUp()
//   00552303  fld [esp+0Ch]                                       up.z
//   00552307  fcom [0060258Ch]   = +0.1f    ; not greater ->
//   0055231C  fcom [00602590h]   = -0.1f    ; not less    -> return false
//   00552333  lea eax,[edx+78h]  MagnitudeSqr(m_vecMoveSpeed)
//   00552354  fcomp [00602594h]  = 0.04f    ; greater     -> return false
//   00552370  add edx,84h        MagnitudeSqr(m_vecTurnSpeed)
//   00552394  fcomp [00602594h]  = 0.04f    ; greater     -> return false
//                                           ; otherwise   -> return true
//
// The three constants were read out of the file, not derived: 0x3DCCCCCD,
// 0xBDCCCCCD and 0x3D23D70B at 0x0060258C. So the speed gate is sq(0.2f), the
// value re3 Vehicle.cpp:1009 has, and it is on BOTH velocities. It confirms
// MOVE_SPEED 0x78 and TURN_SPEED 0x84 a second time, from a function that
// reads them rather than writes them.
constexpr uintptr_t CVehicle__CanPedEnterCar = 0x005522F0;   // () -> bool
constexpr float     VEH_ENTER_MAX_SPEED_SQ   = 0.04f;

// And what the refusal costs, from CPed::SeekCar's arrival block:
//
//   004D44C0  movzx edx,byte [ebp+1CCh]     m_nNumMaxPassengers
//   004D44C7  movzx eax,byte [ebp+1C9h]     m_nNumGettingIn
//   004D44CF  cmp eax,edx / jge out         (after inc edx)
//   004D44D9  mov ecx,ebp / call 005522F0   CanPedEnterCar
//   004D44E0  test al,al / je 004D4617
//   004D4617  mov ecx,ebx / call 004C5E30   CPed::RestorePreviousState
//
// RestorePreviousState, and nothing else. m_objective is left at
// OBJECTIVE_ENTER_CAR_AS_DRIVER, so the next frame runs SeekCar again and the
// ped walks back to the same door. There is no timeout and no give-up: a car
// that fails CanPedEnterCar forever is a car the player can never enter, and
// the only sign of it is a character walking on the spot.
//
// The same block re-confirms PED_VEH_DOOR 0x2E8 as a *word*
// (`cmp word [ebx+2E8h],0Fh` at 004D452A) and CAR_DOOR_LF == 15 against
// VEH_DRIVER 0x1A4 - the two constants branch `entercar` found independently.
// 0x004D3F90 is where the function starts - 1720 bytes, ending at 0x004D4648,
// with the block above at its tail. Taken from the IDA function list beside
// the binary rather than by scanning backwards, and cross-checked by the one
// either side of it: 0x004D3E70/118 bytes is CPed::Teleport, which this file
// already records from an independent reading.
constexpr uintptr_t CPed__SeekCar              = 0x004D3F90;   // ()
constexpr uintptr_t CPed__RestorePreviousState = 0x004C5E30;   // ()

// __thiscall CEntity::GetDistanceFromCentreOfMassToBaseOfModel() -> float.
// CREATE_CAR adds it to the spawn z so the car sits on its wheels rather than
// with its centre of mass on the road surface.
constexpr uintptr_t CVehicle__GetDistanceFromCentreOfMassToBaseOfModel = 0x004755C0;

// CCarCtrl::JoinCarWithRoadSystem(CVehicle*). Opens by zeroing
// AutoPilot.m_nNextRouteNode (+0x130) and m_nCurrentRouteNode (+0x12C), which
// is both re3's first statement and another confirmation of VEH_AUTOPILOT.
constexpr uintptr_t CCarCtrl__JoinCarWithRoadSystem = 0x0041F820;

// ---- traffic generation ---------------------------------------------------
//
// What docs/population.md needs to tell the engine how crowded the street
// really is. Verified 2026-09-22, by the route that has worked every time
// here: walk the script opcode that already does the thing.
//
// SET_CAR_DENSITY_MULTIPLIER is opcode 491, so entry 91 of the 400..499 table
// at 0x005EEFC8 (base opcode a round 400, see the note at the top of this
// file). The handler is 0x004426FA and it is four instructions:
//
//   004426FA  lea  eax, [ebp+10h]              &ScriptParams
//             mov  ecx, ebp                    the running script
//             push 1 / push eax
//             call 004382E0                    CollectParameters(&m_nIp, 1)
//   00442707  fld  dword [006ED460]            ScriptParams[0]
//   00442715  fstp dword [005EC8B4]            CarDensityMultiplier
//
// That names three things at once, and then the multiplier names the
// generator: of the six instructions in the image that touch 0x005EC8B4,
// three are `mov imm32` resets, one is the store above, and the two `fmul`s
// are the readers. The one at 0x004166E3 is inside CCarCtrl code.
constexpr uintptr_t CTheScripts__CollectParameters = 0x004382E0;
constexpr uintptr_t CTheScripts__ScriptParams      = 0x006ED460;

// CCarCtrl::GenerateOneRandomCar(void). __cdecl, no arguments.
//
// Confirmed twice over. It is the function containing the density read, and
// its prologue carries re3's `static int32 unk = 0` as the MSVC pair an
// initialised function-local static compiles to - a guard byte and the
// storage:
//
//   004165F0  push ebx/esi/edi/ebp / sub esp,188h
//   004165FA  cmp  byte [0062356C], 0          the init guard
//   00416603  mov  byte [0062356C], 1
//   0041660A  mov  dword [00623568], 0         the static itself
//
// Then its first two gates are re3's first two statements, in order:
//
//   004166E3  fmul dword [005EC8B4]            * CarDensityMultiplier
//   004166E9  fmul dword [006182F8]            * CIniFile::CarNumberMultiplier
//   004166EF  fild dword [00943118]            NumRandomCars
//             fcompp / jne                     >= means return
//
//   00416710  mov  eax, [00943118]             NumRandomCars
//             add  eax, [008F1B38]             + five more counters
//             add  eax, [008F1B54]
//             add  eax, [008F29E0]
//             add  eax, [00885BB0]
//             add  eax, [009411F0]
//   00416733  cmp  eax, [005EC8B8]             MaxNumberOfCarsInUse
//             jl                               under it means carry on
//
// These two gates are the whole point: rewrite what they read and the engine
// counts everybody's cars before deciding to make another. population.md 1.3.
constexpr uintptr_t CCarCtrl__GenerateOneRandomCar = 0x004165F0;
constexpr uintptr_t CCarCtrl__CarDensityMultiplier = 0x005EC8B4;
constexpr uintptr_t CCarCtrl__MaxNumberOfCarsInUse = 0x005EC8B8;
constexpr uintptr_t CIniFile__CarNumberMultiplier  = 0x006182F8;

// The gate counter. Read twice in the two gates above, which is its own
// confirmation.
constexpr uintptr_t CCarCtrl__NumRandomCars = 0x00943118;

// The second term of that sum, and named independently a few instructions
// later by the wanted-level branch - re3's `NumLawEnforcerCars <
// pWanted->m_MaximumLawEnforcerVehicles`:
//
//   00416767  movzx eax, byte [edi+12h]        m_MaximumLawEnforcerVehicles
//   0041676B  cmp  dword [008F1B38], eax
//   00416771  jge                              at the cap, no cop car
constexpr uintptr_t CCarCtrl__NumLawEnforcerCars = 0x008F1B38;

// ---- who maintains those counters, and it is the constructor --------------
//
// Verified 2026-09-22, and this is the fact docs/population.md §3 step 4 had
// to establish before deciding whether traffic needs the counter rewriting.
//
// CCarCtrl::UpdateCarCount(CVehicle*, bool remove). __cdecl, two stack args:
//
//   004202E0  cmp   byte [esp+8], 0          the `remove` argument
//   004202E5  mov   ecx, [esp+4]             the vehicle
//   004202E9  jne   00420330                 -> the decrement half
//   004202EB  movzx eax, byte [ecx+1F4h]     VehicleCreatedBy  (offs 0x1F4)
//   004202F2  dec   eax / cmp eax,3 / ja out
//   004202F8  jmp   [eax*4 + 5ECC94h]        four-entry table, createdBy-1
//
// Both jump tables read out of the image, and between them they NAME three of
// the counters that the generator's sum left ambiguous - the table index is
// VehicleCreatedBy - 1, and that enum is fixed by the constructor argument
// CREATE_CAR passes:
//
//   0x005ECC94 (increment) = 4202FF, 420317, 42031F, 420327
//   0x005ECC84 (decrement) = 420344, 42035C, 420364, 42036C
//
//   index 0  RANDOM_VEHICLE     004202FF  test bIsLawEnforcer -> inc 008F1B38
//                                         then inc 00943118   (NumRandomCars)
//   index 1  MISSION_VEHICLE    00420317  inc 008F1B54
//   index 2  PARKED_VEHICLE     0042031F  inc 008F29E0
//   index 3  PERMANENT_VEHICLE  00420327  inc 008F29F0
//
// So 0x008F1B54 is NumMissionCars and 0x008F29E0 is NumParkedCars, which is
// the second independent witness those two needed - the first was only their
// presence in the generator's reassociated sum, which does not say which is
// which. 0x008F29F0 is NumPermanentCars and is NOT in that sum, which agrees:
// re3's sum does not include it either. By elimination the two still unnamed,
// 0x00885BB0 and 0x009411F0, are NumFiretrucksOnDuty and NumAmbulancesOnDuty
// in some order, and nothing here says which - they stay in
// docs/addresses-unverified.md.
//
// **And the callers are the constructor and the destructor**, which is the
// whole point. A whole-image scan for `call 0x004202E0` finds exactly three:
//
//   00550C89  push 0 / push eax / call 4202E0   inside CVehicle::CVehicle
//                                               (which starts at 0x00550A60)
//   005510CA  push 1 / push ebx / call 4202E0   inside ~CVehicle
//   00596DC4                                    the replay system
//
// This is the vehicle equivalent of CPed::CPed calling
// CPopulation::UpdatePedCount, and it is why population.md §1.3.1's ped
// result had a fair chance of repeating for cars. It is not the same, though,
// and the difference is the whole measurement: **which** counter a vehicle
// lands in is decided by the byte at +0x1F4, so a replica created as
// MISSION_VEHICLE is counted in NumMissionCars and is invisible to the
// generator's *first* gate, which reads NumRandomCars alone. A replica
// created as RANDOM_VEHICLE lands in the same counter its original did on the
// machine that made it, and both gates see it.
constexpr uintptr_t CCarCtrl__UpdateCarCount = 0x004202E0;
constexpr uintptr_t CCarCtrl__NumMissionCars = 0x008F1B54;
constexpr uintptr_t CCarCtrl__NumParkedCars  = 0x008F29E0;

// The other two terms of the sum are 0x00885BB0 and 0x009411F0. They are
// NumFiretrucksOnDuty and NumAmbulancesOnDuty in some order, and neither the
// sum nor UpdateCarCount says which - deliberately not guessed at, and listed
// in docs/addresses-unverified.md instead.

// ---- what a session car's copy costs the traffic budget --------------------
//
// Verified 2026-09-23 for game/carlife.h. A copy is built MISSION_VEHICLE, so
// UpdateCarCount's second arm puts it in NumMissionCars, and NumMissionCars is
// one of the six terms above. Every copy alive on this machine is one car
// fewer the engine will generate.
//
// The same six-term sum is compared against MaxNumberOfCarsInUse in exactly
// three places, found by scanning the whole image for [005EC8B8]:
//
//   00416733  cmp eax,[005EC8B8] / jl carry on    GenerateOneRandomCar
//   0041FC93  cmp eax,[005EC8B8] / jle carry on   the function at 0x0041FC50
//                                                 (called from 0x004165E7),
//                                                 re3's GenerateEmergencyServicesCar
//   004F4B05  cmp eax,[005EC8B8] / jl             CPopulation::AddToPopulation,
//                                                 the cop-car-for-a-cop branch
//
// and written in exactly one, CIniFile::LoadIniFile's `12.0f *
// CarNumberMultiplier` at 0x0059BF9E, whose only caller (0x0048BEED) is
// startup. NumMissionCars is read nowhere else: its 17 references are those
// three sums, two resets (0x0041D2BC, 0x0041D3DD) and incs and decs. So
// raising MaxNumberOfCarsInUse by what the copies add to the sum is the same
// thing, at all three gates, as the copies not being counted.
//
// PERMANENT_VEHICLE's counter, 0x008F29F0 (UpdateCarCount's fourth arm,
// 0x00420327), is not in the sum.
constexpr uintptr_t CCarCtrl__NumPermanentCars = 0x008F29F0;

// CPools::SaveVehiclePool(uint8 *buf, uint32 *size). __cdecl: GenericSave's
// only call to it, 0x0058FDD2, pushes &size and buf and pops both itself.
//
// It walks the vehicle pool (0x009430DC) twice, once to count and once to
// write, and both walks use the same test (0x004A20E0-0x004A212B, then
// 0x004A21E2-0x004A2223):
//
//   cmp [v+1A8h+i*4],0 for i < 8     any passenger -> skip
//   cmp [v+1A4h],0                   a driver      -> skip
//   cmp [v+284h],0 / cmp [v+1F4h],2  a car and MISSION_VEHICLE -> written
//   cmp [v+284h],1 / cmp [v+1F4h],2  a boat and MISSION_VEHICLE -> written
//
// and the write is a memcpy of the whole object (0x5A8 for a car, 0x484 for a
// boat) through 0x005B3BB0. So any empty mission car in the pool goes into the
// single-player save, and a parked session copy is exactly that. Nothing else
// in the function reads VehicleCreatedBy, so a copy that reads anything but 2
// for the length of the call is simply not written.
constexpr uintptr_t CPools__SaveVehiclePool = 0x004A2080;

// ---- pedestrian generation -------------------------------------------------
//
// The other half of what docs/population.md §1.3 needs. Verified 2026-09-22 by
// the same route as the traffic block above, and it landed on a correction
// worth reading before the addresses: **population.md §4 names the wrong
// function.** CPopulation::ManagePopulation does not decide whether to add a
// pedestrian - it is the *reaper*, and the gate that decides whether to make
// one lives in CPopulation::AddToPopulation. Both are named below.
//
// SET_PED_DENSITY_MULTIPLIER is opcode 990. That range is not laid out like
// the 400 one, so the table was found rather than assumed: the dispatcher's
// tenth arm is `cmp dx,3E8h / jge / movsx eax,dx / push eax / call 0044CB80`
// (0x00439608), and that handler opens
//
//   0044CB95  lea  eax, [ebx - 384h]           base opcode 900, not a round
//   0044CB9B  cmp  eax, 63h                    number pulled off the 400 case
//   0044CBA4  jmp  dword [eax*4 + 005EFA14]    100 entries
//
// Entry 90 of 0x005EFA14 is 0x0044F730, and it is the same four instructions
// the car one was, which is how this pass knew the base was right before
// trusting anything read out of the table:
//
//   0044F730  lea/mov eax,ecx = [esp+4] + 10h  &ScriptParams, the script
//             push 1 / push eax
//             call 004382E0                    CollectParameters(&m_nIp, 1)
//   0044F743  fld  dword [006ED460]            ScriptParams[0]
//   0044F751  fstp dword [005FA56C]            PedDensityMultiplier
//
// Same CollectParameters and same ScriptParams as opcode 491 - a third
// independent witness for both.
constexpr uintptr_t CPopulation__PedDensityMultiplier = 0x005FA56C;

// And the multiplier names the generator. Of the four instructions in the
// image that touch 0x005FA56C, two are `mov dword [005FA56C],3F800000h`
// resets back to 1.0f (0x00437C1C in CTheScripts, 0x004F3796 in
// CPopulation::Initialise), one is the store above, and exactly one is a
// reader:
//
//   004F4B55  fmul dword [005FA56C]
//
// which sits inside 0x004F4A00.
//
// __cdecl CPopulation::AddToPopulation(float minDist, float maxDist,
//                                      float minDistOffScreen,
//                                      float maxDistOffScreen).
//
// Identified three ways over, not one. It matches re3 Population.cpp:571-605
// statement for statement:
//
//   004F4A2B  movzx ebx,byte [0095CD61]        CWorld::PlayerInFocus
//             imul  ebx,ebx,13Ch               sizeof(CPlayerInfo)
//             add   ebx,009412F0               CWorld::Players
//   004F4A46  call  004A1170                   FindPlayerCentreOfWorld
//   004F4A6A  call  004B6FB0                   GetZoneInfoForTimeOfDay
//   004F4A71  call  004A1150 / mov eax,[eax+53Ch] / mov edx,[eax+18h]
//             cmp   edx,2 / jle                GetWantedLevel() > 2
//   004F4A90  movzx eax,byte [esi+11h]         m_MaxCops
//             cmp   dword [00885AFC],eax / jge   ms_nNumCop < m_MaxCops
//   004F4AA0  cmp   byte [ecx+314h],0 / jne    !bInVehicle
//   004F4AAD  movzx eax,byte [esi+12h]         m_MaximumLawEnforcerVehicles
//             cmp   dword [008F1B38],eax / jge
//   004F4AB9  mov   ax,[ebx+0FCh]              m_nTrafficMultiplier, a word
//             fild / fmul [005EC8B4] / fild [00943118] / fcompp
//   004F4AE2  the same six-term car sum, against [005EC8B8]
//
// - which is the entire `addCop` block, and on the way through re-confirms
// every address in the traffic block above from a second function. Then the
// gate this is all here for:
//
//   004F4B36  movzx esi,[esp+6Ch] / movzx eax,[esp+8Eh]
//             add   esi,eax                    pedDensity + carDensity
//   004F4B4B  fild  [esp+40h]
//   004F4B4F  fmul  dword [ebx+100h]           playerInfo->m_fRoadDensity
//   004F4B55  fmul  dword [005FA56C]           PedDensityMultiplier
//   004F4B5B  fmul  dword [006182F4]           CIniFile::PedNumberMultiplier
//   004F4B63  fild  dword [005FA574]           MaxNumberOfPedsInUse
//             fcomp / jne / fild again         Min(..., MaxNumberOfPedsInUse)
//   004F4BA0  cmp   dword [0095CB50],eax       ms_nTotalPeds < that
//             jl    carry on
//   004F4BA8  cmp   byte [esp+18h],0 / je out  ... || addCop
//
// This is the pedestrian equivalent of GenerateOneRandomCar's two gates, and
// it is the thing population.md §1.3 rewrites. Note what it actually reads:
// **ms_nTotalPeds**, not ms_nNumCivMale and ms_nNumCivFemale. Those two are
// inputs to it (see UpdatePedCount below) but the comparison is against the
// total, so that is the counter the replica count has to be written into.
constexpr uintptr_t CPopulation__AddToPopulation = 0x004F4A00;

// CIniFile::PedNumberMultiplier, four bytes below CarNumberMultiplier - which
// is how the pair are declared in re3 IniFile.cpp and a free cross-check on
// the one this project already had.
constexpr uintptr_t CIniFile__PedNumberMultiplier = 0x006182F4;

// CPopulation::MaxNumberOfPedsInUse, an int32 and not a float - the gate above
// reads it with `fild`, not `fld`. Two independent witnesses, and the second
// one is exact: CIniFile::LoadIniFile (0x0059BE20) ends with the ped line and
// the car line side by side,
//
//   0059BF3F  fld  dword [0061831C]            25.0f
//             fmul dword [006182F4]            * PedNumberMultiplier
//             fistp / mov [005FA574], eax
//   0059BF68  fld  dword [00618320]            12.0f
//             fmul dword [006182F8]            * CarNumberMultiplier
//             fistp / mov [005EC8B8], eax
//
// which is re3's `MaxNumberOfPedsInUse = 25.0f * PedNumberMultiplier` and
// `MaxNumberOfCarsInUse = 12.0f * CarNumberMultiplier` verbatim. The two
// constants really are 25.0f and 12.0f in the image, and the initialised data
// at the two destinations reads 25 and 12 as int32 - so the already-verified
// MaxNumberOfCarsInUse is what vouches for this one.
//
// This refutes the guess of 0x008F5F74 in docs/addresses-unverified.md.
constexpr uintptr_t CPopulation__MaxNumberOfPedsInUse = 0x005FA574;

// ---- the population counters ----------------------------------------------
//
// Not walked outwards from ms_nTotalMissionPeds - that walk is refuted, and
// its candidate for ms_nNumCivMale (0x008F5F78, 116 references) is nowhere
// near the truth. These came out of the function that maintains them.
//
// __cdecl CPopulation::UpdatePedCount(ePedType, bool decrease) at 0x004F5A60
// is two 23-entry jump tables over ePedType - 0x005FAACC to increment,
// 0x005FAA70 to decrement - and every arm is a single `inc`/`dec dword`. So
// the enum indexes the counters directly and nothing is inferred from
// declaration order:
//
//   type                       inc site   counter
//   PLAYER1..PLAYER4 (0..3)    -          falls through to the ret
//   CIVMALE     (4)            004F5A81   008F2548
//   CIVFEMALE   (5)            004F5A8C   008F5F44
//   COP         (6)            004F5A97   00885AFC
//   GANG1..GANG9 (7..15)       004F5AAD.. 008F1B1C 008F1B14 008F1B18 008F1B2C
//                                         008F1B30 008F1B20 008F1B28 008F1B0C
//                                         008F1B10
//   EMERGENCY/FIREMAN (16,17)  004F5AA2   0094071C   (both arms, one counter)
//   CRIMINAL    (18)           004F5B10   008F2548   (CivMale again)
//   UNUSED1     (19)           -          ret
//   PROSTITUTE  (20)           004F5B1B   008F5F44   (CivFemale again)
//   SPECIAL     (21)           -          ret
//   UNUSED2     (22)           004F5B26   008F1A98
//
// The decrement table is the same 23 arms pointing at the same 15 globals.
// That whole shape - six types that count nothing, two pairs of types sharing
// one counter, CRIMINAL landing on CivMale and PROSTITUTE on CivFemale - is
// re3's UpdatePedCount exactly, and no other assignment of names to those
// globals reproduces it. This is the proof; everything below just uses it.
constexpr uintptr_t CPopulation__UpdatePedCount = 0x004F5A60;

constexpr uintptr_t CPopulation__ms_nNumCivMale   = 0x008F2548;
constexpr uintptr_t CPopulation__ms_nNumCivFemale = 0x008F5F44;
constexpr uintptr_t CPopulation__ms_nNumCop       = 0x00885AFC;
constexpr uintptr_t CPopulation__ms_nNumEmergency = 0x0094071C;
constexpr uintptr_t CPopulation__ms_nNumDummy     = 0x008F1A98;

// The three totals. These are not maintained by UpdatePedCount at all - they
// are recomputed from the per-type counters in two places, and both compute
// the identical three lines, which is what names them:
//
//   004F39E0 (CPopulation::Update)  and  004F3AD6 (GeneratePedsAtStartOfGame)
//
//     mov  eax,[008F2548] / add eax,[008F5F44] / mov [008F2C3C],eax
//                                          ms_nTotalCivPeds = male + female
//     mov  eax,[008F1B1C] / add the other eight gangs / mov [00885AF0],eax
//                                          ms_nTotalGangPeds
//     mov  eax,[008F2C3C] / add [00885AF0] / add [00885AFC]
//                         / add [0094071C] / add [008F1A98]
//                         / mov [0095CB50],eax
//                                          ms_nTotalPeds
//
// re3 Population.cpp:430-435 and :451-456, term for term. Every one of these
// globals also gets a `mov dword [..],0` from CPopulation::Initialise
// (0x004F3770), which is the second witness each of them gets.
constexpr uintptr_t CPopulation__ms_nTotalCivPeds  = 0x008F2C3C;
constexpr uintptr_t CPopulation__ms_nTotalGangPeds = 0x00885AF0;

// The one the generator's gate actually compares. Read at 0x004F4BA0, written
// at 0x004F3A4E and 0x004F3B3D, zeroed at 0x004F37E0, and nowhere else in the
// image - four references, which is what a total recomputed once a frame and
// read once a frame should have.
constexpr uintptr_t CPopulation__ms_nTotalPeds = 0x0095CB50;

// The rest of CPopulation, named for free by CPopulation::Update's call order.
// Update is 0x004F39A0: `cmp byte [0095CD5B],1 / jne` is CReplay::IsPlayingBack,
// then the first two calls are ManagePopulation and
// MoveCarsAndPedsOutOfAbandonedZones, then the m_CountDownToPedsAtStart
// countdown that runs GeneratePedsAtStartOfGame - re3 Population.cpp:420-444
// in order.
constexpr uintptr_t CPopulation__Update = 0x004F39A0;

// CPopulation::ManagePopulation(void). The reaper, not the generator: its body
// opens with `mov bp,word [009412EC]` (CTimer::m_FrameCounter, already
// verified above) `and ebp,1Fh`, then sweeps a 1/32 slice of the *object* pool
// at stride 0x19C before it ever reaches a ped - which is re3's
// ManagePopulation opening the object sweep, and is nothing like a function
// that adds pedestrians.
constexpr uintptr_t CPopulation__ManagePopulation = 0x004F3B90;

constexpr uintptr_t CPopulation__MoveCarsAndPedsOutOfAbandonedZones = 0x004F5BE0;
constexpr uintptr_t CPopulation__GeneratePedsAtStartOfGame          = 0x004F3AD0;
constexpr uintptr_t CPopulation__Initialise                         = 0x004F3770;

// float CPopulation::PedCreationDistMultiplier(). Called from all four of
// Update, ManagePopulation, GeneratePedsAtStartOfGame and AddToPopulation,
// always immediately before an `fmul` against a creation-distance constant.
constexpr uintptr_t CPopulation__PedCreationDistMultiplier = 0x004F6410;

// int8, and the reason a fresh game makes a crowd: Update counts it down and
// runs GeneratePedsAtStartOfGame (fifty AddToPopulation calls, `cmp bx,32h`)
// when it hits zero. `cmp byte [0095CD4F],0` / `dec byte [0095CD4F]` at
// 0x004F39BA.
constexpr uintptr_t CPopulation__m_CountDownToPedsAtStart = 0x0095CD4F;

namespace offs {

// CPed::m_nPedType, a dword. Read off the two call sites of UpdatePedCount
// that matter: CPed::CPed pushes `(m_nPedType, false)`
//
//   004C5082  mov eax,[ecx+32Ch] / push 0 / push eax / call 004F5A60
//
// and CPed::~CPed pushes `(m_nPedType, true)`
//
//   004C51DE  mov eax,[ebx+32Ch] / push 1 / push eax / call 004F5A60
//
// so the field that decides which counter a ped belongs to is +0x32C. The
// values are the PEDTYPE_* constants already in this file - COP == 6 out of
// CREATE_CHAR - and the jump tables above independently confirm the enum runs
// 0..22 with no gaps.
constexpr size_t PED_TYPE = 0x32C;

} // namespace offs

// CPed::m_nCreatedBy is offs::PED_CHAR_CREATED_BY, already verified further up
// this file out of COMMAND_CREATE_CHAR's `mov byte [ebx+0x160],2`. It is
// listed in population.md §4 as still to find; it isn't. CHAR_CREATED_BY_RANDOM
// is what the ped generator leaves behind, and it is what tells an ambient ped
// from a mission one.

// CTheScripts::ClearSpaceForMissionEntity(const CVector&, CEntity*). CREATE_CAR
// calls it between SetPosition and SetStatus; it shoves whatever is standing in
// the spawn volume out of the way.
constexpr uintptr_t CTheScripts__ClearSpaceForMissionEntity = 0x00454060;

// CWorld::Remove(CEntity*). DELETE_CAR's teardown is Remove ->
// RemoveReferencesToDeletedObject -> deleting destructor through vtable slot 0
// with flag 1, i.e. the same shape as DELETE_CHAR plus the explicit Remove.
constexpr uintptr_t CWorld__Remove = 0x004AE9D0;

// Vtables, for the same live/dead test the ped path uses. CVehicle's is
// written by its constructor at 0x00550A71, CAutomobile's by its own at
// 0x0052C6D6 - both right after the base constructor returns.
constexpr uintptr_t CVehicle__vtable    = 0x006028A8;
constexpr uintptr_t CAutomobile__vtable = 0x00600C1C;

// eVehicleCreatedBy. The engine's own switch table (0x006026A4, reached from
// CanBeDeleted) is what fixes these values: 1 and 3 return "deletable", 2 and 4
// return "not". A remote player's car must be 2.
constexpr int VEHICLE_CREATED_BY_RANDOM    = 1;
constexpr int VEHICLE_CREATED_BY_MISSION   = 2;
constexpr int VEHICLE_CREATED_BY_PARKED    = 3;
constexpr int VEHICLE_CREATED_BY_PERMANENT = 4;

// eVehicleType, the value at offs::VEH_TYPE. TRAIN == 2 is read straight out of
// FindPlayerTrain's `cmp eax,2`; the rest follow from re3's enum order, and
// BIKE == 5 is confirmed by CREATE_CAR's IsBikeModel test against the model
// info's +0x58.
constexpr int VEHICLE_TYPE_CAR   = 0;
constexpr int VEHICLE_TYPE_BOAT  = 1;
constexpr int VEHICLE_TYPE_TRAIN = 2;
constexpr int VEHICLE_TYPE_HELI  = 3;
constexpr int VEHICLE_TYPE_PLANE = 4;
constexpr int VEHICLE_TYPE_BIKE  = 5;

// eCarLock, the value at offs::VEH_DOOR_LOCK. UNLOCKED == 1 because that is
// what CVehicle::CVehicle writes (`mov dword [eax+0x224],1`) where re3 has
// `m_nDoorLock = CARLOCK_UNLOCKED`.
constexpr int CARLOCK_NOT_USED = 0;
constexpr int CARLOCK_UNLOCKED = 1;
constexpr int CARLOCK_LOCKED   = 2;

// eEntityStatus values CREATE_CAR uses, packed into bits 3-7 of ENTITY_FLAGS.
// ABANDONED == 4 because the handler writes `or al,0x20` (4 << 3) there, and
// ENTITY_TYPE_VEHICLE == 2 because CVehicle::CVehicle writes
// `and al,0F8h / or al,2` into the same byte's bits 0-2.
constexpr uint8_t ENTITY_STATUS_SHIFT     = 3;
constexpr uint8_t ENTITY_STATUS_ABANDONED = 4;
constexpr uint8_t ENTITY_TYPE_VEHICLE     = 2;

// STATUS_WRECKED == 5 because CAutomobile::BlowUpCar writes `or al,0x28`
// (5 << 3) into the same bits, and CCarCtrl::PossiblyRemoveVehicle reads them
// back with `shr dl,3 / cmp eax,5`. Both are transcribed below.
constexpr uint8_t ENTITY_STATUS_WRECKED = 5;

// STATUS_PHYSICS == 3: the emergency-vehicle spawn at 0x00420212 writes
// `and al,7 / or al,18h` (3 << 3) and CAutomobile::ProcessControl's status
// switch (0x0053191E, table 0x00600A24) sends 3 to the arm that calls
// CCarAI::UpdateCarAI and SteerAICarWithPhysics (0x00531B3D).
//
// This used to say only the siren detour writes it. The engine writes it on
// every car CoopIII seats a replica in: CPed::WarpPedIntoCar does
// `and al,7 / or al,18h` at 0x004D7E9B for any ped IsPlayer says no to, and
// CPed::PedSetInCarCB does the same at 0x004CF4C2 at the end of an animated
// entry into the driver's seat. game/carstatus.h has what that means.
constexpr uint8_t ENTITY_STATUS_PHYSICS = 3;

// STATUS_PLAYER == 0: the same two functions write `and al,7` with no `or`
// for a ped IsPlayer says yes to (0x004D7E83; 0x004CF45D, which like the 3
// at 0x004CF4C2 is behind an m_objective == 15 test). There are five
// such stores in the image and nothing writes one back every frame, so a car
// that loses it keeps whatever it was given until the player gets out and in.
constexpr uint8_t ENTITY_STATUS_PLAYER = 0;

// ---- the car AI a status runs ------------------------------------------------
//
// CAutomobile::ProcessControl's switch, table 0x00600A24, read out of the image:
//
//   0 PLAYER      0x005319F0  ProcessControlInputs (vtable +48h) only if the
//                             driver's ped type is 0; DoDriveByShootings
//                             (0x00564000) at 0x00531A5D. The horn block after
//                             the switch reads CPad::GetHorn for status 0
//                             only (0x00533FFB, 0x00534191).
//   1, 6-9        0x00531BB3  straight to the physics, controls untouched
//   2 SIMPLE      0x00531A67  UpdateCarAI, then the rails (0x00418880)
//   3 PHYSICS     0x00531B3D  UpdateCarAI, SteerAICarWithPhysics,
//                             PlayHornIfNecessary (0x0053C4B0)
//   4 ABANDONED   0x00531B54  brake 0.2, handbrake off, steer 0, gas 0,
//                             horn timer 0 (0x00531BAC)
//   5 WRECKED     0x00531B7E  brake 0.05, handbrake on, same zeroes
//   10 REMOTE     0x00531925  the RC car, which reads pad 0 first thing
//   11 DISABLED   0x00531B68  brake 1.0, handbrake on, same zeroes
//
// __cdecl void CCarAI::UpdateCarAI(CVehicle *). `push ebx / push esi /
// sub esp,230h / mov ebx,[esp+23Ch]`, called only from the SIMPLE and
// PHYSICS arms (0x00531A68, 0x00531B3E), each `push ebp / call / pop ecx`.
// Mission bookkeeping: police missions for a law enforcer, the anti-reverse
// timer, and at the end a siren car that draws (randomSeed ^ rand()) == 0xAD
// gets a horn timer of 45.
//
// __cdecl void CCarCtrl::SteerAICarWithPhysics(CVehicle *). `push ebx /
// sub esp,10h / mov ebx,[esp+18h]`, one caller, 0x00531B45. Switches on
// m_nTempAction (`movsx eax,byte [ebx+15Bh] / dec eax`, table 0x005ECBC0) and
// otherwise on m_nCarMission (0x0041DD90, table 0x005ECBE8), then writes all
// four controls whatever it decided (0x0041DD4D-0x0041DD7E): steer to +1E8h,
// the handbrake to bit 5 of +1F5h, gas to +1ECh, brake to +1F0h. For
// MISSION_NONE (entry 0, 0x0041E1AD) that is steer 0, gas 0, handbrake on and
// brake 0.5 (3F000000h).
//
// A boat's PHYSICS arm (0x0053F376) calls neither. It calls
// __cdecl void CCarCtrl::SteerAIBoatWithPhysics(CVehicle *), `push ebx /
// mov ebx,[esp+8]`, one caller, 0x0053F388. MISSION_NONE (0x0041E267) zeroes
// the boat's own +2E0h/+2D8h/+2DCh, and 0x0041E2B1-0x0041E2DD publishes them
// whatever the mission was: steer to +1E8h, handbrake off, brake to +1F0h, gas
// to +1ECh, which CBoat::ProcessControl reads back at 0x0053FDEA.
constexpr uintptr_t CCarAI__UpdateCarAI              = 0x00413E50;
constexpr uintptr_t CCarCtrl__SteerAICarWithPhysics  = 0x0041DA60;
constexpr uintptr_t CCarCtrl__SteerAIBoatWithPhysics = 0x0041E250;

// __thiscall bool cAudioManager::ProcessVehicleSirenOrAlarm(cVehicleParams *),
// `ret 4` at all three exits (0x0056C4D1, 0x0056C5DB, 0x0056C5EA). Called
// once in the image, from the automobile arm of ProcessVehicle at 0x00569C2E.
// Opens `push ebx / push esi / push ebp / mov esi,ecx / sub esp,8` after the
// previous function's `ret 4` at 0x0056C417 and its padding. The gate it holds
// against a status-4 siren car is at 0x0056C4C4 (game/siren.h).
constexpr uintptr_t cAudioManager__ProcessVehicleSirenOrAlarm = 0x0056C420;

// ---- the siren's light bar -------------------------------------------------
//
// __thiscall void CAutomobile::PreRender(), CAutomobile's vtable slot 12
// (0x00600C1C + 0x30 = 0x00600C4C reads 0x00535B40). Opens `push ebx / push
// esi / push edi / push ebp / mov ebp,ecx / sub esp,6F8h`. The light bar is a
// switch on the model near the end:
//
//   005373D7  movsx eax,word [ebp+5Ch] / lea edx,[eax-61h] / cmp edx,33h
//   005373E3  ja 00537F82                      past the switch
//   005373E9  jmp [edx*4 + 00600AD0h]
//
// whose table sends 97 (fire truck), 106 (ambulance), 116 (police) and 117
// (Enforcer) to `cmp byte [ebp+22Eh],0 / je 00537F82` at 0x005373F0, 107 (FBI
// car) to the same test at 0x00537E4F, 110 (taxi) to 0x00537D1F and 113 (Mr
// Whoopee) straight to 0x00537F82. No status test on the way skips it: the
// only two before it in the function (0x00535DC0, 0x00536048) choose what the
// wheels do, and the status-4/5 arm, `jne 00539DB7` at 0x00536062, lands on
// 0x0053655E and falls through to the switch. So m_bSirenOrAlarm alone lights
// the bar, on any status.
//
// CAutomobile::ProcessControl (slot 8, 0x00600C3C reads 0x00531470) reads the
// same byte at 0x005347ED: with it set, the frame counter (0x009412EC) & 7 == 5,
// CVehicle::UsesSiren (0x00552200) and not Mr Whoopee (0x71), it calls
// 0x00416280, which re3 calls CCarAI::MakeWayForCarWithSiren - this machine's
// traffic pulls over for it.
constexpr uintptr_t CAutomobile__PreRender          = 0x00535B40;
constexpr uintptr_t CAutomobile__ProcessControl     = 0x00531470;
constexpr uintptr_t CAutomobile__PreRender_SirenBar = 0x005373F0;
constexpr uintptr_t CAutomobile__PreRender_FbiLight = 0x00537E4F;

// ---- a car's destruction ---------------------------------------------------
//
// Verified 2026-09-21 against the retail image, re-verified instruction by
// instruction 2026-09-22 (every transcription below was re-disassembled from
// the file, and two of the notes in this block were wrong - see the
// corrections marked "Corrected 2026-09-22"). Found from
// CExplosion::AddExplosion (0x005591C0, already here from M3): the only call
// site in the whole exe that chooses between EXPLOSION_CAR and
// EXPLOSION_CAR_QUICK is the one inside CAutomobile::BlowUpCar, which is
// re3 Automobile.cpp:3913-3915. From that call the function start falls out of
// the vtable, and everything BlowUpCar writes on the way is a field.
//
// ############################################################################
// # THE POINT OF THIS BLOCK: m_fHealth is a number, destruction is an event. #
// #                                                                          #
// # Writing m_fHealth does not destroy a car. The engine's own damage path   #
// # proves it in three instructions: CVehicle::InflictDamage writes          #
// # `mov dword [esi+200h],0` at 0x00551C10 and then, separately, calls       #
// # `call dword [ebx+74h]` at 0x00551C55. The zero and the destruction are   #
// # two acts, and only the second one blows anything up.                     #
// #                                                                          #
// # Every destruction goes through CAutomobile::BlowUpCar (a CBoat has its   #
// # own, below), and the callers are:                                        #
// #                                                                          #
// #   CVehicle::InflictDamage        a damage *transition* - the write above #
// #   CVehicle::ProcessDelayedExplosion   a bomb timer                       #
// #   CAutomobile::ProcessControl    the five-second fire timer below        #
// #   COMMAND_EXPLODE_CAR            the script (0x00442E08)                 #
// #                                                                          #
// # There is exactly ONE place in the engine that reads a raw m_fHealth and  #
// # can end up destroying a car, and that is the fire timer below - it needs #
// # health < 250 and then five seconds of ProcessControl. Nothing anywhere   #
// # tests m_fHealth for zero.                                                #
// #                                                                          #
// # So an observer that writes a health of 0 straight into a remote car's    #
// # m_fHealth gets a car with no health that is not destroyed. That is why   #
// # a synced car blew up on its driver's screen and nowhere else, and it is  #
// # why the fix is an event on the wire rather than another field.           #
// #                                                                          #
// # BlowUpCar is never called directly: a scan of .text for an E8/E9 rel32   #
// # to 0x0053BC60 or to CBoat's 0x00541CB0 finds nothing at all. Every one   #
// # of the callers above goes through vtable slot 29, which is why a detour  #
// # on the function catches all of them and a detour on a call site would    #
// # catch one.                                                               #
// ############################################################################
//
// __thiscall void CAutomobile::BlowUpCar(CEntity *culprit).  ret 4.
//
//   0x0053BC60  push ebx/esi/edi/ebp ; mov ebx,ecx      this
//   0x0053BC69  mov al,[ebx+1F7h] / shr al,6 / and al,1 / jne
//                                                       if (!bCanBeDamaged) return
//   0x0053BC80  fld [ebx+80h] / fadd [0x6007F8] / fstp  m_vecMoveSpeed.z += 0.13f
//   0x0053BC92  mov al,[ebx+50h] / and al,7 / or al,28h SetStatus(STATUS_WRECKED)
//   0x0053BC9C  mov al,[ebx+52h] / and al,0EFh / or al,10h   bRenderScorched = true
//   0x0053BCA6  mov eax,[0x885B48] / mov [ebx+210h],eax m_nTimeOfDeath = time
//   0x0053BCB1  lea ecx,[ebx+288h] / call 0x00545B70    Damage.FuckCarCompletely()
//   0x0053BCBC  cmp word [ebx+5Ch],83h                  GetModelIndex() != MI_RCBANDIT
//   ... bumper/door damage, the flying wheel, the dead occupants ...
//   0x0053BF20  push 0 / push esi / push 3 / push [esp+28h] / push ebx
//   0x0053BF2A  call 0x005591C0 / add esp,14h
//                                    AddExplosion(this, culprit, EXPLOSION_CAR, pos, 0)
//
//     Corrected 2026-09-22: this line used to show three pushes and no
//     `add esp`. It is five arguments, the same five M3 recorded for
//     COMMAND_ADD_EXPLOSION. The arm at 0x0053BF15 pushes 4 instead of 3,
//     which is what fixes EXPLOSION_CAR == 3 and EXPLOSION_CAR_QUICK == 4.
//
// re3 Automobile.cpp:3837-3925, statement for statement. Two offsets fall out
// of it for free and both are new: m_nTimeOfDeath at +0x210 and
// CDamageManager at +0x288, which is SIZEOF_VEHICLE, i.e. CAutomobile's first
// member.
constexpr uintptr_t CAutomobile__BlowUpCar = 0x0053BC60;

// CBoat::BlowUpCar. A genuinely different function - its own prologue, its own
// stack frame (`sub esp,28h` against CAutomobile's `sub esp,8`) - opening on
// the same `mov al,[ebp+1F7h] / shr al,6 / and al,1` bCanBeDamaged test. It is
// slot 29 of CBoat's vtable, which is how it was found: the constant
// 0x00541CB0 appears exactly once in the whole file, at 0x00600F18 in .data,
// i.e. 0x74 into the vtable at 0x00600EA4.
//
// It is hooked as well as CAutomobile's, because a detour on one function
// catches only that function. Without it the local player's own boat exploding
// says nothing to the session, and since 2026-09-23 SpawnRemoteVehicle builds
// a real CBoat for a boat model, so an observed boat's destruction comes
// through here too (see "boats" above).
constexpr uintptr_t CBoat__BlowUpCar = 0x00541CB0;

// It is virtual, and going through the vtable is what makes a replay right for
// whichever of the two the object actually is. Slot 29 in CAutomobile's vtable
// (0x00600C1C + 0x74 == 0x0053BC60), in CBoat's (0x00600EA4 + 0x74 ==
// 0x00541CB0) and in CVehicle's (0x006028A8 + 0x74 == 0x00444B10), where it is
// the do-nothing base: four instructions, `mov [esp+4],ecx / ret 4`. The
// engine calls it exactly this way - see the fire timer below, which ends on
// `call dword [ebx+74h]`.
constexpr size_t VTABLE_BLOW_UP_CAR = 29;

// __thiscall void CVehicle::ProcessDelayedExplosion(void). The bomb timer, and
// the second of BlowUpCar's callers. `mov ecx,ebp / call 0x00551C90` at
// 0x005347E6, two instructions after the fire block below, which is re3
// Automobile.cpp:1111 calling Vehicle.cpp:846. Its own tail is the third
// `call dword [ebx+74h]` in the vehicle modules, at 0x00551D7C.
constexpr uintptr_t CVehicle__ProcessDelayedExplosion = 0x00551C90;

// CVehicle::InflictDamage is BlowUpCar's first caller and the one that settles
// what this whole block is about. Its entry point is NOT recorded here: the
// instructions below were read at their own addresses and nothing CoopIII does
// needs to call the function, so recording a start address would be recording
// something unproven next to things that are proved. Two arms, both reached
// from the same 250.0f compare against the constant at 0x0060256C:
//
//   0x00551BA5  fld [esp+4] / fcomp [0x0060256C]      damage vs 250.0f
//   0x00551BBA  fld [esi+200h] / fcomp [0x0060256C]   m_fHealth vs 250.0f
//   0x00551BE1  lea ecx,[esi+288h] / push 0E1h / call 0x00545940
//                                                     the "set on fire" arm
//   0x00551BF3  mov [esi+574h],ebp                    m_pSetOnFireEntity
//   ---- the fatal arm ----
//   0x00551C10  mov dword [esi+200h],0                m_fHealth = 0
//   0x00551C55  mov ecx,esi / push ebp / mov ebx,[ecx]
//   0x00551C5A  call dword [ebx+74h]                  BlowUpCar(culprit)
//
// The write and the call are 69 bytes apart and neither implies the other.
// That is the whole reason a health of zero on the wire produced an
// intact-looking car with no health on the observer's screen.

// ---- the five-second fire timer, and why an observer must not run it -------
//
// This is the only place in the engine that reads a raw m_fHealth and
// destroys a car for it, which makes it the one path a health value copied
// off the wire can reach. Inside CAutomobile::ProcessControl:
//
//   0x00534761  fild qword [esp+0D0h]                CTimer::GetTimeStepInMs()
//   0x00534768  fadd  dword [ebp+530h]
//   0x0053476E  fstp  dword [ebp+530h]               m_fFireBlowUpTimer += step
//   0x00534774  fld   dword [ebp+530h]
//   0x0053477A  fcomp dword [0x00600730]             5000.0f
//   0x00534780  fnstsw ax / test ah,45h
//   0x00534785  jne   0x005347BA                     not past 5000 yet
//   0x00534787  movzx ecx,byte [0x0095CD61]          CWorld::PlayerInFocus
//   0x0053478F  imul  ecx,ecx,13Ch / add ecx,9412F0h CWorld::Players[..]
//   0x0053479B  call  0x004A15F0                     AwardMoneyForExplosion(this)
//   0x005347A2  mov   eax,[ebp+574h]                 m_pSetOnFireEntity
//   0x005347A8  mov   ebx,[ecx] / push eax
//   0x005347AB  call  dword [ebx+74h]                BlowUpCar(m_pSetOnFireEntity)
//   0x005347AE  jmp   0x005347BA
//   0x005347B0  mov   dword [ebp+530h],0             the ELSE arm, see below
//
// And the entry test, 0x2A0 bytes earlier, which is what the timer hangs off:
//
//   0x00534510  fld [ebp+200h] / fcomp [0x006005C0]  m_fHealth vs 250.0f
//   0x0053451E  and ah,5 / cmp ah,1
//   0x00534524  jne 0x005347B0                       not damaged -> reset timer
//   0x0053452A  mov cl,[ebp+50h] / shr cl,3
//   0x00534533  cmp eax,5
//   0x00534536  je  0x005347B0                       already wrecked -> reset
//
//     Corrected 2026-09-22: the old note called 0x005347B0 the "else" of the
//     5000.0f compare. It is not - 0x005347AE jumps straight over it. It is
//     the else of the *entry* test above, which is why holding the timer at
//     zero from CoopIII works at all: the engine only resets it for a car
//     that is healthy or already wrecked.
//
// re3 Automobile.cpp:1098-1106. m_fHealth at +0x200 is re-confirmed nine
// instructions after the timer by the engine-damage arm (`fld [ebp+200h] /
// fcomp [0x006005C0]` at 0x005347C1 against the same 250.0f), as is
// m_bSirenOrAlarm at +0x22E two instructions after that (0x005347ED).
//
// Note what is NOT in the entry condition: any flag, any damage event, any
// culprit. An observer that copies a health below 250 arms this timer, and
// five seconds later its own engine decides that somebody else's car is
// destroyed - at a moment its owner did not choose, at whatever position
// local physics had it in, crediting a null culprit, and paying the observer
// AwardMoneyForExplosion for it once per frame until it succeeds.
//
// vehicle.cpp holds this at zero for any car another player is driving or
// settling, which leaves the flames (those are drawn off m_fHealth, not off
// the timer) and takes away the decision. The BlowUpCar detour is the backstop for the other
// two callers.
namespace offs {
constexpr size_t AUTO_FIRE_BLOWUP_TIMER  = 0x530;   // float
constexpr size_t AUTO_SET_ON_FIRE_ENTITY = 0x574;   // CEntity*
constexpr size_t AUTO_DAMAGE_MANAGER     = 0x288;   // CDamageManager, == SIZEOF_VEHICLE
constexpr size_t VEH_TIME_OF_DEATH       = 0x210;   // uint32, ms
} // namespace offs

static_assert(offs::AUTO_DAMAGE_MANAGER == offs::SIZEOF_VEHICLE,
              "CDamageManager is CAutomobile's first member, so BlowUpCar's "
              "`lea ecx,[ebx+288h]` re-states sizeof(CVehicle)");
static_assert(offs::AUTO_FIRE_BLOWUP_TIMER < offs::SIZEOF_AUTOMOBILE &&
                  offs::AUTO_SET_ON_FIRE_ENTITY < offs::SIZEOF_AUTOMOBILE,
              "both live inside the object the pool actually strides for");
static_assert(offs::VEH_TIME_OF_DEATH > offs::VEH_CHANGE_GEAR_TIME &&
                  offs::VEH_TIME_OF_DEATH < offs::VEH_DOOR_LOCK,
              "m_nTimeOfDeath sits between two offsets Area E already proved");

constexpr float VEH_FIRE_HEALTH    = 250.0f;
constexpr float VEH_FIRE_BLOWUP_MS = 5000.0f;

// ---- what "burning" means for a custodian (boat.h, VehicleOnFire) ----------
//
// Verified 2026-09-23 against the retail image. A custodian keeps a burning
// car until it goes up, so it has to ask its own engine the same question the
// two fire blocks ask, and one more:
//
//   0x00534510  fld [ebp+200h] / fcomp [0x006005C0]    car: health < 250.0f
//   0x00534533  cmp eax,5 / je 0x005347B0              ...and not WRECKED
//   0x0053F917  fld [ebp+200h] / fcomp [0x00600D10]    boat: health < 150.0f
//   0x0053F653  cmp eax,5 / je 0x00541880              ...inside the <= 600
//                                                      block, not WRECKED
//
// And a CFire on the car, which is burning before the health gets there.
// CFire::ProcessFire's vehicle arm:
//
//   0x004799CA  cmp [esi+1E4h],ebx / je                m_pCarFire == this,
//                                                      else Extinguish
//   0x004799E0  cmp byte [ebx+1],0 / jne               not a script fire
//   0x004799E6  fld [0x005F1AC8] / fmul [0x008E2CB4]   1.2f * timestep
//   0x004799FB  push 9 / push [ebx+14h]
//   0x004799FE  call 0x00551950                        InflictDamage(src, 9, ..)
//
// and CFireManager::StartFire gives a car's fire 4000 ms plus up to 1000 more
// (0x0047978E rand, 0x004797A0 * 1/32768 * 1000, 0x004797CE add eax,0FA0h).
// So a lit car loses about 270 health to its own CFire and then, if that took
// it under 250, burns for the five seconds above.

// bRenderScorched is byte B (+0x52) bit 4, from BlowUpCar's
// `and al,0EFh / or al,10h`. It shares that byte with bExplosionProof (bit 1),
// which M3 found the same way and for the same reason - byte B is where the
// flags that did not fit in byte A ended up, and guessing which byte a
// CEntity flag lives in has cost this project two rounds.
namespace offs {
constexpr uint8_t ENTITY_RENDER_SCORCHED = 0x10;   // byte B
} // namespace offs

static_assert((offs::ENTITY_RENDER_SCORCHED & offs::ENTITY_EXPLOSION_PROOF) == 0,
              "two different bits of byte B, whatever else they share");

// ---- the reaping site that deletes a car BECAUSE it is locked --------------
//
// CCarCtrl::PossiblyRemoveVehicle's wreck branch, which begins at 0x00418726:
//
//   0x00418726  mov dl,[ebx+50h] / shr dl,3 / cmp eax,5 / jne out
//                                             GetStatus() != STATUS_WRECKED
//   0x00418738  mov eax,[ebx+210h] / test eax,eax / je out
//                                             m_nTimeOfDeath == 0
//   0x00418746  add eax,0EA60h                 + 60000 ms
//   0x0041874B  cmp [0x885B48],eax / jbe out   CTimer::GetTimeInMilliseconds()
//   0x00418757  call 0x00474CC0                GetIsOnScreen()
//   0x00418763  call 0x004AAA00                IsEntityCullZoneVisible()
//   0x004187B2  fcomp [0x005EC978]             distance from the player
//   0x004187DC  call 0x00428260                IsPointWithinHideOutGarage()
//   0x004187E7  call 0x004AE9D0                CWorld::Remove
//               ... then the deleting destructor
//
// ############################################################################
// # Corrected 2026-09-22. This block used to say, in bold, "there is no      #
// # bIsLocked test and no CanBeDeleted call anywhere in it". That is wrong.  #
// # Both are there, immediately above, and they are what JUMPS INTO the      #
// # branch:                                                                  #
// #                                                                          #
// #   0x004186BF  shr al,3 / and al,1 / jne 0x00418726   bIsLocked -> wreck  #
// #   0x004186C6  mov ecx,ebx / call 0x005511B0          CanBeDeleted()      #
// #   0x004186CF  test al,al / je 0x00418726             !deletable -> wreck #
// #   0x004186D4  call 0x00455350 / jne 0x00418726       in a mission -> ""  #
// #                                                                          #
// # So 0x00418726 is the ELSE of the two gates, not code that forgot them.   #
// # The engine's reasoning is "this car cannot be recycled as traffic; is it #
// # at least a minute-old wreck?" - and a minute-old wreck goes, gates and   #
// # all. The conclusion CoopIII depends on is unchanged and now rests on     #
// # the right reading: passing !bIsLocked && CanBeDeleted() does NOT keep a  #
// # wrecked mission car in the pool, it is precisely what routes it here.    #
// #                                                                          #
// # The practical consequence, which is what matters: a destroyed synced car #
// # leaves the pool about 60 seconds later, by itself, and CoopIII must not  #
// # read that empty slot as "respawn it".                                    #
// ############################################################################
//
// (re3 marks an added bIsLocked check as a FIX_BUGS at the top of the same
// function, CarCtrl.cpp:719-722. That is a *different* test in a different
// place, and reading it as this one is how the note above came to be wrong.
// Four times now a re3 reading has had to be corrected against the binary.)
constexpr uint32_t VEH_WRECK_REMOVAL_MS = 60000;

// ---- an explosion damages every car in its radius, on EVERY machine --------
//
// Verified 2026-09-22, and it is the measurement the unowned-car work
// (docs/roadmap.md §5.8) rests on, because it decides how much of the problem
// was a problem at all.
//
// CExplosion::AddExplosion ends on CWorld::TriggerExplosion, which walks the
// vehicle, ped and object lists of every sector the blast radius touches.
// The vehicle arm of CWorld::TriggerExplosionSectorList, in full:
//
//   0x004B1891  cmp al,2 / jne                  GetType() != ENTITY_TYPE_VEHICLE
//   0x004B1895  lea ecx,[ebp+50h]
//   0x004B1898  mov dl,[ecx] / shr dl,3 / cmp eax,2 / jne
//                                                GetStatus() == STATUS_SIMPLE
//   0x004B18A5  and al,7 / or al,18h            SetStatus(STATUS_PHYSICS == 3)
//   0x004B18AE  call 0x0041F7F0                 SwitchVehicleToRealPhysics
//   0x004B18B4  fld dword [0x005F79B0]          1100.0f
//   0x004B18BA  fmul dword [esp+14h]            * fDamageMultiplier
//   0x004B18C4  push 12h                        WEAPONTYPE_EXPLOSION
//   0x004B18C6  push [esp+0DCh]                 pCreator
//   0x004B18CD  call 0x00551950                 CVehicle::InflictDamage
//   0x004B18E0  cmp word [ebp+216h],0 / div si  m_nBombTimer /= 10
//
// re3 World.cpp:2105-2119, statement for statement, and 1100.0f appears
// exactly once in the whole image.
//
// ############################################################################
// # WHAT THIS MEANS FOR SYNC, and it is not small. fDamageMultiplier is      #
// # (radius - distance) * 2 / radius, clamped to 1 - a function of the       #
// # blast position and the car's position and nothing else. No random        #
// # number, no frame timing, no local AI.                                    #
// #                                                                          #
// # CoopIII already replays every player's explosion on every machine at a   #
// # fixed world position everyone agrees on (protocol.md §1.9.3). A parked   #
// # car is placed from the same map data on every machine. So a parked car   #
// # inside a replayed blast takes the SAME damage on every machine, through  #
// # the SAME function - the one that calls BlowUpCar through the vtable when #
// # the damage is fatal.                                                     #
// #                                                                          #
// # An unowned car blown up by an explosion is therefore already destroyed   #
// # everywhere, for free, by the same route §5.7 found for pavement fires.   #
// # What is NOT free is a car that got there by accumulated damage - gunfire #
// # spread and accuracy are rolled per machine, and a collision with a       #
// # replica is a collision with something whose transform is corrected       #
// # rather than simulated. Those diverge, and the fire timer above turns a   #
// # divergence in health into a divergence in whether the car ever blows up  #
// # at all. That is what the destruction event is a backstop for.            #
// ############################################################################
//
// __thiscall void CVehicle::InflictDamage(CEntity *culprit, eWeaponType,
//                                         float damage).
//
// Its entry point used to be deliberately absent from this file, on the
// grounds that the instructions recorded in the destruction block above were
// read at their own addresses and a start address would have been unproven.
// It is proven now and the reason is gone: 0x00551950 is the target of the
// `call` at 0x004B18CD above, it is a clean function entry (preceded by
// alignment padding, opening `push ebx / push esi / mov esi,ecx`), and that
// `mov esi,ecx` is what makes the `mov dword [esi+200h],0` at 0x00551C10
// - already recorded above - a write to m_fHealth. Seven call sites in the
// image: 0x004799FE (CFire::ProcessFire), 0x004B18CD (the explosion above)
// and five inside the weapon modules.
//
// ---- and now CoopIII does call it. Re-verified 2026-09-23, in full --------
//
// The block above recorded this address on the strength of one `call` and a
// three-instruction look at the prologue, and said plainly that nothing here
// called it. Both halves changed: game/vehicle.cpp now detours it and
// ApplyRemoteVehicleHit calls it, so the address, the convention and every
// argument had to be proved rather than inherited. One address in this project
// was found to be three bytes off in the same week, which is the reason this
// was re-read from the file instead of trusted.
//
// **It is a function start.** 0x00551944..0x0055194F is twelve bytes of 0x00
// alignment fill, after the previous function's `jmp` at 0x00551942. The entry
// is:
//
//   0x00551950  push ebx / push esi / mov esi,ecx / push ebp / sub esp,10h
//
// `mov esi,ecx` is what makes every `[esi+...]` below a member of `this`, and
// it is __thiscall for exactly that reason.
//
// **It takes three arguments and it closes `ret 0Ch`.** Every one of the seven
// exits is `add esp,10h / pop ebp / pop esi / pop ebx / ret 0Ch`, the last at
// 0x00551C83. Twelve bytes is three dwords and there is no fourth. With the
// prologue's 0x1C of pushes and locals, the callee reads them as:
//
//   [esp+20h] -> ebp     arg1  CEntity *culprit
//   [esp+24h] -> edx     arg2  eWeaponType, a full dword
//   [esp+28h]            arg3  float damage
//
// and the explosion call site writes the same three in the same order, which
// is the independent witness:
//
//   0x004B18BE  push eax                     reserve the float's slot
//   0x004B18BF  mov ecx,ebp                  this = the car
//   0x004B18C1  fstp dword [esp]             arg3 = 1100.0f * fDamageMultiplier
//   0x004B18C4  push 12h                     arg2 = WEAPONTYPE_EXPLOSION
//   0x004B18C6  push [esp+0DCh]              arg1 = pCreator
//   0x004B18CD  call 0x00551950
//
// (The transcription in the block above shows the `fld`/`fmul` and the two
// pushes but not the `push eax / fstp dword [esp]` pair between them, which is
// where the float actually lands. The conclusion was right and the listing was
// short by two instructions.)
//
// **It returns nothing.** No exit sets eax on purpose; the last one leaves it
// holding a model index off `movsx eax,word [esi+5Ch]` from the Yardie check
// at 0x00551C5D. Anything reading a bool out of this is reading a leftover.
//
// **Two gates in front of the damage, and the second one bites us.**
//
//   0x00551958  [esi+1F7h] bit 6 clear -> ret 0Ch     !bCanBeDamaged
//   0x00551972  [esi+53h]  bit 4 set   -> culprit must be FindPlayerPed()
//               (0x004A1150) or FindPlayerVehicle() (0x004A10C0), else ret
//                                                     bOnlyDamagedByPlayer
//
// On the owner's machine the culprit CoopIII passes is a replica of the
// *shooter's* ped, which is neither of those, so a car carrying
// bOnlyDamagedByPlayer takes nothing off the wire. That is a residual and it
// is left standing: the only way round it is to name the local player as the
// culprit, which is a lie about who fired, and the flag exists precisely to
// stop anybody but the player hurting that car.
//
// **The proof flags sit inside a switch, and the switch leaks.** This is the
// same shape as CPed::InflictDamage (docs/protocol.md §1.10.2) and it is why
// setting flags on a replica was never going to be the mechanism:
//
//   0x0055199F  cmp eax,13h / ja 0x00551A10        anything above 19: no check
//   0x005519A4  jmp [eax*4 + 0x006026CC]           twenty entries
//
// Resolved out of the file, the table is:
//
//   0, 1              -> 0x005519AB  [esi+53h] bit 3   bMeleeProof
//   2..7, 13, 19      -> 0x005519BE  [esi+53h] bit 0   bBulletProof
//   8, 10, 11, 18     -> 0x005519E6  [esi+52h] bit 1   bExplosionProof
//   9                 -> 0x005519D4  [esi+53h] bit 1   bFireProof
//   16                -> 0x005519FC  [esi+53h] bit 2   bCollisionProof
//   12, 14, 15, 17    -> 0x00551A10  nothing is checked at all
//
// So DETONATOR, TOTALWEAPONS, ARMOUR, RUNOVERBYCAR and every cause from 20
// up - DROWNING, FALL, UNIDENTIFIED - reach the health with no flag consulted.
// CoopIII sets exactly one of those five bits on an observed car
// (bCollisionProof, game/vehicle.cpp SetVehicleObserved) and must not set the
// rest: bExplosionProof would stop a replayed blast reaching a car it is
// supposed to reach identically on every machine. The detour is the positive
// statement; the flag stays as the half that still works if the detour fails.
//
// **Then the health.** `m_fHealth <= 0.0f` (0x00551A10, against the 0.0f at
// 0x00602534) leaves straight for the exit, so a call against a car that is
// already finished is harmless rather than merely wasteful. Otherwise:
//
//   0x00551AB1  mov [esi+228h],al                m_nLastWeaponDamage = weapon
//   0x00551AC5  health <= damage ? -> 0x00551C10 m_fHealth = 0, then BlowUpCar
//   0x00551ADE  m_fHealth -= damage
//   0x00551BA5  old health >= 250 and new < 250  the "set it on fire" arm
//
// The 250.0f is the constant at 0x0060256C. Note that 0x00551BA5 reads the
// *pre-hit* health out of the scratch slot written at 0x00551ABD, not the
// damage - the pair at 0x00551BA5 and 0x00551BBA is a transition test across
// 250, which is why a car catches fire once rather than every frame.
//
// CoopIII calls it in one place only, ApplyRemoteVehicleHit, and only on a car
// this machine's own player is driving.
//
// It is here because it is the function that turns damage into destruction,
// and every argument about who may destroy what ends up pointing at it.
// (WEAPONTYPE_EXPLOSION == 18 is already below, under the damage block, and
// the `push 12h` above is one more witness for it. CWorld::TriggerExplosion
// and CWorld::TriggerExplosionSectorList have no entry point recorded here on
// purpose: the instructions above were read at their own addresses, CoopIII
// calls neither, and an unproven start address next to proved ones is exactly
// what this file exists to prevent.)
constexpr uintptr_t CVehicle__InflictDamage   = 0x00551950;
constexpr float     VEH_EXPLOSION_BASE_DAMAGE = 1100.0f;

// The weapon switch's own ceiling: `cmp eax,13h / ja 0x00551A10` at
// 0x0055199F, so causes 0..19 are looked up in the table at 0x006026CC and
// everything from 20 up skips the proof flags entirely.
constexpr uint32_t VEH_INFLICT_WEAPON_TABLE_LEN = 20;

namespace offs {
// m_nLastWeaponDamage, written verbatim from the second argument's low byte at
// 0x00551AB1, before any health arithmetic and whatever the outcome.
constexpr size_t VEH_LAST_WEAPON_DAMAGE = 0x228;   // uint8
} // namespace offs

static_assert(offs::VEH_LAST_WEAPON_DAMAGE < offs::SIZEOF_VEHICLE,
              "m_nLastWeaponDamage is a CVehicle member, not a CAutomobile one");

// ---- CDamageManager: what a dented car actually stores ---------------------
//
// Verified 2026-09-22 for docs/cardamage.md. The whole class, its eleven
// accessors, everything in the image that writes it, and the four functions
// that turn a status byte into something you can see.
//
// The block above already proved WHERE it is - BlowUpCar's
// `lea ecx,[ebx+288h] / call 0x00545B70` and InflictDamage's
// `lea ecx,[esi+288h] / call 0x00545940`, two independent witnesses that
// CDamageManager is CAutomobile's first member. What follows is what is in it.
//
// Nothing here is arithmetic on a re3 declaration. Every field is named by an
// instruction that touches it:
//
//   0x005458B0  lea eax,[eax*4] / mov eax,0Fh / and [edx+14h],eax
//                                        SetPanelStatus: +0x14, four bits each
//   0x005458E0  mov eax,[edx+14h] / shr / and eax,0Fh      GetPanelStatus
//   0x00545860  add eax,eax / mov eax,3 / and [edx+10h],eax
//                                        SetLightStatus: +0x10, two bits each
//   0x00545890  mov eax,[edx+10h] / shr / and eax,3        GetLightStatus
//   0x00545900  mov [ecx+edx+5],al       SetWheelStatus: +0x05, uint8[4]
//   0x00545910  movzx eax,[ecx+eax+5]    GetWheelStatus
//   0x00545920  mov [ecx+edx+9],al       SetDoorStatus:  +0x09, uint8[6]
//   0x00545930  movzx eax,[ecx+eax+9]    GetDoorStatus
//   0x00545940  cmp eax,0FAh / mov [ecx+4],al
//                                        SetEngineStatus: +0x04, clamped to 250
//   0x00545960  movzx eax,[ecx+4]        GetEngineStatus
//
// sizeof is 0x1C and it is proved twice, neither time from a header:
//
//   0x00545850  xor eax,eax / push edi / mov edi,ecx / stosd x7 / pop edi / ret
//                                        ResetDamageStatus: 7 dwords = 28 bytes
//   0x0052C6EF  mov dword [edx+288h],3F400000h    m_fWheelDamageEffect = 0.75f
//   0x0052C6FD  mov byte  [eax+2A0h],1            field_18 = 1   (0x288 + 0x18)
//   0x0052C708  add eax,2A4h                      the NEXT member (0x288 + 0x1C)
//   0x0052C70D  push 6 / push 24h / push 0 / push 0052D150h / push eax
//   0x0052C719  call 0059CD10                     vector ctor iterator
//
// Six objects of stride 0x24 starting at 0x2A4 are CAutomobile::Doors[6], so
// there is nothing between field_18 and them. The 0.75f and the 1 are the
// inlined CDamageManager constructor, which is the only writer of either
// field: m_fWheelDamageEffect is read only as a traction multiplier for a
// WHEEL_STATUS_BURST wheel, and nothing in this build ever bursts one (see
// CAutomobile__BurstTyre below). Treat both as constants.
namespace offs {
// All relative to AUTO_DAMAGE_MANAGER, not to the vehicle.
constexpr size_t DMG_WHEEL_DAMAGE_EFFECT = 0x00;   // float, 0.75f, effectively const
constexpr size_t DMG_ENGINE_STATUS       = 0x04;   // uint8, 0..250
constexpr size_t DMG_WHEEL_STATUS        = 0x05;   // uint8[4]
constexpr size_t DMG_DOOR_STATUS         = 0x09;   // uint8[6]
constexpr size_t DMG_LIGHT_STATUS        = 0x10;   // uint32, 2 bits per light
constexpr size_t DMG_PANEL_STATUS        = 0x14;   // uint32, 4 bits per panel
constexpr size_t DMG_FIELD_18            = 0x18;   // uint8, written once, read nowhere
constexpr size_t SIZEOF_DAMAGEMANAGER    = 0x1C;

// CAutomobile::Doors[6], the member immediately after it, and the array of
// RwFrame* every applier subscripts.
constexpr size_t AUTO_DOORS      = 0x2A4;   // CDoor[6], stride SIZEOF_DOOR
constexpr size_t SIZEOF_DOOR     = 0x24;
constexpr size_t AUTO_CAR_NODES  = 0x37C;   // RwFrame *m_aCarNodes[]

// CVehicle::bIsDamaged, byte +0x1F7 bit 1. From SetComponentVisibility's
// `mov al,[ebx+1F7h] / and al,0FDh / or al,2` at 0x005300E8 - the same byte
// VEH_FLAGS_C already names, and a third bit of it after bIsLocked and
// bHasBeenOwnedByPlayer.
constexpr uint8_t VEH_IS_DAMAGED = 0x02;   // byte +0x1F7
} // namespace offs

static_assert(offs::DMG_DOOR_STATUS + 6 <= offs::DMG_LIGHT_STATUS,
              "six door bytes fit between the wheels and the light word");
static_assert(offs::DMG_WHEEL_STATUS + 4 == offs::DMG_DOOR_STATUS,
              "the door array starts where the wheel array ends");
static_assert(offs::AUTO_DAMAGE_MANAGER + offs::SIZEOF_DAMAGEMANAGER ==
                  offs::AUTO_DOORS,
              "CAutomobile::Doors[6] begins exactly where CDamageManager ends "
              "- the constructor's `add eax,2A4h`");
static_assert(offs::AUTO_DOORS + 6 * offs::SIZEOF_DOOR <= offs::AUTO_CAR_NODES,
              "and the six doors fit before m_aCarNodes");
static_assert(offs::AUTO_CAR_NODES + 20 * 4 < offs::SIZEOF_AUTOMOBILE,
              "twenty car nodes stay inside the object the pool strides for");

// The eleven accessors. Trivial, non-virtual, __thiscall, and worth having by
// address rather than reimplemented: SetPanelStatus and SetLightStatus are
// bitfield surgery on a shared dword and a hand-rolled copy that got the shift
// wrong would be a silent wrong panel.
constexpr uintptr_t CDamageManager__ResetDamageStatus   = 0x00545850;
constexpr uintptr_t CDamageManager__GetComponentGroup   = 0x00545790;
constexpr uintptr_t CDamageManager__SetLightStatus      = 0x00545860;
constexpr uintptr_t CDamageManager__GetLightStatus      = 0x00545890;
constexpr uintptr_t CDamageManager__SetPanelStatus      = 0x005458B0;
constexpr uintptr_t CDamageManager__GetPanelStatus      = 0x005458E0;
constexpr uintptr_t CDamageManager__SetWheelStatus      = 0x00545900;
constexpr uintptr_t CDamageManager__GetWheelStatus      = 0x00545910;
constexpr uintptr_t CDamageManager__SetDoorStatus       = 0x00545920;
constexpr uintptr_t CDamageManager__GetDoorStatus       = 0x00545930;
constexpr uintptr_t CDamageManager__SetEngineStatus     = 0x00545940;
constexpr uintptr_t CDamageManager__GetEngineStatus     = 0x00545960;

// The four that move a status one step, and the one that decides by how much.
// ProgressDoorDamage refuses at 3, ProgressPanelDamage at 3, ProgressWheelDamage
// at 2 - so every ladder climbs and none of them descends. That is what makes
// the wire word a monotone join (docs/cardamage.md §4).
//
//   0x0054597C  call GetDoorStatus  / cmp eax,3 / jne / inc eax / SetDoorStatus
//   0x00545A0C  call GetPanelStatus / cmp al,3  / jne / ...
//   0x00545A4C  call GetWheelStatus / cmp al,2  / jne / ...
//   0x005459B4  call GetEngineStatus / call CGeneral::GetRandomNumber
//               lea edx,[ebp+20h] / and eax,1Fh / add eax,edx
//               cmp bp,0E1h / jae ; cmp ax,0E0h / jbe ; mov eax,0E0h
//                                   status + 32 + (rand & 0x1F), capped at 224
//                                   below ENGINE_STATUS_ON_FIRE (225)
constexpr uintptr_t CDamageManager__ProgressDoorDamage   = 0x00545970;
constexpr uintptr_t CDamageManager__ProgressEngineDamage = 0x005459B0;
constexpr uintptr_t CDamageManager__ProgressPanelDamage  = 0x00545A00;
constexpr uintptr_t CDamageManager__ProgressWheelDamage  = 0x00545A40;

// __thiscall bool CDamageManager::ApplyDamage(tComponent, float damage,
//                                             float collisionMultiplier).
// ret 0Ch. The one decision in the whole system:
//
//   0x00545AA3  fmul dword [eax*4 + 006012B0h]   G_aComponentDamage[group]
//   0x00545AB2  fcomp dword [006012CCh]          > 150.0f ?
//   0x00545AD2  jmp dword [eax*4 + 006012D4h]    six arms, one per group
//   0x00545AEB  fcomp dword [006012D0h]          > 220.0f -> ProgressEngineDamage
//
//   006012B0  G_aComponentDamage[7] = 2.5, 1.25, 3.2, 1.4, 2.5, 2.8, 0.5
//             (bumper, wheel, door, bonnet, boot, panel, default)
//
// Deterministic. Scanning its caller CAutomobile::VehicleDamage end to end
// (0x0052F390-0x005300B5) for a rel32 to CGeneral::GetRandomNumber
// (0x005A41D0) finds none, so the only randomness anywhere in the damage
// model is ProgressEngineDamage above - and docs/cardamage.md §2.3 shows that
// one decides the density of the smoke and nothing else.
constexpr uintptr_t CDamageManager__ApplyDamage = 0x00545A80;

// __thiscall void CDamageManager::FuckCarCompletely(void). BlowUpCar's damage,
// and it takes no input of any kind:
//
//   0x00545B73  mov byte [ebx+5],2              m_wheelStatus[0] = MISSING
//   0x00545B7A  mov byte [ebx+9..0Eh],3         all six doors = MISSING
//   0x00545B92  ProgressPanelDamage(10h) / (11h)   x3   - see below
//   0x00545BAD  mov dword [ebx+10h],0           m_lightStatus = 0
//   0x00545BB9  mov dword [ebx+14h],0           m_panelStatus = 0
//   0x00545BC0  call SetEngineStatus(250)
//
// So a wrecked car's damage model is identical on every machine by
// construction, and protocol.md §1.11 already makes every machine run
// BlowUpCar. Nothing about a wreck needs to travel that does not travel now.
//
// The two ProgressPanelDamage calls pass 16 and 17, which are tComponent
// values where a ePanels index belongs - retail's own bug, the one re3 guards
// with FIX_BUGS. SetPanelStatus computes `panel*4` and `shl` masks the count
// to five bits, so 16 aliases panel 0 and 17 aliases panel 1; both are then
// overwritten by the `mov dword [ebx+14h],0` two instructions later. It is
// deterministic and it changes nothing. Do not "fix" it into a divergence.
constexpr uintptr_t CDamageManager__FuckCarCompletely = 0x00545B70;

// ---- the one door a dent comes through -------------------------------------
//
// __thiscall void CAutomobile::VehicleDamage(float impulse, uint16 piece).
// ret 8. It has exactly ONE caller in the image - 0x00531FE3, inside
// CAutomobile::ProcessControl, passing (0.0f, 0) - and all thirteen of
// ApplyDamage's call sites are inside it. There is no other path in the engine
// from anything to a dent, a smashed panel or a door on the pavement.
//
//   0x0052F390  fld dword [006004FCh]           0.2f, for an explicit call
//   0x0052F3BF  fld dword [ebp+10Ch]            m_fDamageImpulse
//   0x0052F3CF  mov di,word [ebp+120h]          m_nDamagePieceType
//   0x0052F3F8  [ebp+1F7h] bit 6                bCanBeDamaged
//   0x0052F47C  fcomp dword [006005B0h]         impulse > 25.0f
//   0x0052F653  [ebp+53h]  bit 4                bOnlyDamagedByPlayer
//   0x0052F685  [ebp+53h]  bit 2 -> ret 8       bCollisionProof
//   0x0052F6F9  GetLightStatus x4               oldLightStatus[], for the sound
//   0x0052F7C1  the first of thirteen ApplyDamage calls
//
// The bCollisionProof test at 0x0052F685 is one comparison and an unconditional
// return, before the first ApplyDamage. That is what makes the flag a complete
// switch here, and it is worth contrasting with protocol.md §1.10.2: on
// CPed::InflictDamage the proof flags sit inside a switch on the damage cause
// and two arms of that switch test nothing at all. On a car there is no switch
// in front of it.
//
// One thing is NOT behind that gate, and it is above it: the upside-down
// health drain at 0x0052F413 (`GetUp().z < 0.0f` -> m_fHealth -= 4.0f * step,
// the 4.0f at 0x006005A4). An observer holding a remote car on its roof still
// takes health off its own copy. Harmless today because the wire overwrites
// health 25 times a second and a wreck is already at zero; it stops being
// harmless the moment a car with no driver carries health.
constexpr uintptr_t CAutomobile__VehicleDamage = 0x0052F390;

constexpr float VEH_DAMAGE_IMPULSE_GATE  = 25.0f;    // 0x006005B0
constexpr float VEH_DAMAGE_THRESHOLD     = 150.0f;   // 0x006012CC
constexpr float VEH_DAMAGE_ENGINE_THRESH = 220.0f;   // 0x006012D0

namespace offs {
constexpr size_t VEH_DAMAGE_IMPULSE    = 0x10C;   // float,  m_fDamageImpulse
constexpr size_t VEH_DAMAGE_PIECE_TYPE = 0x120;   // uint16, m_nDamagePieceType
} // namespace offs

static_assert(offs::VEH_DAMAGE_IMPULSE < offs::VEH_DAMAGE_PIECE_TYPE &&
                  offs::VEH_DAMAGE_PIECE_TYPE < offs::SIZEOF_VEHICLE,
              "both are CPhysical members, so both are inside CVehicle");

// ---- making a status byte visible ------------------------------------------
//
// Writing m_panelStatus changes the record and nothing on screen, for the same
// reason writing m_fHealth destroys nothing (the block above) and writing
// m_aExtras fits nothing (protocol.md §1.12). What you see is an RpAtomic that
// was swapped or hidden when the status changed, and these are the functions
// that do the swapping.
//
// Each one re-reads the status out of the car's own CDamageManager - there is
// no variant that takes a value - so the sequence is always "write the status,
// then call the applier for that component":
//
//   0x005301A0  mov eax,[esp+8] / lea ecx,[ebx+288h] / call GetPanelStatus
//   0x005301B8  mov edx,[ebx+ebp*4+37Ch]    m_aCarNodes[component]
//   0x005301BF  test edx,edx / jne          no node -> return, silently
//   0x005301C8  cmp al,1                    PANEL_STATUS_SMASHED1 -> show damaged
//   0x005301D3  cmp al,3                    PANEL_STATUS_MISSING  -> spawn + hide
//
// void SetPanelDamage (int32 component, ePanels panel, bool noFlyingComponents)
// void SetBumperDamage(int32 component, ePanels panel, bool noFlyingComponents)
// void SetDoorDamage  (int32 component, eDoors  door,  bool noFlyingComponents)
// all __thiscall, all ret 0Ch.
constexpr uintptr_t CAutomobile__SetBumperDamage       = 0x00530120;
constexpr uintptr_t CAutomobile__SetPanelDamage        = 0x005301A0;
constexpr uintptr_t CAutomobile__SetDoorDamage         = 0x00530200;
constexpr uintptr_t CAutomobile__SpawnFlyingComponent  = 0x00530300;
constexpr uintptr_t CAutomobile__SetComponentVisibility = 0x005300E0;

// The all-at-once version, and the reason CoopIII does not use it.
//
// __thiscall void CAutomobile::SetupDamageAfterLoad(void). Twelve guarded
// calls - two bumpers, six doors, four wings, no windscreen - each
// `if(m_aCarNodes[c]) Set*Damage(c, panel)`. It is the engine's own answer to
// "the CDamageManager bytes are already right, make the model match", and it
// has exactly ONE caller in the whole image: 0x004A1D27, inside
// CPools::LoadVehiclePool - the same function Area E cites as proof of the
// CVehicle layout. A save writes the raw bytes into a fresh CAutomobile and
// then calls this.
//
// It passes noFlyingComponents = FALSE:
//
//   0x0053C31C  push 0 / push 5 / push 7 / call SetBumperDamage
//
// so every already-missing part is SpawnFlyingComponent'ed - a real CObject,
// out of the object pool, dropped at the car's feet. Tolerable once at a load
// screen; not tolerable for a late joiner handed eight damaged cars.
// docs/cardamage.md §5.2. Recorded because it is the proof of the recipe, not
// because CoopIII calls it.
//
// The twelve calls also pin the component indices and m_aCarNodes at +0x37C
// at the same time, e.g. `cmp dword [ebx+398h],0` is node 7 (0x37C + 7*4) and
// `cmp dword [ebx+3C0h],0` is node 17.
constexpr uintptr_t CAutomobile__SetupDamageAfterLoad = 0x0053C310;

// __thiscall void CAutomobile::Fix(void). The only thing in the engine that
// LOWERS a damage status - a Pay 'n' Spray, the script's FIX_CAR.
//
//   0x0053C247  lea ecx,[ebx+288h] / call 00545850   Damage.ResetDamageStatus()
//   0x0053C263  SetDoorStatus(2..5, MISSING) when pHandling->Flags & 0x10
//   0x0053C2A5  mov al,[ebx+1F7h] / and al,0FDh      bIsDamaged = false
//   0x0053C2C0  mov ebp,7 / walk m_aCarNodes[7..] putting the atomics back
//
// It is an event, not a number, exactly like BlowUpCar. It was out of scope
// while garages were unsynced at all (roadmap.md §4), and it is not any more:
// the garage work replays it on every observer, and the owner follows the
// respray with a VEH_DAMAGE_RESET so the record goes with the dents. See
// docs/cardamage.md §6.2 and the spray-shop arm below.
constexpr uintptr_t CAutomobile__Fix = 0x0053C240;

// __thiscall void CAutomobile::BurstTyre(uint8 piece). Present, correct, and
// UNREACHABLE in this build - which is why docs/cardamage.md §2.4 strikes
// wheels from the wire entirely.
//
//   0x0053C0EF  sub eax,0Dh / cmp eax,3 / ja      CAR_PIECE_WHEEL_LF..RR
//   0x0053C116  call GetWheelStatus / test eax,eax / jne    only an OK tyre
//   0x0053C12C  call SetWheelStatus(wheel, WHEEL_STATUS_BURST)
//
// Three independent readings say nothing reaches it:
//   - no E8/E9 rel32 to 0x0053C0E0 anywhere in .text;
//   - the constant 0x0053C0E0 appears exactly once in the whole file, at
//     0x00600C98, which is CAutomobile's vtable (0x00600C1C) + 0x7C, slot 31;
//   - scanning .text for every `call dword ptr [reg+7Ch]` returns exactly
//     three sites - 0x00582350, 0x005993C2, 0x0059A082 - and all three
//     disassemble as COM interface calls in the movie and networking code.
//
// The other route to a wheel status is ProgressWheelDamage, reached only from
// ApplyDamage's COMPGROUP_WHEEL arm, and VehicleDamage's four CAR_PIECE_WHEEL_*
// cases are a bare `break` - so nothing ever passes it a wheel either.
//
// Net: m_wheelStatus is only ever non-zero because FuckCarCompletely set wheel
// 0 to MISSING, inside a BlowUpCar every machine already runs. If a later
// build, a mod or a Vice City port ever dispatches slot 31, this paragraph is
// wrong and the wire needs a wheel nibble.
constexpr uintptr_t CAutomobile__BurstTyre = 0x0053C0E0;
constexpr size_t    VTABLE_BURST_TYRE      = 31;

static_assert(VTABLE_BURST_TYRE > VTABLE_BLOW_UP_CAR,
              "slot 31 sits after BlowUpCar's 29 in the same vtable, which is "
              "how both were found");

// ---- the small enums the appliers are indexed by ---------------------------
//
// Every value below is read off a `push` in BlowUpCar, SetupDamageAfterLoad or
// CReplay::ProcessCarUpdate rather than out of re3's header.
//
// eDoors, from BlowUpCar's six SetDoorDamage calls at 0x0053BCEA..0x0053BD2B:
// (component, door) = (11h,0) (12h,1) (0Fh,2) (0Bh,3) (10h,4) (0Ch,5).
enum eDoors : uint8_t {
	DOOR_BONNET = 0, DOOR_BOOT, DOOR_FRONT_LEFT, DOOR_FRONT_RIGHT,
	DOOR_REAR_LEFT, DOOR_REAR_RIGHT, NUM_DOORS
};

// ePanels, from SetupDamageAfterLoad's (0Dh,0) (09h,1) (0Eh,2) (0Ah,3) and
// BlowUpCar's two SetBumperDamage calls (07h,5) (08h,6). The windscreen is 4,
// from VehicleDamage's CAR_PIECE_WINDSCREEN arm - and note that
// SetupDamageAfterLoad does not apply it.
enum ePanels : uint8_t {
	VEHPANEL_FRONT_LEFT = 0, VEHPANEL_FRONT_RIGHT, VEHPANEL_REAR_LEFT,
	VEHPANEL_REAR_RIGHT, VEHPANEL_WINDSCREEN, VEHBUMPER_FRONT, VEHBUMPER_REAR,
	NUM_PANELS
};

// eCarNodes, the subscript into m_aCarNodes that every applier takes. Only the
// ones the damage path uses are listed; each is witnessed by a `push` above
// and cross-checked against the `cmp dword [ebx+<0x37C + n*4>],0` beside it.
enum eCarNodes : uint8_t {
	CAR_WHEEL_LF   = 4,    // BlowUpCar's SpawnFlyingComponent(4, COMPGROUP_WHEEL)
	CAR_BUMP_FRONT = 7,    // [ebx+398h]
	CAR_BUMP_REAR  = 8,    // [ebx+39Ch]
	CAR_WING_RF    = 9,    // [ebx+3A0h]
	CAR_WING_RR    = 10,   // [ebx+3A4h]
	CAR_DOOR_RF    = 11,
	CAR_DOOR_RR    = 12,
	CAR_WING_LF    = 13,
	CAR_WING_LR    = 14,   // [ebx+3B4h]
	CAR_DOOR_LF    = 15,
	CAR_DOOR_LR    = 16,
	CAR_BONNET     = 17,   // [ebx+3C0h]
	CAR_BOOT       = 18,
	CAR_WINDSCREEN = 19,
};

// eDoorStatus / ePanelStatus, from the `cmp eax,3` in ProgressDoorDamage and
// ProgressPanelDamage and from SetDoorDamage's own switch.
enum eDoorStatus : uint8_t {
	DOOR_STATUS_OK = 0, DOOR_STATUS_SMASHED, DOOR_STATUS_SWINGING,
	DOOR_STATUS_MISSING
};
enum ePanelStatus : uint8_t {
	PANEL_STATUS_OK = 0, PANEL_STATUS_SMASHED1, PANEL_STATUS_SMASHED2,
	PANEL_STATUS_MISSING
};

// ENGINE_STATUS_ON_FIRE, the 0E1h InflictDamage pushes at 0x00551BE7 and the
// value ProgressEngineDamage caps itself one below.
constexpr uint8_t ENGINE_STATUS_ON_FIRE = 225;
constexpr uint8_t ENGINE_STATUS_MAX     = 250;

// ---- parked cars, and the name the map already gives them ------------------
//
// Verified 2026-09-22 for the unowned-car destruction work. The problem this
// solves is naming: a parked car exists on every machine and nobody created
// it, so there is nothing to allocate a netId against and no creator to
// announce it. docs/population.md §1.2 says identity has to be handed out
// rather than chosen - and for a parked car the MAP hands it out, which is
// cheaper than the server doing it.
//
// Every PARKED_VEHICLE in GTA III comes out of a car generator, and the
// generators live in one fixed array filled from the IPLs in file order. The
// same files in the same order on every machine means the same index for the
// same generator, with no round trip and no allocation.
//
// Found from the one debug string that survived into the retail image,
// "CCarGenerator::DoInternalProcessing - can't find ground z for new car
// x = %f y = %f \n" at 0x00600F68, referenced exactly once, at 0x00542B77.
//
// CTheCarGenerators::Process (0x00542F40) is what pins the array, and it
// pins the stride twice over:
//
//   0x00542F57  inc byte [0x0095CDAF] / cmp ..,4    ProcessCounter, 0..3
//   0x00542F6D  movzx ebx,byte [0x0095CDAF]
//   0x00542F74  cmp ebx,[0x008E2C1C]                NumOfCarGenerators
//   0x00542F7C  imul ebp,ebx,48h / add ebp,87CB18h  CarGeneratorArray[i]
//   0x00542F89  call 0x00542BB0                     CCarGenerator::Process
//   0x00542F8E  add ebx,4 / add ebp,120h            i += 4, ptr += 4 * 0x48
//   0x00542F9F  cmp byte [0x0095CDC6],0 / dec       GenerateEvenIfPlayerIs..
//
// re3 CarGen.cpp:210-222. Note that the engine itself only ever walks the
// array up to NumOfCarGenerators - that is the bound, read off the binary,
// and nothing here indexes past it.
// ############################################################################
// # Confirmed in a running gta3.exe, 2026-09-22, read-only over              #
// # ReadProcessMemory at GS_PLAYING_GAME with a save loaded:                 #
// #                                                                          #
// #   NumOfCarGenerators = 149     (re3 NUM_CARGENS is 160; 149 fits)        #
// #   CurrentActiveCount = 133                                               #
// #   [0]  model 106  pos (1140.7 -630.2 -100.0)  ang   0.0  col -1/-1       #
// #   [1]  model 116  pos (1139.0 -646.0 -100.0)  ang  90.0  col -1/-1       #
// #   [3]  model  91  pos (1284.3 -620.5   11.7)  ang   0.0  col -1/-1       #
// #                                                                          #
// # Every field lands where the disassembly says: vehicle model indices,     #
// # Liberty City coordinates, angles of exactly 0/90/180/270 (degrees, as    #
// # DoInternalProcessing's DEGTORAD says), colours of -1 (the "roll one"     #
// # sentinel) and m_nUsesRemaining of 0xFFFF with one exhausted entry at 0.  #
// #                                                                          #
// # And the two counters are LIVE: writing 255 into                          #
// # GenerateEvenIfPlayerIsCloseCounter had the engine decrement it at ~62/s  #
// # (255 -> 235 -> 217 -> 198 -> 178 -> 160 over 1.5 s) while ProcessCounter #
// # cycled 3, 1, 0, 0, 2 - which is this function running once a frame, at   #
// # this address, over these globals.                                        #
// #                                                                          #
// # NOT confirmed in-game: a handle resolving to a live CVehicle. The test   #
// # save spawns in a walled courtyard where the only generator inside 120 m  #
// # is 52 m away, and CheckIfWithinRangeOfAnyPlayer also refuses anything    #
// # closer than farclip - 20 (re3 CarGen.cpp:200) - so nothing generates     #
// # there, forced counter or not. Every generator read -1 for the whole      #
// # session, which is the documented "no car" value rather than a bad read.  #
// ############################################################################
constexpr uintptr_t CTheCarGenerators__Process            = 0x00542F40;
constexpr uintptr_t CTheCarGenerators__CarGeneratorArray  = 0x0087CB18;
constexpr uintptr_t CTheCarGenerators__NumOfCarGenerators = 0x008E2C1C;  // int32
constexpr uintptr_t CTheCarGenerators__ProcessCounter     = 0x0095CDAF;  // uint8
constexpr uintptr_t CTheCarGenerators__CurrentActiveCount = 0x008F2C5C;  // int32
constexpr uintptr_t CTheCarGenerators__GenerateEvenIfPlayerIsCloseCounter =
    0x0095CDC6;   // uint8

constexpr uintptr_t CCarGenerator__Process             = 0x00542BB0;
constexpr uintptr_t CCarGenerator__DoInternalProcessing = 0x005426E0;
constexpr uintptr_t CCarGenerator__CalcNextGen         = 0x005426C0;
constexpr uintptr_t CCarGenerator__CheckIfWithinRange  = 0x00542E50;

// The CCarGenerator layout, all of it out of DoInternalProcessing's tail and
// Process's head, matched against re3 CarGen.h field for field:
//
//   0x00542B36  mov ax,[ebp+14h] / cmp ax,-1        m_nColor1
//   0x00542B40  cmp word [ebp+16h],-1               m_nColor2
//   0x00542AAA  movzx ecx,byte [ebp+19h]            m_nAlarm
//   0x00542AF4  movzx edx,byte [ebp+1Ah]            m_nDoorlock
//   0x00542B56  mov ecx,[0x009430DC] / push ebx
//   0x00542B5D  call 0x00429050                     GetVehiclePool()->GetIndex
//   0x00542B62  mov [ebp+24h],eax                   m_nVehicleHandle
//   0x00542B69  fld dword [ebp+8] ... [ebp+4]       m_vecPos.y, .x
//   0x00542B84  movzx eax,word [ebp+28h]            m_nUsesRemaining
//   0x00542B93  call 0x005426C0 / mov [ebp+20h],eax m_nTimer = CalcNextGen()
//   0x00542BB3  cmp dword [ebx+24h],-1              m_nVehicleHandle == -1
//   0x00542C18  mov byte [ebx+2Ah],1                m_bIsBlocking = true
//
namespace offs {
constexpr size_t CARGEN_MODEL_INDEX    = 0x00;   // int32
constexpr size_t CARGEN_POS            = 0x04;   // CVector
constexpr size_t CARGEN_ANGLE          = 0x10;   // float, degrees
constexpr size_t CARGEN_COLOUR1        = 0x14;   // int16, -1 == "roll one"
constexpr size_t CARGEN_COLOUR2        = 0x16;   // int16
constexpr size_t CARGEN_FORCE_SPAWN    = 0x18;   // uint8
constexpr size_t CARGEN_ALARM          = 0x19;   // uint8, percent
constexpr size_t CARGEN_DOORLOCK       = 0x1A;   // uint8, percent
constexpr size_t CARGEN_MIN_DELAY      = 0x1C;   // int16
constexpr size_t CARGEN_MAX_DELAY      = 0x1E;   // int16
constexpr size_t CARGEN_TIMER          = 0x20;   // uint32, ms
constexpr size_t CARGEN_VEHICLE_HANDLE = 0x24;   // int32, a POOL REF - see below
constexpr size_t CARGEN_USES_REMAINING = 0x28;   // uint16
constexpr size_t CARGEN_IS_BLOCKING    = 0x2A;   // bool
constexpr size_t SIZEOF_CARGENERATOR   = 0x48;
} // namespace offs

static_assert(offs::CARGEN_VEHICLE_HANDLE < offs::SIZEOF_CARGENERATOR &&
                  offs::CARGEN_IS_BLOCKING < offs::SIZEOF_CARGENERATOR,
              "every field read lives inside the stride Process strides by");

// ############################################################################
// # m_nVehicleHandle is a pool REF in exactly the encoding CPools::          #
// # GetVehicleRef produces, which is the whole reason this is usable.        #
// #                                                                          #
// # CPools::GetVehicleRef (0x004A1AC0) is four instructions:                 #
// #   mov eax,[esp+4] / mov ecx,[0x009430DC] / push eax / call 0x00429050    #
// # and 0x00429050 is the same call DoInternalProcessing makes at            #
// # 0x00542B5D with the same pool in ecx. One function, one encoding:        #
// #   0x00429071  shl eax,8 / add eax,ecx     (slot << 8) | flags byte       #
// #                                                                          #
// # So `CPools::GetVehicleRef(v) == gen->m_nVehicleHandle` is a valid test,  #
// # and CPools::GetVehicle (0x004A1AE0 -> GetAt 0x0043EAF0) resolves a       #
// # generator's handle with the free-slot check intact - a generator whose   #
// # car has been reaped resolves to null rather than to whatever took the    #
// # slot. GetAt's `imul eax,eax,5A8h` re-confirms SIZEOF_AUTOMOBILE.         #
// ############################################################################
//
// ############################################################################
// # WHAT THE INDEX IS NOT GOOD FOR. The map's generators are loaded first    #
// # and are identical everywhere. The SCRIPT can create more at runtime      #
// # (COMMAND_CREATE_CAR_GENERATOR), appended to the same array, and only     #
// # the host runs the script (docs/campaign.md) - so from the first script   #
// # generator onwards the two machines' arrays stop agreeing. CoopIII        #
// # therefore snapshots NumOfCarGenerators once, when the world has finished #
// # loading, and refuses to put any index at or above that on the wire.      #
// # Below it, the index is map data and means the same thing everywhere.     #
// ############################################################################

// ---- a vehicle's extra components ------------------------------------------
//
// Verified 2026-09-22. The second half of the same family of bug as the paint
// job: GTA III picks a car's extras at spawn, per machine, so two machines
// independently roll different ones and each player sees a car the other does
// not.
//
// ############################################################################
// # Extras are NOT like the colours, and copying the colour fix would do     #
// # nothing at all. m_currentColour1/2 are read by the renderer every frame, #
// # so writing them after construction changes the car. m_aExtras is a       #
// # *record* of a decision already taken: the components are RwAtomics that  #
// # were cloned into the clump during construction, and writing the bytes    #
// # afterwards changes the record and not one thing on screen.               #
// #                                                                          #
// # So an extra has to be forced BEFORE the constructor runs, through the    #
// # engine's own override, and the engine has one because the garages need   #
// # exactly this (re3 Garages.cpp:1905, CStoredCar::RestoreCar).             #
// ############################################################################
//
// CVehicleModelInfo::ms_compsToUse - int8[2], the override. Both bytes are
// 0xFE (-2) in the file at 0x005FF2EC, which is re3's initialiser
// `{ -2, -2 }` (VehicleModelInfo.cpp:24). -2 means "choose at random".
//
// It is ONE-SHOT, and that is the whole mechanism. CVehicleModelInfo::
// ChooseComponent (0x00520AB0) opens:
//
//   0x00520AB8  cmp byte [0x005FF2EC],0FEh
//   0x00520AC1  je  0x00520AD7                  -2: fall through to the roll
//   0x00520AC3  movsx eax,byte [0x005FF2EC]     otherwise: use it
//   0x00520ACD  mov byte [0x005FF2EC],0FEh      and put it back to -2
//   0x00520AD6  ret
//
// ChooseSecondComponent (0x00520BE0) is the same nine instructions against
// 0x005FF2ED. Note `movsx`: the value is signed, so -1 ("no component")
// survives the round trip and is what the engine itself passes for a car with
// no extra fitted.
constexpr uintptr_t CVehicleModelInfo__ms_compsToUse = 0x005FF2EC;

// CVehicleModelInfo::ms_compsUsed - int8[2], what was actually chosen. Read
// straight out of CVehicle::SetModelIndex (0x00551170), which is already in
// this file and whose whole body is six instructions:
//
//   0x00551179  call 0x00473E70                 CEntity::SetModelIndex
//   0x0055117E  mov dl,[0x0095CCB2]
//   0x00551185  mov [ebp+19Eh],dl               m_aExtras[0] = ms_compsUsed[0]
//   0x0055118B  mov al,[0x0095CCB3]
//   0x00551190  mov [ebp+19Fh],al               m_aExtras[1] = ms_compsUsed[1]
//   0x00551196  call 0x005219D0                 max passengers from door count
//
// which is re3 Vehicle.cpp:157-158 and the sixth independent confirmation of
// offs::VEH_EXTRAS at 0x19E.
constexpr uintptr_t CVehicleModelInfo__ms_compsUsed = 0x0095CCB2;

constexpr int8_t VEHICLE_COMPS_RANDOM = -2;   // ms_compsToUse: roll for it
constexpr int8_t VEHICLE_EXTRA_NONE   = -1;   // m_aExtras: nothing fitted

// ---- why the write has to happen before the constructor --------------------
//
// CEntity::SetModelIndex (0x00473E70) is four instructions:
//
//   0x00473E70  mov eax,[esp+4]
//   0x00473E75  mov [ecx+5Ch],ax                m_modelIndex
//   0x00473E79  mov ebx,[ecx] / call dword [ebx+14h]   CreateRwObject(), slot 5
//   0x00473E7F  ret 4
//
// and CVehicle::SetModelIndex is called from CAutomobile's constructor. So the
// clump - and therefore CVehicleModelInfo::CreateInstance, which is what does
// the choosing - is built INSIDE the constructor call. ms_compsToUse has to be
// set before CoopIII calls the constructor, exactly as the garage does.
constexpr uintptr_t CEntity__SetModelIndex = 0x00473E70;

// ---- and why it has to be put back afterwards ------------------------------
//
// CVehicleModelInfo::CreateInstance, 0x0051FCB0. Found by scanning .text for
// the two writes to ms_compsUsed (three sites each; the other one is
// SetModelIndex above). It is re3 VehicleModelInfo.cpp:181-222 statement for
// statement:
//
//   0x0051FCB9  call 0x004F8920                 CClumpModelInfo::CreateInstance
//   0x0051FCC1  cmp dword [ebp+1F4h],0
//   0x0051FCC8  je  0x0051FDA3                  if (m_numComps == 0) ...
//   0x0051FCD9  call 0x00520AB0                 comp1 = ChooseComponent()
//   0x0051FCE0  cmp ebx,-1 / je                 -1 means fit nothing
//   0x0051FCE5  mov eax,[ebp+ebx*4+1DCh]        m_comps[comp1]   <-- UNCHECKED
//   0x0051FD38  mov byte [0x0095CCB2],bl        ms_compsUsed[0] = comp1
//   0x0051FD3E  call 0x00520BE0                 comp2 = ChooseSecondComponent()
//   0x0051FD9B  mov byte [0x0095CCB3],bl        ms_compsUsed[1] = comp2
//   0x0051FDA3  mov byte [0x0095CCB2],0FFh      the m_numComps == 0 arm:
//   0x0051FDAA  mov byte [0x0095CCB3],0FFh      both used = -1 and RETURN
//
// ############################################################################
// # Two traps, both in that listing.                                         #
// #                                                                          #
// # 1. `mov eax,[ebp+ebx*4+1DCh]` is the ONLY thing done with the chosen     #
// #    index, and the only guard on it is `cmp ebx,-1`. There is no upper    #
// #    bound. A component index off the wire goes straight into that         #
// #    subscript and out of it into RpAtomicClone. m_comps is six pointers   #
// #    (+0x1DC to +0x1F3, with m_numComps at +0x1F4 immediately after), so   #
// #    anything at or above m_numComps reads another model's fields and      #
// #    calls RenderWare on them. Bound it, in CoopIII, against m_numComps -  #
// #    this is the same class of bug as the four-entry animation group and   #
// #    the twelve-slot node array, which is three for three now.             #
// #                                                                          #
// # 2. The m_numComps == 0 arm at 0x0051FDA3 sets ms_compsUsed and NEVER     #
// #    TOUCHES ms_compsToUse. So an override written for a model that has no #
// #    components is not consumed, and the next car created on that machine  #
// #    - traffic, a parked car, anything - picks it up instead. Put it back  #
// #    to { -2, -2 } after the constructor returns, unconditionally.         #
// ############################################################################
namespace offs {
constexpr size_t MODELINFO_COMPS      = 0x1DC;   // RpAtomic *[6]
constexpr size_t MODELINFO_NUM_COMPS  = 0x1F4;   // int32
constexpr size_t MODELINFO_COMP_RULES = 0x0EC;   // uint32
} // namespace offs

// Six, from the layout rather than from re3's declaration: m_comps starts at
// +0x1DC and m_numComps, read by CreateInstance's own `cmp dword [ebp+1F4h]`,
// sits at +0x1F4. (0x1F4 - 0x1DC) / 4 == 6. re3 agrees
// (VehicleModelInfo.h:107) but it is not the source here.
constexpr int MAX_VEHICLE_COMPS = 6;
static_assert((offs::MODELINFO_NUM_COMPS - offs::MODELINFO_COMPS) /
                      sizeof(void *) ==
                  MAX_VEHICLE_COMPS,
              "m_comps runs from +0x1DC up to m_numComps at +0x1F4");

// MODELINFOSIZE. ms_modelInfoPtrs is 5500 pointers, which is proved twice over
// by two separate walks of the array that both end on `cmp reg,157Ch / jl`:
// 0x00401121 and 0x00406919. It is also, independently, the value already in
// this file as STREAM_OFFSET_TXD - the streaming array puts txd ids straight
// after the last model id, so the two numbers are the same number.
constexpr uint32_t MODELINFO_SIZE = 0x157C;
static_assert(MODELINFO_SIZE == STREAM_OFFSET_TXD,
              "txd ids begin where model ids end");

// ms_modelInfoPtrs[id], bounded. Null for a model that has never been loaded,
// which every caller has to handle anyway.
inline void *VehicleModelInfo(uint32_t modelId) {
	if (modelId >= MODELINFO_SIZE)
		return nullptr;
	return reinterpret_cast<void **>(CModelInfo__ms_modelInfoPtrs)[modelId];
}

// How many extra components this model actually has, or 0 when the model is
// not loaded - in which case nothing may be forced, because CreateInstance
// would subscript m_comps on a model info that is not there.
inline int VehicleModelCompCount(uint32_t modelId) {
	void *const info = VehicleModelInfo(modelId);
	if (!info)
		return 0;
	const int32_t n = *reinterpret_cast<const int32_t *>(
	    reinterpret_cast<uint8_t *>(info) + offs::MODELINFO_NUM_COMPS);
	if (n <= 0)
		return 0;
	return n > MAX_VEHICLE_COMPS ? MAX_VEHICLE_COMPS : static_cast<int>(n);
}

// The one gate between a byte off the wire and CreateInstance's unchecked
// subscript. Pure arithmetic, so tools/clienttest covers it without the game.
//
// Anything that is not a component this model has becomes -1, "fit nothing".
// -1 is the engine's own value for that and the `cmp ebx,-1` above is the one
// bound CreateInstance does check, so it is the safe answer as well as the
// honest one: better a car missing an extra than a call into RenderWare with
// somebody else's pointer.
inline int8_t ClampVehicleExtra(int8_t wire, int compCount) {
	if (wire < 0 || compCount <= 0)
		return VEHICLE_EXTRA_NONE;
	if (wire >= compCount || wire >= MAX_VEHICLE_COMPS)
		return VEHICLE_EXTRA_NONE;
	return wire;
}

// ---- animation ------------------------------------------------------------
//
// Verified 2026-09-21 (milestone M1). Every rel32 below was resolved with
// tools/calltarget against the real binary, never by hand.
//
// A ped's animations live on its RenderWare clump, not in the CPed - the
// clump carries an RW plugin whose data pointer sits at a runtime offset held
// in the ClumpOffset global. CPed::SetModelIndex ends on that indirection:
//
//     mov  edx,[ebx+4Ch]              ; GetClump()
//     add  edx,[008F1B84h]            ; + ClumpOffset
//     mov  eax,[edx]                  ; -> CAnimBlendClumpData*
//     mov  [eax+0Ch],ecx              ; ->velocity2d = &m_vecAnimMoveDelta
//
// That's what pins the global, and CAnimManager::BlendAnimation opens with
// exactly the same three instructions.
constexpr uintptr_t RpAnimBlend__ClumpOffset = 0x008F1B84;

// CAnimManager::BlendAnimation(RpClump*, AssocGroupId, AnimationId, float),
// __cdecl. This is the API for starting an animation on a clump the way the
// engine does: fades the previous one out, reuses the association if one is
// already there, and syncs a movement anim to whatever movement anim is
// already running. re3 AnimManager.cpp:714-760.
//
// The disassembly matches that source statement for statement, and in the
// process witnesses the whole CAnimBlendAssociation layout below:
// `cmp eax,[ecx+2Ch]` is `anim->animId == animId`, `fld [ecx+18h]` is
// `blendAmount`, `mov [ecx+1Ch],0BF800000h` is `blendDelta = -1.0f`,
// `or [ecx+30h],4` is `flags |= ASSOC_DELETEFADEDOUT`, and `lea edx,[ebx+4]`
// is `&assoc->link`.
constexpr uintptr_t CAnimManager__BlendAnimation      = 0x00403710;
constexpr uintptr_t CAnimManager__AddAnimation        = 0x00403620;
constexpr uintptr_t CAnimManager__GetAnimAssociation  = 0x004035E0;

// CAnimManager::ms_aAnimAssocGroups - a pointer to an array of
// CAnimBlendAssocGroup, allocated at anim-load time. GetAnimAssociation is
// four instructions and gives both the global and the 8-byte stride:
//
//     mov  ecx,[esp+4] / lea ecx,[ecx*8] / add ecx,[008F583Ch] / jmp GetAnimation
//
// Null until CAnimManager::LoadAnimFiles has run, so it must be checked.
constexpr uintptr_t CAnimManager__ms_aAnimAssocGroups = 0x008F583C;

// CAnimBlendAssocGroup: { CAnimBlendAssociation *assocList; int32 numAssociations; }
//
// CAnimBlendAssocGroup::GetAnimation(uint32 id) is the whole reason
// numAssociations has to be read before every lookup:
//
//     8B 44 24 04   mov eax,[esp+4]
//     C1 E0 06      shl eax,6          ; * sizeof(CAnimBlendAssociation)
//     03 01         add eax,[ecx]      ; + assocList
//     C2 04 00      ret 4
//
// No bounds check anywhere in it. See docs/protocol.md §1.8.1.
constexpr size_t ANIMGROUP_ASSOC_LIST = 0x00;
constexpr size_t ANIMGROUP_COUNT      = 0x04;
constexpr size_t SIZEOF_ANIMGROUP     = 0x08;

// AssocGroupId. Only ASSOCGRP_STD holds the whole AnimationId namespace - the
// rest are walking styles with four or five entries each (re3 AnimManager.cpp
// ms_aAnimAssocDefinitions and the per-group name tables).
constexpr int ASSOCGRP_STD = 0;

// How many groups exist, read out of the allocation itself in
// CAnimManager::LoadAnimFiles (0x004038F0):
//
//     push 0D0h / push 401000h / push 401010h / push 8 / push 19h
//     call <vector new> / mov [008F583Ch],eax
//
// 0x19 elements of 8 bytes, and 0xD0 = 25*8 + 8 for MSVC's element count -
// so 25, not the 19 re3 declares by default. The extra six are the
// PC_PLAYER_CONTROLS strafe groups, and this build does have them.
constexpr int NUM_ANIM_ASSOC_GROUPS = 25;

// CAnimBlendAssociation, 0x40 bytes. The original has a virtual destructor,
// so a vtable pointer precedes re3's first declared member and everything
// shifts by four - which is why these come off the binary and not out of the
// header. Witnessed between four functions:
//
//   BlendAnimation  (0x00403710)  animId 2Ch, flags 30h, blendAmount 18h,
//                                 blendDelta 1Ch, link 04h
//   SetBlend        (0x004017E0)  writes 18h then 1Ch, in that order
//   SetCurrentTime  (0x00401700)  writes 20h, reads hierarchy 14h,
//                                 nodes 10h on a 0x1Ch stride
//   UpdateTime      (0x004031F0)  timeStep(28h) = (flags&20h ? relSpeed *
//                                 hierarchy->totalLength : speed(24h)) * dt;
//                                 currentTime(20h) += timeStep
//   SetFinishCallback (0x00401820) callbackType 34h, callback 38h, arg 3Ch
constexpr size_t ANIM_LINK_NEXT    = 0x04;
constexpr size_t ANIM_NUM_NODES    = 0x0C;
constexpr size_t ANIM_NODES        = 0x10;
constexpr size_t ANIM_HIERARCHY    = 0x14;
constexpr size_t ANIM_BLEND_AMOUNT = 0x18;
constexpr size_t ANIM_BLEND_DELTA  = 0x1C;
constexpr size_t ANIM_CURRENT_TIME = 0x20;
constexpr size_t ANIM_SPEED        = 0x24;
constexpr size_t ANIM_TIME_STEP    = 0x28;
constexpr size_t ANIM_ID           = 0x2C;
constexpr size_t ANIM_FLAGS        = 0x30;
constexpr size_t SIZEOF_ANIM_ASSOC = 0x40;

// CAnimBlendAssociation::FromLink is `link - offsetof(assoc, link)`, and the
// offset is 4 because of that vtable. Proved by BlendAnimation's own loop,
// which does `lea ecx,[esi-4]` on every node.
constexpr size_t ANIM_LINK_TO_ASSOC = 0x04;

// CAnimBlendClumpData::link.next is its first dword - BlendAnimation reaches
// the first association with `mov esi,[edi]` straight off the clump data
// pointer.
constexpr size_t ANIMCLUMP_LINK_NEXT = 0x00;
constexpr size_t ANIMCLUMP_VELOCITY  = 0x0C;

// Association flags (re3 AnimBlendAssociation.h:7-21). Four are witnessed
// directly in the functions above; the rest follow from the same enum and
// are only ever read, not written, here.
constexpr int32_t ASSOC_RUNNING         = 0x001;
constexpr int32_t ASSOC_REPEAT          = 0x002;
constexpr int32_t ASSOC_DELETEFADEDOUT  = 0x004;
constexpr int32_t ASSOC_FADEOUTWHENDONE = 0x008;
constexpr int32_t ASSOC_PARTIAL         = 0x010;
constexpr int32_t ASSOC_MOVEMENT        = 0x020;

// CAnimBlendAssociation::SetCurrentTime(float), __thiscall. Seeking by hand
// isn't equivalent - this one also uncompresses the hierarchy and re-seeks
// every node's key frame, which a raw write to currentTime would skip.
constexpr uintptr_t CAnimBlendAssociation__SetCurrentTime = 0x00401700;

// The locomotion animations. These four are the only ids present in every
// AssocGroup - which is exactly what makes them safe to look up in a ped's
// own m_animGroup (docs/protocol.md §1.8.1).
constexpr uint16_t ANIM_STD_WALK    = 0;
constexpr uint16_t ANIM_STD_RUN     = 1;
constexpr uint16_t ANIM_STD_RUNFAST = 2;
constexpr uint16_t ANIM_STD_IDLE    = 3;

// eMoveState (re3 Ped.h:290), and the reason the four ids above are the ones
// every group has: CPed::SetMoveAnim switches on this and blends exactly
// IDLE / WALK / RUN / RUNFAST out of the ped's own m_animGroup. Writing
// m_nMoveState is how a remote player walks instead of sliding - the engine
// picks the animation, we only say which gait.
constexpr uint32_t PEDMOVE_NONE   = 0;
constexpr uint32_t PEDMOVE_STILL  = 1;
constexpr uint32_t PEDMOVE_WALK   = 2;
constexpr uint32_t PEDMOVE_RUN    = 3;
constexpr uint32_t PEDMOVE_SPRINT = 4;
constexpr uint32_t PEDMOVE_LAST   = PEDMOVE_SPRINT;

// ---- weapons --------------------------------------------------------------
//
// CPed::GiveWeapon(eWeaponType, uint32 ammo) and
// CPed::SetCurrentWeapon(uint32 weaponType), both __thiscall, both matching
// re3 Ped.cpp:4700-4748 statement for statement. Together they're how a ped
// ends up holding a weapon *model*: SetCurrentWeapon removes the old model
// from the right-hand bone and adds the new one, and that part can't be done
// by writing m_currentWeapon directly.
constexpr uintptr_t CPed__GiveWeapon        = 0x004CF9B0;
constexpr uintptr_t CPed__SetCurrentWeapon  = 0x004CFA60;
constexpr uintptr_t CPed__AddWeaponModel    = 0x004CF8F0;
constexpr uintptr_t CPed__RemoveWeaponModel = 0x004CF980;

// CWeaponInfo::GetWeaponInfo(eWeaponType), __cdecl, five instructions:
//
//     8B 44 24 04   mov  eax,[esp+4]
//     6B C0 54      imul eax,eax,54h     ; sizeof(CWeaponInfo)
//     05 EC 03 65 00 add eax,6503ECh     ; CWeaponInfo::aWeaponInfo
//     C3            ret
//
// No range check, so the type has to be bounded before the call.
constexpr uintptr_t CWeaponInfo__GetWeaponInfo = 0x00564FD0;
constexpr uintptr_t CWeaponInfo__aWeaponInfo   = 0x006503EC;
constexpr size_t    SIZEOF_WEAPONINFO          = 0x54;

// m_nModelId is the model SetCurrentWeapon instantiates into the ped's hand,
// -1 for weapons that have none (fists, the detonator). Read out of
// SetCurrentWeapon itself: `call GetWeaponInfo / mov eax,[eax+4Ch] / push eax
// / call AddWeaponModel`. m_Flags is the dword right after it, tested for
// 0x80 (WEAPONFLAG_CANAIM_WITHARM) at the end of CPed::SetAimFlag.
constexpr size_t WEAPONINFO_ANIM_TO_PLAY = 0x34;
constexpr size_t WEAPONINFO_MODEL_ID     = 0x4C;
constexpr size_t WEAPONINFO_FLAGS        = 0x50;

// The animation block in the middle of CWeaponInfo, and the reason a remote
// player's gun looked like it was being drawn over and over instead of fired.
//
// A weapon has one animation, not three. The draw, the aim pose, the shot and
// the recovery are all frames of it, and which part you see is decided by
// where the association is parked and whether it is running:
//
//   m_fAnimLoopStart   CPed::PointGunAt parks it here and clears
//                      ASSOC_RUNNING. That frozen frame is the "gun up,
//                      ready" pose, and it is what a player holding an aim
//                      without firing is showing.
//   m_fAnimFrameFire   CPed::FireGun discharges the weapon when the playhead
//                      crosses this.
//   m_fAnimLoopEnd     and wraps back to m_fAnimLoopStart while the trigger
//                      is held, which is the automatic-fire loop. It never
//                      plays past here until the player stops.
//
// All four read out of CPed::FireGun in re3's own order (PedFight.cpp:
// 525-770), which also re-confirms m_AnimToPlay and m_Flags:
//
//   0x004E6BC4  mov esi,[eax+34h]     weaponAnim = m_AnimToPlay
//   0x004E6C0A  fld dword [eax+44h]   delayBetweenAnimAndFire = m_fAnimFrameFire
//   0x004E6CBF  mov ebp,[eax+38h]     m_Anim2ToPlay
//   0x004E6CE3  mov eax,[eax+50h]     IsFlagSet(WEAPONFLAG_THROW)
//   0x004E6D16  fld dword [eax+3Ch]   animStart = m_fAnimLoopStart
//   0x004E71B9  fld dword [eax+40h]   animLoopEnd = m_fAnimLoopEnd
//
// and the class is bracketed at both ends by offsets this file already had:
// m_AnimToPlay at 0x34, m_nModelId at 0x4C, m_Flags at 0x50, total 0x54.
constexpr size_t WEAPONINFO_ANIM2_TO_PLAY   = 0x38;
constexpr size_t WEAPONINFO_ANIM_LOOP_START = 0x3C;
constexpr size_t WEAPONINFO_ANIM_LOOP_END   = 0x40;
constexpr size_t WEAPONINFO_ANIM_FRAME_FIRE = 0x44;

constexpr uint32_t WEAPONFLAG_CANAIM          = 0x040;
constexpr uint32_t WEAPONFLAG_CANAIM_WITHARM  = 0x080;

// eWeaponType. The inventory types are 0..12 and double as slot indices into
// CPed::m_weapons. Everything from WEAPONTYPE_LAST_WEAPONTYPE (13) upward is
// a damage *cause* (drowning, falling, run over) with no slot at all.
constexpr uint8_t WEAPONTYPE_UNARMED         = 0;
constexpr uint8_t WEAPONTYPE_DETONATOR       = 12;
constexpr uint8_t WEAPONTYPE_LAST_INVENTORY  = 12;

static_assert(WEAPONINFO_FLAGS + 4 == SIZEOF_WEAPONINFO,
              "m_Flags is the final CWeaponInfo member (re3 WeaponInfo.h:41)");
static_assert(WEAPONTYPE_LAST_INVENTORY + 1 == offs::NUM_WEAPON_SLOTS,
              "there is exactly one m_weapons slot per inventory weapon type");

// ---- aim ------------------------------------------------------------------
//
// CPed::SetAimFlag(float worldYaw) and CPed::ClearAimFlag(), both __thiscall.
// Per docs/protocol.md §1.8.3, aim has to go in by setting these inputs and
// letting CPed::ProcessControl rotate the torso later in the frame -
// CWorld::Process rewrites every bone matrix from the animation before
// ProcessControl runs, so anything CoopIII writes into a bone from its own
// hook is gone by the time the frame is drawn.
//
// SetAimFlag also decides AIMS_WITH_ARM from the current weapon's flags, so
// calling it instead of writing bIsAimingGun keeps a pistol aiming with the
// arm and a rifle with the torso without CoopIII needing to know which is
// which.
constexpr uintptr_t CPed__SetAimFlag   = 0x004C6960;
constexpr uintptr_t CPed__ClearAimFlag = 0x004C6A50;

// CPed::IsPlayer(), called at the tail of ClearAimFlag. Recorded because it's
// the cheap way to tell a remote player's CCivilianPed from the local
// CPlayerPed, and that call site is the proof of it.
//
// The body was read on 2026-09-22 and agrees: `mov edx,[ecx+32Ch]` - which is
// the `m_nPedType` this file already had - then 0, 1..2 and 3 in turn, i.e.
// re3's four PEDTYPE_PLAYER values. It is a test on the ped *type* and not on
// being FindPlayerPed(), which matters: CFireManager::StartFire decides the
// AI arm on the latter and the extinguish time on this.
constexpr uintptr_t CPed__IsPlayer = 0x004D48E0;

// ---- rendering ------------------------------------------------------------
//
// Verified 2026-09-21 while chasing the second "invisible ped". The whole
// render path for a ped got walked in the retail disassembly and matched
// against reference/re3 statement for statement. The four functions below
// carry the proof and pin every address in this section, plus a few flag
// bytes CoopIII had never needed before.
//
//   CRenderer::ScanSectorList (0x004A9BB0) is re3 Renderer.cpp:1467-1539:
//     mov  ax,[0095CC64h] / cmp [ebp+58h],ax     m_scanCode vs ms_nCurrentScanCode
//     call 004AAA00                              IsEntityCullZoneVisible
//     call 004A9350                              SetupEntityVisibility
//     jmp  [eax*4+005F72CCh]                     switch over VIS_*
//     inc  [00940730h] / mov [eax*4+006E9920h]   InsertEntityIntoList
//
//   CRenderer::SetupEntityVisibility (0x004A9350) is re3 Renderer.cpp:695-770.
//   Its non-simple-modelinfo branch (the one a ped takes) is exactly four
//   tests, and they are the complete list of reasons a ped is not drawn:
//     cmp  [ebp+4Ch],0        -> VIS_INVISIBLE   m_rwObject == nil
//     [ebp+52h] bit 2         -> VIS_INVISIBLE   !bIsVisible
//     call 00474CC0           -> VIS_OFFSCREEN   !GetIsOnScreen()
//     [ebp+54h] bit 5         -> alpha list      bDrawLast
//
//   CRenderer::RenderEverythingBarRoads (0x004A7930) is re3 Renderer.cpp:311-352:
//     cmp  al,3 / mov eax,[esi+4Ch] / call 00528F70 / cmp eax,0FFh
//   i.e. a PED whose clump alpha isn't 255 doesn't get rendered here at all -
//   it goes into CVisibilityPlugins' sorted alpha list instead.
//
//   CPed::ProcessControl (0x004C8910) opens with re3 Ped.cpp:1697-1714: the
//   m_nZoneLevel gate, then GetClumpAlpha, then `bFadeOut ? alpha-8 : alpha+16`
//   clamped to 0..255, then SetClumpAlpha. This is the only thing in the whole
//   engine that raises a ped's clump alpha - which is why a ped that never
//   runs ProcessControl stays invisible no matter what else is right.

constexpr uintptr_t CRenderer__SetupEntityVisibility    = 0x004A9350;
constexpr uintptr_t CRenderer__IsEntityCullZoneVisible  = 0x004AAA00;
constexpr uintptr_t CRenderer__ScanSectorList           = 0x004A9BB0;
constexpr uintptr_t CRenderer__RenderEverythingBarRoads = 0x004A7930;
constexpr uintptr_t CRenderer__RenderOneNonRoad         = 0x004A7BA0;
constexpr uintptr_t CRenderer__ms_aVisibleEntityPtrs    = 0x006E9920;
constexpr uintptr_t CRenderer__ms_nNoOfVisibleEntities  = 0x00940730;
constexpr size_t    NUM_VISIBLE_ENTITIES                = 2000;

// CEntity::GetIsOnScreen(), __thiscall, bool in AL. Builds the bound centre
// and radius from the model's col model, then asks TheCamera.
constexpr uintptr_t CEntity__GetIsOnScreen = 0x00474CC0;

// The two halves of GetIsOnScreen, resolved from the two E8 calls inside it
// (0x00474CCB and 0x00474CD2) with tools/calltarget. Both read the bounding
// sphere out of the model's collision model:
//   0x00474310  movsx ecx,[ecx+0x5C] / mov edx,[ecx*4+0x0083D408] / mov eax,[edx+0x1C]
// i.e. ms_modelInfoPtrs[m_modelIndex]->m_colModel->boundingSphere. Missing
// col model means GetIsOnScreen answers "no" for a ped standing right next to
// the camera - exactly the symptom this chased down.
constexpr uintptr_t CEntity__GetBoundCentre = 0x004742C0;   // __thiscall(CVector *out)
constexpr uintptr_t CEntity__GetBoundRadius = 0x00474310;   // __thiscall -> float
constexpr uintptr_t CModelInfo__ms_modelInfoPtrs_base = 0x0083D408;
constexpr size_t    MODELINFO_COLMODEL = 0x1C;
constexpr uintptr_t TheCamera              = 0x006FACF8;

// CVisibilityPlugins' per-clump extension. GetClumpAlpha is five instructions
// and gives both the global and the layout at once:
//     mov eax,[esp+4] / add eax,[0060012Ch] / add eax,4 / mov eax,[eax] / ret
// so ClumpExt is { RpClump *(*visibilityCB)(...); int32 alpha; }, alpha being
// the dword at +4. SetClumpAlpha mirrors it.
constexpr uintptr_t CVisibilityPlugins__GetClumpAlpha        = 0x00528F70;
constexpr uintptr_t CVisibilityPlugins__SetClumpAlpha        = 0x00528F50;
constexpr uintptr_t CVisibilityPlugins__ms_clumpPluginOffset = 0x0060012C;
constexpr size_t    CLUMPEXT_VISIBILITY_CB                   = 0x00;
constexpr size_t    CLUMPEXT_ALPHA                           = 0x04;

// CWorld::ms_nCurrentScanCode (uint16) and CEntity::m_scanCode (uint16, +0x58).
constexpr uintptr_t CWorld__ms_nCurrentScanCode = 0x0095CC64;

// CWorld::ms_listMovingEntityPtrs - the head pointer of the CPtrList every
// CPhysical is put on by CWorld::Add. CWorld::Process walks it three times:
// once to update animations, once for ProcessControl, once for collision. A
// ped not on this list is never animated and never collides.
// CPtrNode is { void *item; CPtrNode *prev; CPtrNode *next; } - CWorld::Process
// reads `mov ebp,[edi] / mov edi,[edi+8]`.
constexpr uintptr_t CWorld__ms_listMovingEntityPtrs = 0x008F433C;
constexpr size_t    PTRNODE_ITEM = 0x00;
constexpr size_t    PTRNODE_PREV = 0x04;
constexpr size_t    PTRNODE_NEXT = 0x08;
constexpr size_t    SIZEOF_PTRNODE = 0x0C;   // the `push 0Ch` every allocator uses

// CWorld::Process. Its animation loop is re3 World.cpp:1865-1876 exactly:
//     mov eax,[ebp+4Ch] / test / cmp byte [eax],2        rpCLUMP
//     push eax / call 004031B0                           GetFirstAssociation
//     test eax,eax / je skip
//     fld [008E2CB4h] / fmul [005F79B8h]                 timeStep * 0.02
//     push / push clump / call 004024B0                  UpdateAnimations
// and its collision loops are re3 World.cpp:1908-1957:
//     call [vtbl+20h] / call [vtbl+24h]                  ProcessCollision/Shift
//     lea ecx,[ebp+4] / call 004B8EC0                    GetMatrix().UpdateRW()
//     mov ecx,ebp     / call 00474330                    UpdateRwFrame()
constexpr uintptr_t CWorld__Process = 0x004B1A60;

// RpAnimBlend. The clump's animation list, and the function that evaluates
// it into the clump's frame hierarchy. CWorld::Process calls UpdateAnimations
// every frame for every moving clump entity with at least one association -
// so a remote ped does NOT need CoopIII to call it, as long as it's on the
// moving list and CPed::SetModelIndex left an idle association on it.
constexpr uintptr_t RpAnimBlendClumpGetFirstAssociation = 0x004031B0;
constexpr uintptr_t RpAnimBlendClumpUpdateAnimations    = 0x004024B0;

// How many animations may be on a clump at once before the engine starts
// writing over its own stack.
//
// RpAnimBlendClumpUpdateAnimations builds an array of the nodes it is about
// to blend, in a local, and neither fills it nor terminates it with a bound:
//
//   0x004024B4  sub  esp, 40h                     the whole frame
//   0x004024F0  xor  esi, esi                     i = 0
//   0x00402527  mov  [esp+esi*4+10h], eax         nodes[i++] = assoc->GetNode(0)
//   0x0040256E  mov  dword [esp+esi*4+10h], 0     nodes[i] = nil
//
// The array starts at esp+0x10 and the frame ends at esp+0x40, so there is
// room for twelve. What follows it is the function's own saved state.
//
// **The order of those saved registers was written down backwards here, and
// it is the half of this note that decides which count is fatal.** The
// prologue is five instructions and its bytes are
// `53 56 57 55 83 EC 40` at 0x004024B0:
//
//   004024B0  53           push ebx
//   004024B1  56           push esi
//   004024B2  57           push edi
//   004024B3  55           push ebp
//   004024B4  83 EC 40     sub  esp,40h
//
// ebp is pushed last, so ebp is the one nearest esp, not ebx:
//
//   [esp+40h] saved ebp   <- nodes[12]
//   [esp+44h] saved edi   <- nodes[13]
//   [esp+48h] saved esi   <- nodes[14]
//   [esp+4Ch] saved ebx   <- nodes[15]
//   [esp+50h] return address                      <- nodes[16]
//   [esp+54h] the clump argument                  <- nodes[17]
//   [esp+58h] timeDelta                           <- nodes[18]
//
// `mov eax,[esp+54h]` at 0x004024B7 reads the clump argument, which is what
// pins the whole table: the argument really is at +0x54, so the four saved
// registers really are at +0x40..+0x4C in push order.
//
// A thirteenth animation corrupts the caller's registers, a seventeenth the
// return address, and an eighteenth the clump. The last of those is what the
// 2026-09-21 crash dump caught: ESI was 0x11, the terminator wrote null over
// the clump argument, and two hundred bytes later the function's own tail
// read it back and faulted at 0x004025D2 on `mov eax,[eax+4]`, address 4.
//
// Worth knowing that saved edi and esi are in that window, because
// CWorld::Process's walk over the moving list keeps the node cursor in **edi**
// and the entity in ebp across this very call:
//
//   - 12 associations: saved ebp is replaced. Harmless - the walk's very next
//     instruction is `mov ebp,[edi]`, which overwrites it anyway.
//   - 13: saved edi is nil'd. The walk's `test edi,edi` then ends walk 1
//     early. A dropped frame of animation, no crash.
//   - **14: saved esi is nil'd and saved edi comes back holding
//     nodes[13] - a CAnimBlendNode\*.** The walk resumes on it, reads
//     `[node+0]` as node->item into ebp and `[node+8]` as node->next into
//     edi, and dereferences ebp+0x4C. CAnimBlendNode is
//     `{ float theta; float invSin; int32 frameA; ... }` (re3
//     AnimBlendNode.h), so a node sitting at the start of its sequence -
//     theta 0.0f from CalcDeltas on two identical key frames, frameA 0 -
//     puts **exactly zero** in both, and the fault is a read of 0x0000004C.
//   - 15: saved ebx is nil'd.
//   - 16: the return address.
//   - 17: the clump argument.
//
// Fourteen is therefore the only count that reproduces a fault at
// 0x004B1B25 *and* leaves ebx untouched, which is why AnimNodeArrayOverflow
// below is written out per index rather than as "past twelve is bad".
//
// **re3 declares this array as sixteen** (`CAnimBlendNode *nodes[16]`,
// src/animation/RpAnimBlend.h:8-12). The retail build has twelve. Anyone
// sizing a cap from re3 would be four over and would not find out until the
// game corrupted its own stack.
//
// The terminator is written at nodes[count], so eleven associations is the
// last count that stays inside the frame.
constexpr int ANIM_UPDATE_NODE_SLOTS  = 12;
constexpr int MAX_CLUMP_ANIM_ASSOCS   = ANIM_UPDATE_NODE_SLOTS - 1;   // 11

// RwObject::type for a clump. Walk 1 tests it with `cmp byte ptr [eax],2` at
// 0x004B1B2C before it will look at an entity's animations, and so does
// anything that mirrors walk 1.
constexpr uint8_t RW_TYPE_CLUMP = 2;

// What the terminator write `nodes[count] = nil` lands on, for a clump that
// has `count` associations surviving UpdateBlend.
//
// Pure arithmetic over the stack map above, so tools/clienttest can pin it
// without a game. It exists because "twelve is the limit" is true and is not
// enough: the register a given count destroys is what decides whether the
// symptom is a dropped frame or a crash at 0x004B1B25, and that mapping was
// recorded backwards in this file until 2026-09-22.
enum class AnimNodeArrayOverflow {
	None,            // count <= 11: the terminator stays inside the frame
	SavedEbp,        // 12
	SavedEdi,        // 13
	SavedEsi,        // 14  <- the one that crashes CWorld::Process's walk 1
	SavedEbx,        // 15
	ReturnAddress,   // 16
	ClumpArgument,   // 17 and past it
};

inline AnimNodeArrayOverflow AnimNodeArrayTarget(int count) {
	switch (count) {
	case ANIM_UPDATE_NODE_SLOTS + 0: return AnimNodeArrayOverflow::SavedEbp;
	case ANIM_UPDATE_NODE_SLOTS + 1: return AnimNodeArrayOverflow::SavedEdi;
	case ANIM_UPDATE_NODE_SLOTS + 2: return AnimNodeArrayOverflow::SavedEsi;
	case ANIM_UPDATE_NODE_SLOTS + 3: return AnimNodeArrayOverflow::SavedEbx;
	case ANIM_UPDATE_NODE_SLOTS + 4: return AnimNodeArrayOverflow::ReturnAddress;
	default: break;
	}
	if (count <= MAX_CLUMP_ANIM_ASSOCS)
		return AnimNodeArrayOverflow::None;
	return AnimNodeArrayOverflow::ClumpArgument;
}

// Does a clump with this many associations write outside the node array at
// all? The question every sweep over somebody else's clump asks.
inline bool AnimNodeArrayOverflows(int count) {
	return count > MAX_CLUMP_ANIM_ASSOCS;
}

// CMatrix::UpdateRW(), __thiscall on the CMatrix (i.e. entity + offs::MATRIX).
// Copies right/up/at/pos into the attached RwMatrix and calls RwMatrixUpdate.
// 0x004B8E00 is CMatrix::AttachRW, which calls it; 0x004B8E50 is
// CMatrix::Update, the other direction; 0x004B8E30 is Detach.
constexpr uintptr_t CMatrix__UpdateRW = 0x004B8EC0;
constexpr uintptr_t CMatrix__Update   = 0x004B8E50;
constexpr uintptr_t CMatrix__AttachRW = 0x004B8E00;

// CEntity::UpdateRwFrame(), __thiscall. RwFrameUpdateObjects on the clump's
// (or atomic's) frame - this is what makes RenderWare recompute the LTM.
constexpr uintptr_t CEntity__UpdateRwFrame = 0x00474330;

// ---- moving an entity the engine already owns -----------------------------
//
// CPhysical::Add / Remove / RemoveAndAdd, in that order in the image, which
// is also re3 Physical.cpp's order (83 / 137 / 148). Add and Remove are slots
// 1 and 2 of CCivilianPed's vtable (0x005F819C), which is how they were
// found. RemoveAndAdd is the function right after Remove and is unmistakable:
// it calls vtable slot 7 (GetBoundRect) into a stack CRect, turns each edge
// into a sector index with `fmul 0.025 / fadd 50` (re3's GetSectorIndexX/Y
// over a 100 x 100 grid of 40-unit sectors covering [-2000, 2000]), and
// walks m_entryInfoList to recycle its nodes.
//
// RemoveAndAdd is the engine's answer to "this entity moved". CPhysical's
// collision and shift paths call it every frame for every entity the physics
// moved (re3 Physical.cpp:1815/1843/1863/1875/1996). An entity whose position
// gets written from outside gets none of that, and stays filed in the sector
// it was added in - invisible, since CRenderer::ScanWorld only walks the
// sectors around the camera.
constexpr uintptr_t CPhysical__Add          = 0x004951F0;
constexpr uintptr_t CPhysical__Remove       = 0x004954B0;
constexpr uintptr_t CPhysical__RemoveAndAdd = 0x00495540;

// The world the sector grid covers. Feed RemoveAndAdd or CWorld::Add a
// position outside it and you index past the end of CWorld::ms_aSectors -
// the retail build has no bounds check (re3's are asserts, and those get
// compiled out). Positions arrive off the wire, so they're clamped before
// they're written (see ClampToWorld in pedanim.h).
constexpr float WORLD_MIN_XY   = -2000.0f;
constexpr float WORLD_MAX_XY   =  2000.0f;
constexpr float SECTOR_SIZE_XY =    40.0f;
constexpr int   NUM_SECTORS_XY =   100;

// CPed::Teleport(CVector), __thiscall, vtable slot 11. Matches re3
// Ped.cpp:6614 statement for statement: CWorld::Remove, SetPosition,
// `[+0x154] &= ~1` (bIsStanding = false), `[+0x220] = 0` (m_nPedStateTimer),
// then CWorld::Add. Recorded as the proof that moving a ped means re-filing
// it, and as the right call for a jump - not something to call per frame,
// since clearing bIsStanding every frame would fight the ped's own ground
// logic.
constexpr uintptr_t CPed__Teleport = 0x004D3E70;

// CMatrix::SetRotate(float x, float y, float z), __thiscall on the CMatrix.
// This is the one COMMAND_CREATE_CHAR calls for SetOrientation(0,0,0):
// `lea ecx,[ebx+4] / push 0 / push 0 / push 0 / call 0x004B93A0`, and the
// handler then adds the saved position back - which is how we know it zeroes
// the position, and what CPlaceable::SetOrientation/SetHeading do around it.
// With x = y = 0 it reduces exactly to re3's SetRotateZOnly: right = (c,s,0),
// up = (-s,c,0), at = (0,0,1). Its neighbours are the SetRotate*Only family:
// 0x004B9160 is SetRotateXOnly (first store is `[ebx] = 1.0f`) and 0x004B9310
// is SetRotateX, which calls it and then zeroes the position.
constexpr uintptr_t CMatrix__SetRotate      = 0x004B93A0;
constexpr uintptr_t CMatrix__SetRotateXOnly = 0x004B9160;

// RenderWare layout, read off the retail image rather than off a header.
//
//   CEntity::CreateRwObject (0x00473EC4) does, for the rpCLUMP case:
//       mov eax,[edx+4] / add eax,10h / lea ecx,[ebp+4] / call CMatrix::AttachRW
//   so RpClumpGetFrame(clump) is the dword at clump+4 (RwObject::parent, which
//   also fixes sizeof(RwObject) at 8), and RwFrameGetMatrix(frame) is frame+0x10
//   (RwObject 8 + RwLLLink 8). Same function tests `byte [clump] == 1` for
//   rpATOMIC and `== 2` for rpCLUMP.
//
//   RwLinkList is one RwLLLink and is its own sentinel, so a clump's atomic
//   list starts at clump+8 and terminates when the walk returns to it.
constexpr size_t RWOBJECT_TYPE      = 0x00;   // uint8
constexpr size_t RWOBJECT_FLAGS     = 0x02;   // uint8
constexpr size_t RWOBJECT_PARENT    = 0x04;
constexpr size_t RWCLUMP_ATOMICLIST = 0x08;
constexpr size_t RWFRAME_MODELLING  = 0x10;
constexpr size_t RWFRAME_LTM        = 0x50;
constexpr size_t RWMATRIX_POS       = 0x30;
constexpr uint8_t RWTYPE_ATOMIC = 1;
constexpr uint8_t RWTYPE_CLUMP  = 2;

// CCollision::ms_collisionInMemory - the level whose collision is loaded.
// CPed::ProcessControl's first two instructions read m_nZoneLevel and compare
// it against this. A ped whose level is neither LEVEL_IGNORE/LEVEL_GENERIC
// nor the loaded one returns before anything else, including the alpha
// fade-in.
constexpr uintptr_t CCollision__ms_collisionInMemory = 0x008F6250;

constexpr uintptr_t CPed__ProcessControl          = 0x004C8910;
constexpr uintptr_t CCivilianPed__ProcessControl  = 0x004BFFE0;

// ---- what CPed::ProcessControl actually does, in its own order ------------
//
// Verified 2026-09-22, walking 0x004C8910 end to end against re3
// Ped.cpp:1693-2364. This block exists because docs/protocol.md §6 asked for
// two years whether a remote ped should be parked in a state "past
// PED_STATES_NO_AI" or have ProcessControl skipped, and the answer to both is
// no. §1.13 is the argument; these are the instructions it rests on.
//
// The function is one straight line with two early returns. In order:
//
//   004C891E  m_nZoneLevel vs ms_collisionInMemory          -> return
//   004C8948  call 00528F70   GetClumpAlpha
//   004C894D  [+0x15A] bit 7  bFadeOut ? alpha-8 : alpha+16, clamped 0..255
//   004C8981  call 00528F50   SetClumpAlpha      <- the ONLY alpha raise
//             bIsShooting = false; BuildPedLists(); bIsInWater = false;
//             ProcessBuoyancy()
//             the PED_DEAD arm (bloody footprints, blood pool) -> return
//             the bWasPostponed arm -> return
//             m_panicCounter; `m_fHealth <= 1.0f && m_nPedState <= 22h
//             && !bIsInTheAir && !bIsLanding -> SetDie(KO_FRONT)`
//             the whole collision-damage block (RAMMEDBYCAR, knockdowns)
//   004CAF61  call 00495F10   CPhysical::ProcessControl
//   004CAF66  cmp state,30h   PED_DIE  -> else arm is SetDead (004D3970)
//   004CAFD0  cmp state,31h   PED_DEAD -> skips the next two
//   004CAFD7  call 004C73F0   CalculateNewVelocity
//   004CAFDE  call 004C7EA0   CalculateNewOrientation
//   004CAFE5  call 004C7A00   UpdatePosition
//   004CAFEC  call 004CC6C0   PlayFootSteps
//   004CAFF3  call 004CE6C0   IsPedInControl -> CheckIfInTheAir/SetInTheAir
//   004CB023  call 004D94E0   ProcessObjective
//   004CB037  call 004C6AA0   AimGun          (bIsAimingGun, [+0x154] bit 7)
//   004CB04C  call 004C6BB0   RestoreGunPosition
//   004CB060  call 004C65B0   MoveHeadToLook  / 004C6930 RestoreHeadPosition
//   004CB08A  call 004D0D10   InTheAir        (bIsInTheAir)
//   004CB09C  bUpdateAnimHeading, skipped for state 24h/25h (FALL/GETUP)
//   004CB0F1  call 004D5D80   Wait            (m_nWaitState at +0x238)
//   004CB0F6  cmp state,1     if (!PED_IDLE) condemn ANIM_STD_IDLE_BIGGUN
//   004CB11B  the state switch (below)
//   004CB788  call [vtable+48h]  SetMoveAnim
//             bPedIsBleeding, ServiceTalking, `bInVehicle && !m_pMyVehicle`
//
// Two entries in that list are the ones CoopIII cannot do without. The
// SetClumpAlpha at 004C8981 is the only thing in the whole engine that raises
// a ped's clump alpha, so a ped that never runs ProcessControl is never drawn
// (the render block above says the same thing from the other end). And
// CPhysical::ProcessControl is what makes a remote ped fall to the ground and
// stand on it between snapshots.
// Each of the names below is pinned twice: by its position in the call
// sequence above, which matches re3 statement for statement, and by its own
// first few instructions. CalculateNewVelocity opens `call IsPedInControl`;
// CalculateNewOrientation and UpdatePosition both open
// `cmp byte [0095CD5Bh],1` (CReplay::IsPlayingBack); AimGun opens by reading
// m_pPointGunAt at +0x30C and switching on its entity type; SetDead opens by
// clearing bUsesCollision and zeroing m_fHealth at +0x2C0; Wait opens with
// DyingOrDead and `mov ebx,0ADh` (ANIM_STD_NUM). PlayFootSteps and InTheAir
// are pinned by position alone and by taking only `this`.
constexpr uintptr_t CPhysical__ProcessControl = 0x00495F10;
constexpr uintptr_t CPed__CalculateNewVelocity    = 0x004C73F0;
constexpr uintptr_t CPed__CalculateNewOrientation = 0x004C7EA0;
constexpr uintptr_t CPed__UpdatePosition          = 0x004C7A00;
constexpr uintptr_t CPed__PlayFootSteps           = 0x004CC6C0;
constexpr uintptr_t CPed__CheckIfInTheAir         = 0x004D0BE0;
constexpr uintptr_t CPed__SetInTheAir             = 0x004D0CA0;
constexpr uintptr_t CPed__InTheAir                = 0x004D0D10;
constexpr uintptr_t CPed__ProcessObjective        = 0x004D94E0;
constexpr uintptr_t CPed__AimGun                  = 0x004C6AA0;
constexpr uintptr_t CPed__RestoreGunPosition      = 0x004C6BB0;
constexpr uintptr_t CPed__Wait                    = 0x004D5D80;
constexpr uintptr_t CPed__SetDead                 = 0x004D3970;
constexpr size_t    PED_VTABLE_SETMOVEANIM        = 0x48;

// ---- AimGun, and where a ped's aim pitch comes from -----------------------
//
// CPed::AimGun (0x004C6AA0) has three arms, and only its one caller -
// 004CB037 in ProcessControl, per a byte scan for E8 rel32 - ever reaches it:
//
//   004C6AA6  mov ecx,[ebx+30Ch] / test / je 004C6B30     m_pSeekTarget?
//   004C6AFC  call 004ED920                               PointGunAtPosition
//   004C6B32  call 004D48E0 / test al,al / je 004C6B70    IsPlayer()?
//   004C6B41  push dword [ebx+5ECh]                       m_fFPSMoveHeading
//   004C6B47  push dword [ebx+4BCh]                       m_fLookDirection
//   004C6B4D  call 004ED9B0                               PointGunInDirection
//   004C6B76  push dword [005F8438h]                      0.0f, for everyone else
//   004C6B7C  push dword [ebx+4BCh]
//   004C6B82  call 004ED9B0
//
// IsPlayer (0x004D48E0) is `mov edx,[ecx+32Ch]` and true for m_nPedType 0..3,
// so a remote player - a CCivilianPed made as PEDTYPE_CIVMALE - takes the
// third arm and gets a level aim whatever its owner is doing.
//
// PointGunInDirection is __thiscall on the CPedIK, (float yaw, float pitch),
// `ret 8`, and returns bCanPointGunAtTarget in al. It is the whole aim, with
// the arm/torso split inside it: `and eax,4` on m_flags picks
// PointGunInDirectionUsingArm (0x004EDB20) for AIMS_WITH_ARM, whose MoveLimb
// takes ms_upperArmInfo at 0x5F9FA4 = {20,-100,20, 70,-70,10} degrees and the
// pitch minus the parent frame's world pitch; otherwise it is MoveLimb on
// m_torsoOrient with ms_torsoInfo at 0x5F9F8C = {50,-50,15, 45,-45,7}. Both
// end in RotateTorso (0x004EDDB0), which is RwMatrixRotate (0x005A2BF0) on the
// PED_MID frame's modelling matrix with rwCOMBINEPOSTCONCAT, then puts the
// frame's position back. No RpHAnim - III's peds are a plain frame hierarchy.
// Three callers in the image: AimGun's two arms and PointGunAtPosition at
// 004ED9A1. Nothing else bends a ped's arms toward a pitch.
//
// The player's pitch is CPlayerPed::m_fFPSMoveHeading at +0x5EC. Written in
// four places: CPed::SetAttack's `call 0046B850 / fstp [ebp+5ECh]` at
// 004E6570 (TheCamera.Find3rdPersonQuickAimPitch, only when the attacker is
// FindPlayerPed() and Cams[0] is the 3rd-person mouse camera), and zeroed by
// ClearAimFlag (004C6A90), RestoreGunPosition (004C6C01) and the run of
// CPlayerPed member zeroes at 004EF968. Find3rdPersonQuickAimPitch ends on
// `fchs`, so the convention is positive = down. CREATE_CHAR allocates 0x53C
// for a ped, so on a remote player this offset is past the end of the object.
constexpr uintptr_t CPedIK__PointGunInDirection = 0x004ED9B0;
constexpr size_t    PEDIK_PED                   = 0x00;    // CPed *m_ped
constexpr size_t    PLAYERPED_FPS_MOVE_HEADING  = 0x5EC;   // float, CPlayerPed only

// ---- the per-state switch, and the whole ePedState enum with it -----------
//
//   004CB11B  mov eax,[ebx+224h]          m_nPedState
//   004CB127  dec eax
//   004CB128  cmp eax,36h
//   004CB12D  ja  004CB9F0                the default arm
//   004CB133  jmp [eax*4 + 005F8778h]
//
// So the table covers states 1..55 and everything else - 0, and anything
// from 56 up - takes the default. The default arm at 004CB9F0 is two
// instructions, `fstp st(0) / jmp 004CB784`, and 004CB784 is the
// `call [vtable+48h]` above. **Taking the default does not skip anything
// except the per-state function.**
//
// Twenty of the fifty-five table entries already point at that same default,
// which is what dates the whole "park the state past PED_STATES_NO_AI" idea:
// PED_STATES_NO_AI is 34, its own table entry is the default arm, and the
// name means "states above this one are not the ped's own decision", not
// "the AI stops running here". re3 only ever uses it as the bound in
// IsPedInControl, CanPedReturnToState and the health<=1 auto-SetDie.
//
// Reading the table off the image also confirms re3's enum against retail,
// which is worth having written down after ANIM_STD_NUM came out one short:
constexpr uintptr_t g_PedStateSwitchTable = 0x005F8778;   // indexed state-1
constexpr uint32_t  PEDSTATE_TABLE_MAX    = 55;           // `cmp eax,36h`
constexpr uint32_t PEDSTATE_LOOK_ENTITY    = 2;    // 004CB6B6, shared with 3
constexpr uint32_t PEDSTATE_WANDER_RANGE   = 4;    // 004CB5AF
constexpr uint32_t PEDSTATE_WANDER_PATH    = 5;    // 004CB6E0
constexpr uint32_t PEDSTATE_ATTACK         = 16;   // 004CB639
constexpr uint32_t PEDSTATE_FIGHT          = 17;   // 004CB647
constexpr uint32_t PEDSTATE_AIM_GUN        = 22;   // 004CB655
constexpr uint32_t PEDSTATE_STATES_NO_AI   = 34;   // default arm; the bound
                                                   // IsPedInControl uses
constexpr uint32_t PEDSTATE_FALL           = 36;   // 004CB6C4, and the 24h in
                                                   // the bUpdateAnimHeading
                                                   // test above
constexpr uint32_t PEDSTATE_GETUP          = 37;   // 004CB1BC -> 004D0F20
constexpr uint32_t PEDSTATE_STATES_NO_ST   = 40;   // IsPedShootable's bound
constexpr uint32_t PEDSTATE_HANDS_UP       = 55;   // 004CB13A, the last entry
// PEDSTATE_ON_FIRE (32, 004CB707), PEDSTATE_DRIVING (44, 004CB1D8),
// PEDSTATE_DIE (48, 004CB62B) and PEDSTATE_DEAD (49, default) are declared
// with the features that write them, above and below.

// CPed::IsPedShootable has no address here on purpose: the retail build has
// no out-of-line copy of it - `cmp dword [ecx+224h],28h` appears nowhere in
// the image - so it is inlined at each of its three call sites. Recorded
// only to say what it is NOT. All three sites are AI target selection
// (BuildPedLists, the KILL_CHAR objective, CPlayerPed's lock-on), and
// nothing in the damage path calls it, so a ped parked above 40 would stop
// being *aimed at* by the engine's own peds and would still take every
// bullet. It is not a damage gate and must not be used as one.

// __thiscall void CPed::Idle() - the switch's PED_IDLE arm, and therefore
// the only AI a remote player's ped runs today. Two things in it matter, and
// both were measured rather than assumed:
//
//   004D077E  cmp  dword [ebx+22Ch], 1      m_nMoveState == PEDMOVE_STILL
//   004D0789  je   004D07C1                 -> the ANIM_STD_IDLE_BIGGUN arm
//   004D078F  [ebp+1Ch] = 0C1000000h        armedIdle->blendDelta = -8.0f
//   004D0796  [ebp+30h] |= 4                ASSOC_DELETEFADEDOUT
//   004D07A6  call 004D48E0                 CPed::IsPlayer()
//   004D07AD  jne  004D0949                 ...and a player is left alone
//   004D07B5  push 1 / call 004C5A30        SetMoveState(PEDMOVE_STILL)
//
// 1. It is the same `if (!IsPlayer())` shape as CFireManager::StartFire's
//    burning-ped block: GTA III's own way of saying "somebody else decides
//    where this one goes". There are more of these than the fire one.
// 2. Because it runs *before* SetMoveAnim in the same frame, the move state
//    CoopIII writes from PreFrame never survives to be read. Writing
//    m_nMoveState and letting SetMoveAnim pick the walk has therefore never
//    once worked on a remote ped; remote players walk because ApplyAnimation
//    blends the wire's animId directly. docs/protocol.md §1.13.
//
// The STILL arm is also one of the two engine sources of animations on a
// remote ped's clump: it adds ANIM_STD_IDLE_BIGGUN (id 0Ah) on a 3000..8500ms
// random timer. The other is CPed::SetInTheAir, which blends
// ANIM_STD_FALL_GLIDE (id 98h) the moment CheckIfInTheAir finds no ground
// under a ped whose position came off the wire.
constexpr uintptr_t CPed__Idle = 0x004D0690;

// __thiscall void CPed::ProcessObjective() - the other half of the AI, and
// the half that made a remote ped walk back to a car it had been taken out
// of. Its guard is the reason clearing the objective was enough:
//
//   004D94EC  [+0x15Bh] bit 3               bClearObjective
//   004D94F9  call 004CE6C0 / cmp state,2Ch IsPedInControl() || PED_DRIVING
//   004D950D  call 004D8DF0                 ClearObjective
//   004D9522  call 004D8F30                 UpdateFromLeader
//   004D9527  cmp dword [ebx+164h], 0       m_objective != OBJECTIVE_NONE
//   004D952E  je  004D9544                  ...or the whole body is skipped
//
// So a ped holding OBJECTIVE_NONE decides nothing here, whatever its state
// is and whether or not ProcessControl runs. That is the off switch, and it
// costs one dword.
constexpr uintptr_t CPed__ClearObjective   = 0x004D8DF0;
constexpr uintptr_t CPed__UpdateFromLeader = 0x004D8F30;

// ---- putting a ped in a vehicle -------------------------------------------
//
// Verified 2026-09-21 (M2). There's no single engine export for "seat this
// ped" - the two directions live in two script opcode handlers, so both got
// disassembled and both are transcribed below.
//
// How they were found: the dispatcher at CRunningScript__ProcessCommands is a
// chain of `cmp dx,<limit> / jge / movsx eax,dx / push eax / call <range>`.
// The 800..899 link is `cmp dx,384h` at 0x004395F6 with its call at
// 0x00439601, resolving to the range handler below. That handler opens
// `lea eax,[ebp-320h]` (base 800), `cmp eax,63h`, `jmp [eax*4 + 5EF77Ch]`, so
// its jump table is the second constant. Entry 74 is
// COMMAND_WARP_CHAR_INTO_CAR (opcode 874) and entry 66 is
// COMMAND_WARP_CHAR_FROM_CAR_TO_COORD (866).
constexpr uintptr_t CRunningScript__ProcessCommands800To899 = 0x00448240;
constexpr uintptr_t g_ScriptOpcodeTable_800                 = 0x005EF77C;

// ---- getting in ----
//
// WARP_CHAR_INTO_CAR's handler (0x0044BB7C) is five calls long:
//
//   CollectParameters(script, 2)
//   ped     = CPool<CPed>::GetAt    (ms_pPedPool,     ScriptParams[0])
//   vehicle = CPool<CVehicle>::GetAt(ms_pVehiclePool, ScriptParams[1])
//   ped->SetObjective(OBJECTIVE_ENTER_CAR_AS_DRIVER, vehicle)
//   ped->WarpPedIntoCar(vehicle)
//
// The SetObjective call is not ceremony - it's the one thing about this pair
// that's easy to get wrong. WarpPedIntoCar branches on m_objective: at
// 0x004D7D94 it does `mov eax,[ebp+164h] / cmp eax,0Fh`, and the else arm
// compares against 0Eh. Call it with anything else and it still sets
// bInVehicle and PED_DRIVING, then returns having assigned no seat at all -
// leaving a ped who thinks it's in a car that has never heard of it. Set the
// objective first.
//
// What WarpPedIntoCar itself does, in order: bInVehicle = true, m_pMyVehicle
// and m_carInObjective = car (each with a RegisterReference), PED_DRIVING,
// bUsesCollision = false, bIsInTheAir = false, then either SetDriver(this) or
// the first free passenger slot. For a non-player, the car's status then
// becomes STATUS_PHYSICS and the ped gets taken out of the world, moved to
// the car's position, and put back. The engine positions it in the seat
// every frame after that, so a seated remote ped has to stop being written
// to (client/src/game/ped.cpp, ApplyRemotePose).
constexpr uintptr_t CPed__SetObjective   = 0x004D83E0;   // (eObjective, void*)
constexpr uintptr_t CPed__WarpPedIntoCar = 0x004D7D20;   // (CVehicle*)

// Called by WarpPedIntoCar's driver arm. Recorded both as the proof of that
// arm and because M2's next piece needs the handoff directly.
constexpr uintptr_t CVehicle__SetDriver = 0x00551F20;   // (CPed*)

// __thiscall void CEntity::RegisterReference(CEntity **ref)  [ret 4]
//
// An entity keeps a list of the pointers other objects hold to it and nils
// every one of them when it is destroyed, which is the engine's own answer
// to the crash class that has bitten this project twice. It is why
// despawning a seated ped is safe: WarpPedIntoCar registers &pDriver, so
// CWorld::RemoveReferencesToDeletedObject (which the despawn already calls)
// nils the car's pointer to the ped on its way out.
//
//   004A7480  [ecx+50h] & 7 == 1 -> return            buildings never move
//   004A7494  walk [ecx+60h], comparing node->ref     already registered?
//   004A74B6  take a node from [0x008F1AF8] and link it
//
// CoopIII calls it directly in one place: the fire seam registers a fire's
// &m_pEntity and &m_pSource the way CFireManager::StartFire does, so a
// burning remote ped that gets deleted leaves the fire pointing at nothing
// rather than at a freed pool slot (see the phase-three block below).
constexpr uintptr_t CEntity__RegisterReference = 0x004A7480;

// eObjective. Only these three, because only these three are named by the
// retail image itself: 0Fh from the `push 0Fh` in the handler above, 0Eh
// from WarpPedIntoCar's else arm, 0 from SetObjective's own early-out.
constexpr uint32_t OBJECTIVE_NONE                   = 0;
constexpr uint32_t OBJECTIVE_ENTER_CAR_AS_PASSENGER = 14;
constexpr uint32_t OBJECTIVE_ENTER_CAR_AS_DRIVER    = 15;

// PedState values, from the two handlers and SetObjective's guard.
// `cmp eax,30h` / `cmp eax,31h` against PED_STATE is re3's DyingOrDead; the
// warp writes 2Ch where re3 has PED_DRIVING; the exit writes 1 into
// PED_STATE and 0 into PED_LAST_STATE where re3 has PED_IDLE and PED_NONE.
constexpr uint32_t PEDSTATE_NONE    = 0;
constexpr uint32_t PEDSTATE_IDLE    = 1;
constexpr uint32_t PEDSTATE_DRIVING = 44;
constexpr uint32_t PEDSTATE_DIE     = 48;
constexpr uint32_t PEDSTATE_DEAD    = 49;

// ---- getting out ----
//
// There's no WarpPedOutOfCar - the sequence is open-coded in
// WARP_CHAR_FROM_CAR_TO_COORD's handler at 0x0044B346, and this is it, taken
// instruction by instruction (matches re3 Script4.cpp:761-801):
//
//   if (!bInVehicle) goto done
//   if (car->bIsBus) ped->bRenderPedInCar = true
//   if (car->pDriver == ped) {
//       car->RemoveDriver()
//       car->SetStatus(STATUS_ABANDONED)      ; [+0x50] &= 7, |= 20h
//       car->bEngineOn = false                ; [+0x1F5] &= 0EFh
//       car->AutoPilot.m_nCruiseSpeed = 0
//   } else
//       car->RemovePassenger(ped)
//   car->m_vecMoveSpeed = (0, 0, -0.00001f)   ; the literal is 0B727C5ACh
//   car->m_vecTurnSpeed = (0, 0, 0)
//   done:
//   ped->bInVehicle = false; ped->m_pMyVehicle = nil
//   ped->m_nPedState = PED_IDLE; ped->m_nLastPedState = PED_NONE
//   ped->bUsesCollision = true
//   ped->m_vecMoveSpeed = (0, 0, 0)
//   ped->AddWeaponModel(current weapon's model)
//   ped->RemoveInCarAnims()
//   if (ped->m_pVehicleAnim) ped->m_pVehicleAnim->blendDelta = -1000.0f
//   ped->m_pVehicleAnim = nil
//   ... then Teleport to the coordinate the opcode was given.
//
// The two calls in it: RemoveDriver is four instructions (SetStatus plus
// `pDriver = nil`), and RemovePassenger walks pPassengers looking for this
// ped - eight slots for a train, m_nNumMaxPassengers otherwise - which
// re-confirms VEH_PASSENGERS, VEH_NUM_PASSENGERS, VEH_NUM_MAX_PASSENGERS and
// VEH_TYPE == 2 meaning train, all in one function.
constexpr uintptr_t CVehicle__RemoveDriver    = 0x005520A0;   // ()
constexpr uintptr_t CVehicle__RemovePassenger = 0x00551EB0;   // (CPed*)

// CPed::RemoveInCarAnims. Recorded to explain why CoopIII doesn't call it:
// its first two instructions are `call CPed::IsPlayer / test al,al / je ret`,
// so on a CCivilianPed - which every remote player is - it does nothing at
// all. The animations it tears down only ever exist on the local player.
constexpr uintptr_t CPed__RemoveInCarAnims = 0x004E4E20;

// The car's downward nudge on exit, so it settles onto its suspension
// instead of hanging exactly where the driver left it. The handler writes
// the bit pattern 0B727C5ACh, which is this value exactly.
constexpr float VEH_EXIT_SETTLE_SPEED_Z = -0.00001f;

// ---- getting in with the door open ----------------------------------------
//
// The two above put a ped in a seat in one instruction. These four are the
// engine's real thing: a ped walks to a door, opens it, climbs in, and a
// chain of animation-finish callbacks ends with the seat being assigned.
// The whole point of them is that they take the best part of a second, so
// nothing here is a call you make and then read the answer to.
//
// None of these are exported and none are reachable from a script opcode, so
// each one was found by the state it writes. CPed::m_nPedState is at 0x224
// (already recorded above) and the three states involved appear as an
// immediate exactly once each in the whole image:
//
//   mov dword [reg+224h], 32h   -> 0x004E04AD   PED_CARJACK   (50)
//   mov dword [reg+224h], 34h   -> 0x004E0B35   PED_ENTER_CAR (52)
//   mov dword [reg+224h], 36h   -> 0x004E14B3   PED_EXIT_CAR  (54)
//
// Each sits inside a function that then matches re3 statement for statement;
// the function starts below are the `53 56 57 55` prologues above them.
//
// __thiscall void CPed::SetEnterCar(CVehicle *car, uint32 unused)   [ret 8]
//
//   004E0920  push ebx/esi/edi/ebp                      function start
//   004E092E  call 00545190   CCranes::IsThisCarBeingCarriedByAnyCrane
//   004E0950  movzx eax, word [ebx+2E8h]                m_vehDoor
//   004E095F  jmp [eax*4 + 5F93B0h]                     the door switch
//   004E09A6  call 004CE6C0   CPed::IsPedInControl      (already verified)
//   004E09AF  fld [ebx+2C0h] / fcomp 0                  m_fHealth > 0
//   004E09C2  [ebp+1CAh] & doorFlag                     m_nGettingInFlags
//   004E09CD  [ebp+1CBh] & doorFlag                     m_nGettingOutFlags
//   004E09D8  [ebp+1F7h] bit 4                          bIsBeingCarJacked
//   004E0A07  [ebx+1D8h] != 0                           m_pVehicleAnim
//   004E0A1C  call 004E0A40                             SetEnterCar_AllClear
//   004E0A25  call 004C5A30 (push 1)                    SetMoveState(STILL)
//
// It refuses far more often than it accepts, and every refusal is silent -
// the ped just stops walking. That is the whole reason the seating loop
// treats this as an attempt with a deadline rather than as a command.
constexpr uintptr_t CPed__SetEnterCar = 0x004E0920;   // (CVehicle*, uint32)

// __thiscall void CPed::SetCarJack(CVehicle *car)   [ret 4]
//
//   004E0220  push ebx/esi/edi/ebp                      function start
//   004E022B  [ebx+284h] == 1 -> return                 car->IsBoat()
//   004E0255  jmp [eax*4 + 5F9380h]                     the same door switch,
//                                                       also picking the ped
//                                                       in the seat being taken
//   004E0312  call 004D48E0 (ecx = esi)                 CPed::IsPlayer
//   004E0319  test al,al / jne 004E0347                 a player SKIPS the
//                                                       three gates below
//   004E031B  [esi+164h] == 8 or == 7 -> 004E0347        m_objective
//   004E032B  [ebx+1F4h] == 2 -> return                 VehicleCreatedBy ==
//                                                       MISSION_VEHICLE
//   004E0334  [ebx+5Ch] != 7Eh -> 004E0347               m_nModelIndex
//
// **The MISSION_VEHICLE bail is behind the IsPlayer test, not in front of
// it.** An earlier note here said SetCarJack "returns without doing anything"
// on a MISSION_VEHICLE full stop, and used that to explain why CoopIII does
// not play the jack animation. Half of that is wrong and the wrong half is
// the one somebody would act on: `jne` at 004E0319 jumps over the whole guard
// block for anything IsPlayer() says yes to, so the local player pressing F
// on a session car is not affected by it at all. What the bail does block is
// a *remote* ped - every one CoopIII builds is a CCivilianPed - from jacking
// an occupied session car, which is the case the seating loop cares about.
//
// Read out of the retail image rather than from re3: the three `jne`/`je`
// targets above all land on 004E0347, and 004D48E0 is the CPed::IsPlayer this
// file already carries.
//
// Recorded, and deliberately not called - see the comment on the jack in
// Client::OnEnterVehicle. The reason it is not called is the second one
// given there, about an observer deciding that somebody else left a car;
// the gate above is only why it would not have worked anyway.
constexpr uintptr_t CPed__SetCarJack = 0x004E0220;   // (CVehicle*)

// __thiscall void CPed::QuitEnteringCar(void)   [ret]
//
//   004E0E00  push ebx / mov ebx,ecx                    function start
//   004E0E14  m_pVehicleAnim->blendDelta = -1000.0f
//   004E0E1D  call 004C5D80                             RestartNonPartialAnims
//   004E0E28  call 004055C0 (push 3)                    get ANIM_STD_IDLE
//   004E0E46  call 00403710                             blend it if missing
//   004E0E5B  m_nPedState == 32h -> car->bIsBeingCarJacked = false
//   004E0E7B  dec [ebp+1C9h]                            m_nNumGettingIn--
//   004E0E90  jmp [eax*4 + 5F93C8h]                     m_nGettingInFlags &= ~flag
//   004E0EBE  [ebx+51h] |= 1                            bUsesCollision = true
//   004E0F78  m_pVehicleAnim = nil
//
// This is the undo, and it is not optional. A ped that is dropped out of an
// entry without it leaves the car's m_nGettingInFlags holding that door and
// m_nNumGettingIn counting somebody who is not coming: the door is then
// refused to everybody, for the rest of the car's life, because that flag is
// the first thing SetEnterCar tests.
constexpr uintptr_t CPed__QuitEnteringCar = 0x004E0E00;

// __thiscall void CPed::EnterCar(void)   [ret]
//
// The per-frame half of an entry, and the answer to "who moves the door".
// Recorded because the seat sync hinges on it: **a car's door is swung by the
// entering ped's own animation, on whichever machine is running that ped, and
// nothing about an open door exists anywhere else.**
//
//   004E0D30  push ebx/esi / mov ebx,ecx / push ebp     function start
//   004E0D38  mov eax,[ebx+310h]                        m_pMyVehicle
//   004E0D48  [eax+50h] >> 3 == 5 -> bail               STATUS_WRECKED
//   004E0D56  fld [ebx+2C0h] / fcomp 0                  m_fHealth > 0
//   004E0D71  call 004E0E00 / call 004D37D0             QuitEnteringCar, SetDie
//   004E0D40  mov esi,[ebx+30Ch]                        m_pSeekTarget = the car
//   004E0D94  movzx from word [ebx+2E8h]                m_vehDoor (a word again)
//   004E0DA5  call 004E1A30                             GetPositionToOpenCarDoor
//   004E0DB0  mov ecx,esi / push ebx / call 005522A0    car->CanPedOpenLocks(this)
//   004E0DC5  mov eax,[ebx+1D8h] / test -> skip          m_pVehicleAnim
//   004E0DCF  mov edx,[eax+2Ch] / push [eax+20h]        anim->animId, currentTime
//   004E0DDE  call dword ptr [ebp+5Ch]                  CVehicle::ProcessOpenDoor
//   004E0DE1  [ebx+155h] &= ~8                          bIsInTheAir = false
//   004E0DF3  call 004DF940 (push 0)                    LineUpPedWithCar(START)
//
// Two things follow, and both were needed to explain a door that opened on
// one screen and not the other:
//
//  - the door is written by a virtual call on the *car*, made out of the
//    *ped's* animation, every frame, from CWorld::Process's walk over the
//    moving list. So a replica ped that is genuinely animating into a car
//    does move that car's real door on the machine that owns the car. There
//    is nothing to send and nothing to hook; the entry has to start, and it
//    has to start while there is still an entry left to play.
//  - QuitEnteringCar makes no call on the car at all (there is no
//    `call [reg+5Ch]` anywhere in 004E0E00..004E0F96). It clears
//    m_nGettingInFlags and leaves the door wherever the animation had got it
//    to. An entry abandoned after the door-opening animation has run
//    therefore leaves that door standing open with nobody in it, and only
//    somebody else's get-in or get-out will ever shut it again.
//
// It also independently re-confirms three offsets this file already had:
// PED_SEEK_TARGET 0x30C, ANIM_ID 0x2C and ANIM_CURRENT_TIME 0x20.
constexpr uintptr_t CPed__EnterCar               = 0x004E0D30;
constexpr uintptr_t CVehicle__CanPedOpenLocks    = 0x005522A0;   // (CPed*) -> bool
constexpr uintptr_t CPed__GetPositionToOpenCarDoor = 0x004E1A30;

// CPed::PedAnimAlignCB, the finish callback SetEnterCar_AllClear hangs on the
// align animation (`push 4DE130h / call 00401820` at 004E0D17). It is what
// blends the door-opening animation and hands it its own callback, so the
// chain from SetEnterCar to an open door is: align -> this -> open-door
// animation -> the per-frame EnterCar above.
constexpr uintptr_t CPed__PedAnimAlignCB = 0x004DE130;

// CVehicle::ProcessOpenDoor's slot in the vehicle vtable, as EnterCar calls
// it. A byte offset, not an index: `call dword ptr [ebp+5Ch]`.
constexpr size_t VEH_VT_PROCESS_OPEN_DOOR = 0x5C;

// ---- the animation id that SHUTS a door -----------------------------------
//
// Wanted for one thing: an entry that is abandoned after the door-opening
// animation has run leaves that door standing open with nobody in it, because
// QuitEnteringCar makes no call on the car (the note above). One
// ProcessOpenDoor with a closing animation and a time past its end puts it
// back, and this is that animation. It was not guessed; here is the whole
// chain, read out of the retail image on 2026-09-23.
//
// **The engine's own closing call.** CPed::PedAnimDoorCloseCB, the finish
// callback of the shut-the-door animation, at 0x004DF234:
//
//   004DF234  [ebp+1F6h] bit 1                          veh->bIsBus
//   004DF240  movzx eax, word [ebx+2E8h]                m_vehDoor
//   004DF24B  push [005F8CB8h]                          1.0f (read: 0x3F800000)
//   004DF251  push 5Ch                                  the animation id
//   004DF254  call dword [esi+5Ch]                      ProcessOpenDoor
//   004DF257  switch on m_vehDoor - 0Bh                 the door it names
//
// so the engine passes **0x5C** and a time of 1.0f to close a door, whichever
// door it is - the side comes from the component, not from the id. The open
// half of the same chain is one instruction shorter and sits at 0x004DE7B1
// with `push 56h`, which is the matching ANIM_STD_CAR_OPEN_DOOR_LHS.
//
// **And what 0x5C does when it gets there.** CAutomobile::ProcessOpenDoor is
// CAutomobile's vtable slot 0x5C (0x00600C1C + 0x5C reads 0x0052E910). It
// switches on `anim - 52h` through a 92-entry table at 0x00600820:
//
//   entries 0x52, 0x56, 0x65       -> 0x0052EA50   open,  0.66f .. 0.8f
//   entries 0x5C, 0x5D, 0x6B, 0x6C -> 0x0052EB6D   close, 0.2f  .. 0.63f
//
// Four ids share the closing arm, which is exactly the set re3 groups as
// {CLOSE_DOOR_LHS, CLOSE_DOOR_LO_LHS, CLOSE_DOOR_RHS, CLOSE_DOOR_LO_RHS}, and
// three share the opening one, exactly {QUICKJACK, OPEN_DOOR_LHS,
// OPEN_DOOR_RHS}. The two constants read 0.2f at 0x006004FC and 0.63f at
// 0x0060056C. Past the second of those the arm at 0x0052EBE0 does:
//
//   0052EBE4  fcomp [0060056Ch]                          time > 0.63f
//   0052EBF7  push [006004F8h]                           0.0f
//   0052EC01  call dword [ebp+58h]                       OpenDoor(comp, door, 0)
//
// so ProcessOpenDoor(door, 0x5C, 1.0f) is "shut that door, now", by the same
// call the engine makes at the end of every get-in. It is also safe on a door
// that is already shut: OpenDoor is handed a ratio of 0 either way.
constexpr uint16_t ANIM_STD_CAR_OPEN_DOOR_LHS  = 0x56;   // 86
constexpr uint16_t ANIM_STD_CAR_CLOSE_DOOR_LHS = 0x5C;   // 92

// ---- which door a driver's entry actually uses ----------------------------
//
// __thiscall void CPed::GetNearestDoor(CVehicle *veh, CVector *out)
//
// Recorded because it refutes the thing everybody assumes about the enter
// key: **a driver does not walk round the car.** CPed::SeekCar's non-passenger
// arm, at 0x004D40F5:
//
//   004D40F5  cmp word [ebx+2E8h], 0                    m_vehDoor == 0, or
//   004D4103  cmp eax, 0Fh / je 004D4181                m_objective == 15
//                                                       (ENTER_CAR_AS_DRIVER)
//   004D4189  call 004E1CF0                             -> GetNearestDoor
//   004D410A  call 004E4D90 ... 004D4121 call 004E1A30  the jack arm instead
//
// and GetNearestDoor compares the four door positions by squared distance and
// writes the winner straight into m_vehDoor (`mov word [esi+2E8h],10h` at
// 0x004E2199 for the rear-left, `0Ch` at 0x004E21E6 for the rear-right, and
// the front pair above them). So pressing the enter key on the passenger side
// opens the *near* door; CPed::PedAnimDoorCloseCB's third arm then blends
// ANIM_STD_CAR_SHUFFLE_RHS and the ped slides across to the wheel inside the
// car. There is no walk-around to replicate, and the seat does not name the
// door - which is why docs/protocol.md §1.14.7 puts the door on the wire.
//
// CoopIII does not call this. It is here because the entry seam is otherwise
// impossible to reason about, and because the first guess about it was wrong.
constexpr uintptr_t CPed__GetNearestDoor = 0x004E1CF0;   // (CVehicle*, CVector*)

// ---- and how fast a car may be moving, for real ---------------------------
//
// CVehicle::CanPedEnterCar above gates on sq(0.2f) before the walk. The
// door-opening callback gates again, on the same number, *after* it - and
// this second one is the expensive one, because failing it does not refuse
// the entry, it knocks the ped over. At 0x004DE752, immediately before the
// ProcessOpenDoor quoted above:
//
//   004DE74C  fsqrt                                     |m_vecMoveSpeed|
//   004DE758  fcomp [005F8D50h]                         = 0.2f  (0x3E4CCCCD)
//   004DE763  jne 004DE7A0                              under: carry on
//   004DE769  call 004E0E00                             QuitEnteringCar
//   004DE78D  push 3E8h / call 004D09B0                 SetFall(1000, ...)
//
// 0.2 m/s, on the magnitude rather than on its square. So a pre-check looser
// than sq(0.2f) buys nothing: a car creeping at 0.3 m/s passes it, the ped
// walks up, and the engine drops him in the road a second later.
constexpr float PED_ENTER_MAX_SPEED_SQ = 0.04f;   // = sq(0.2f), and VEH_ENTER_MAX_SPEED_SQ

// __thiscall void CPed::SetExitCar(CVehicle *veh, uint32 wantedDoorNode) [ret 8]
//
//   004E1010  push ebx/esi/edi/ebp / sub esp,88h        function start
//   004E1059  call 005523C0                             veh->CanPedExitCar()
//   004E1090  m_nPedState 36h or 33h -> return          already leaving
//   004E10AA  zeroes [ebp+78h..8Ch]                     move and turn speed
//   004E10F7  pDriver     == this -> door 0Fh
//   004E1106  pPassengers[0] == this -> door 0Bh
//   004E1115  pPassengers[1] == this -> door 10h
//   004E1124  pPassengers[2] == this -> door 0Ch
//
// Pass 0 for the door and it works out which one from the seat the ped is
// actually in, which is why nothing on the exit path has to know about doors.
constexpr uintptr_t CPed__SetExitCar = 0x004E1010;   // (CVehicle*, uint32)

// ---- why a player can become unable to get OUT of a car -------------------
//
// The mirror of CanPedEnterCar above, and it matters more, because a car you
// cannot get into is a car you walk away from and a car you cannot get out of
// is the rest of the session.
//
// **There is exactly one call site of CPed::SetExitCar in the whole image**
// (0x004DA157, in CPed::ProcessObjective's OBJECTIVE_LEAVE_CAR arm), it is not
// virtual, and it is where every exit in the game goes - the player's included,
// since the exit key sets that objective. And its first act is:
//
//   004E1057  mov ecx,esi / call 005523C0        veh->CanPedExitCar()
//   004E105E  test al,al / jne 004E1090          true -> get on with the exit
//   004E1062  mov ecx,[esi+1A4h] / test / je     false -> pDriver...
//   004E106C  call 004D48E0                              ...->IsPlayer()?
//   004E1075  mov byte [esi+164h],0                      AutoPilot cruise = 0
//   004E107C  mov byte [esi+15Ah],0
//   004E1083  add esp,88h / pop / ret 8                  and RETURN
//
// So a false answer is not "exit differently". It is "no exit", silently, with
// the ped left sitting exactly where it was, and the only thing the function
// does on that arm is stop an AI *driver* (the IsPlayer test at 004E106C is on
// the car's driver, and it skips the write when the driver is the player).
// There is no timeout and nothing retries: the player presses the key, nothing
// happens, and nothing ever will until the car's own numbers change.
//
// __thiscall bool CVehicle::CanPedExitCar(void), disassembled 2026-09-23:
//
//   005523C0  mov edx,ecx / lea eax,[edx+24h] / three movs   GetUp()
//   005523D3  fld [esp+0Ch]                                  up.z
//   005523D7  fcom [0060258Ch]  = +0.1f    ; greater -> 005524D0
//   005523F4  fcom [00602590h]  = -0.1f    ; less    -> 005524D0
//                            ; so the fallthrough is |up.z| <= 0.1, the car
//                            ; on its side, and 005524D0 is every other pose
//   00552405  lea eax,[edx+78h]  MagnitudeSqr(m_vecMoveSpeed)
//   00552426  fcomp [00602550h] = 0.005f   ; greater -> return false
//   0055243A  [edx+84h] fabs / fcom [00602548h] = 0.01f  ; greater -> false
//   00552464  [edx+88h] the same
//   0055248E  [edx+8Ch] the same
//   005524B8  mov al,1 / ret                             ; otherwise true
//   005524D0  the same four tests again, same two constants, same answers
//
// Both arms end up asking for the same thing, so the orientation branch buys
// nothing here and the rule is simply: **a car may be got out of only when
// |m_vecMoveSpeed|^2 <= 0.005f and every component of m_vecTurnSpeed is within
// 0.01f of zero.** The two arms differ on nothing but the boundary - the
// |up.z| <= 0.1 one tests `and ah,5 / cmp ah,1` and so wants strictly less,
// the other tests `test ah,45h` and lets equality through - which is a
// distinction no float coming out of a physics step will ever land on.
//
// The constants were read out of the file at the addresses
// above (0x3BA3D70A and 0x3C23D70A), not derived, and the up.z pair is the same
// +-0.1f CanPedEnterCar uses.
//
// **The number to remember is that 0.005 is eight times tighter than the 0.04
// on the way in.** game/vehicle.cpp's RestRemoteVehicle exists because a car
// held at a departed driver's velocity fails CanPedEnterCar forever; a car held
// at a *fraction* of that velocity still passes the entry gate and fails this
// one. So anything in CoopIII that writes m_vecMoveSpeed or m_vecTurnSpeed onto
// a car the local player is sitting in does not merely make it drive oddly - it
// takes the exit key away, and the player has no way to tell those two apart.
constexpr uintptr_t CVehicle__CanPedExitCar = 0x005523C0;   // () -> bool
constexpr float     VEH_EXIT_MAX_SPEED_SQ   = 0.005f;
constexpr float     VEH_EXIT_MAX_TURN       = 0.01f;

static_assert(VEH_EXIT_MAX_SPEED_SQ < VEH_ENTER_MAX_SPEED_SQ,
              "getting out of a car is stricter about its speed than getting "
              "in - 0.005f against sq(0.2f)");

// ---- when the engine itself stops believing a thing is moving --------------
//
// The other half of the two gates above, and the one that decides when a car
// nobody is driving may be handed back to the pinned world
// (protocol.h, S_VehicleCustody). A custodian that calls rest too early leaves
// a car pinned mid-fall, which is the bug it exists to fix arrived at from the
// other side; one that never calls it streams a parked car for ever.
//
// The answer is not CoopIII's to invent, because GTA III already answers it on
// the same machine about the same object. Inside CPhysical::ProcessControl
// (0x00495F10), disassembled 2026-09-23:
//
//   00496085  avg move speed squared  fcompp against (0.006f * CTimer step)^2
//   00496095  jne 00496172                        ; moving  -> reset
//   004960B3  avg turn  speed squared  fcompp against the same
//   004960D1  jne 00496172                        ; turning -> reset
//   004960D7  inc byte [ebx+0EDh]                 m_nStaticFrames
//   004960DD  cmp byte [ebx+0EDh],0Ah             ten
//   004960E4  jbe 00496179                        ; <= 10, carry on moving it
//   004960EA  mov byte [ebx+0EDh],0Ah             clamp
//   004960F1  [ebx+51h] and 0FBh / or 4           bIsStatic = true
//   004960FB  m_vecMoveSpeed = 0,0,0              (+0x78, +0x7C, +0x80)
//   00496130  m_vecTurnSpeed = 0,0,0              (+0x84, +0x88, +0x8C)
//   00496170  ret                                 WITHOUT ApplyMoveSpeed
//   00496172  mov byte [ebx+0EDh],0               any moving frame resets it
//
// Three facts worth having separately:
//
//  1. **Rest is a run, not an instant.** Eleven consecutive quiet frames, and
//     a single moving one puts the counter back to zero. A car at the top of a
//     bounce has a velocity near zero for one frame and is not at rest, which
//     is precisely the reading a single-frame test would get wrong.
//  2. **The engine zeroes the velocities itself** when it decides, so a car
//     the engine has put to sleep passes both gates above by construction -
//     there is nothing left for CoopIII to write.
//  3. **bIsStatic at +0x51 bit 2 is the engine's own published answer**, which
//     is why `VehicleAtRest` in game/vehicle.cpp reads it as well as counting.
//     Reading it is not redundancy: it is the difference between agreeing with
//     the engine and running a second opinion beside it.
//
// The threshold this counts against is frame-rate scaled and reads a global,
// so CoopIII does not reproduce it. It uses the tightest fixed numbers in the
// engine instead - CanPedExitCar's - which is the conservative direction: a
// car that satisfies those is a car a player can both get into and get out of,
// which is the entire point of letting it settle in the first place.
constexpr size_t  PHYSICAL_STATIC_FRAMES  = 0xED;   // CPhysical::m_nStaticFrames
constexpr uint8_t PHYSICAL_STATIC_LIMIT   = 0x0A;   // `cmp byte [ebx+0EDh],0Ah`

// Is this car quiet enough that nothing is left to simulate?
//
// Pure arithmetic on purpose, so tools/clienttest can hold the decision to the
// disassembly without a running game - the same shape as
// WorldRemoveUnlinksFromMovingList. The turn test is per component and not a
// magnitude, because that is what CanPedExitCar does at 0x0055243A, 0x00552464
// and 0x0055248E: three `fabs` compares, not one `MagnitudeSqr`.
constexpr bool VehicleAtRestNumbers(float moveSpeedSq, float turnX, float turnY,
                                    float turnZ) {
	return moveSpeedSq <= VEH_EXIT_MAX_SPEED_SQ &&
	       (turnX < 0.0f ? -turnX : turnX) <= VEH_EXIT_MAX_TURN &&
	       (turnY < 0.0f ? -turnY : turnY) <= VEH_EXIT_MAX_TURN &&
	       (turnZ < 0.0f ? -turnZ : turnZ) <= VEH_EXIT_MAX_TURN;
}

// The pose half of both gates, and the one the whole wedged-car bug turns on.
//
// **A car is refused when its up.z is INSIDE +-0.1, not outside it.** Upright
// passes, upside down passes, on its side does not - which is the pose a car
// ends up in when it was rolling at the moment the session stopped having a
// driver for it, and the pose the per-frame correction then makes permanent.
// Written down as a function because it reads backwards from the way the bug
// is described, and because both gates share it (0x00552307 and 0x005523D7 are
// the same two constants).
constexpr bool VehiclePoseAllowsPedThrough(float upZ) {
	return upZ > 0.1f || upZ < -0.1f;
}

namespace offs {

// m_vehDoor. A uint16, not the uint32 re3 declares in Ped.h:447 - every
// access in the three functions above is `movzx eax, word [reg+2E8h]` or
// `mov word [reg+2E8h], ax`. Writing a dword here would reach into the next
// member.
constexpr size_t PED_VEH_DOOR = 0x2E8;

// eCarNodes, the component index a door is named by (re3 Automobile.h:9-31,
// CAR_WHEEL_RF = 1). Read out of the three `sub eax,0Bh / cmp eax,5 /
// jmp [eax*4 + table]` switches above, which cover 11..16 and leave 13 and 14
// (the wings) on the default arm. The seat each one belongs to is not
// guesswork either - SetExitCar picks the door from the seat and that is the
// mapping below, read backwards.
constexpr uint16_t CAR_DOOR_RF = 11;   // pPassengers[0]
constexpr uint16_t CAR_DOOR_RR = 12;   // pPassengers[2]
constexpr uint16_t CAR_DOOR_LF = 15;   // pDriver
constexpr uint16_t CAR_DOOR_LR = 16;   // pPassengers[1]

// CVehicle::m_nGettingInFlags, the byte between m_nNumGettingIn and
// m_nGettingOutFlags, which this file already had on either side of it. The
// bit each door claims comes from QuitEnteringCar's `and [ebp+1CAh], 0FBh`
// family, one per door, which is also what fixes the bit *order*: the flag
// for the right-front door is 4, not 1.
constexpr size_t  VEH_GETTING_IN_FLAGS = 0x1CA;   // int8
constexpr uint8_t CAR_DOOR_FLAG_LF     = 0x01;
constexpr uint8_t CAR_DOOR_FLAG_LR     = 0x02;
constexpr uint8_t CAR_DOOR_FLAG_RF     = 0x04;
constexpr uint8_t CAR_DOOR_FLAG_RR     = 0x08;

// Bit 4 of the third CVehicle bitfield byte. SetEnterCar, SetCarJack and
// QuitEnteringCar all touch it as `[+1F7h] shr 4 & 1` / `and 0EFh` / `or 10h`.
constexpr uint8_t VEH_IS_BEING_CARJACKED = 0x10;

static_assert(PED_VEH_DOOR == PED_HEADING_RATE + 4,
              "m_vehDoor follows m_headingRate (re3 Ped.h:446-447)");
static_assert(VEH_GETTING_IN_FLAGS == VEH_NUM_GETTING_IN + 1 &&
                  VEH_GETTING_OUT_FLAGS == VEH_GETTING_IN_FLAGS + 1,
              "m_nNumGettingIn, m_nGettingInFlags, m_nGettingOutFlags are "
              "three consecutive bytes (re3 Vehicle.h:121-123)");
static_assert((CAR_DOOR_FLAG_LF | CAR_DOOR_FLAG_LR | CAR_DOOR_FLAG_RF |
               CAR_DOOR_FLAG_RR) == 0x0F,
              "the four door flags are four distinct bits of one nibble");
static_assert((VEH_IS_BEING_CARJACKED & VEH_HAS_BEEN_OWNED_BY_PLAYER) == 0,
              "two different bits of the same CVehicle bitfield byte");

} // namespace offs

// The states an entry or an exit passes through, from the writes that found
// the functions above. PED_DRAG_FROM_CAR is the one a ped being pulled out of
// a seat by somebody else is in; it is here because SetExitCar tests for it,
// not because CoopIII ever sets it.
constexpr uint32_t PEDSTATE_CARJACK       = 50;
constexpr uint32_t PEDSTATE_DRAG_FROM_CAR = 51;
constexpr uint32_t PEDSTATE_ENTER_CAR     = 52;
constexpr uint32_t PEDSTATE_EXIT_CAR      = 54;

// PED_DRAG_FROM_CAR again, from the function that actually sets it:
// CPed::SetBeingDraggedFromCar returns early at 0x004E0649 if the ped is
// already in 33h, clears bUsesCollision (`[+51h] and 0FEh`), and writes 33h at
// 0x004E0701 after LineUpPedWithCar. A cop who opens a stopped car's door is
// what puts a player in it.

// ---- busted ----
//
// PED_ARRESTED, 38h. CoopIII never writes it. It is here because it is the one
// thing on the wire that says a player has been arrested. Every site below is
// the retail image, not re3:
//
//   0x00421459  cmp [ecx+224h],38h / jne        CGameLogic::Update (0x00421400),
//               WBSTATE_PLAYING arm. Two tests in a row on the focus player's
//               ped: 31h calls KillPlayer (0x004A12E0), 38h calls
//               CPlayerInfo::ArrestPlayer (0x004A1330)
//   0x004C2B9D  mov [ebx+224h],38h               CCopPed::SetArrestPlayer
//               (0x004C2B00). Refuses 30h/31h/38h first and copies the old
//               state into m_nLastPedState (+228h). No animation is blended
//               on the player anywhere in it; the only BlendAnimation in the
//               arrest is ANIM_STD_ARREST on the *cop*, in CCopPed::ArrestPlayer
//   0x004C2CCB  mov [esi+224h],38h               CCopPed::ArrestPlayer, behind
//               CanSetPedState (0x004CE7A0)
//   0x004DED31  mov [ebp+224h],38h               PedAnimGetInCB: a player a cop
//               caught climbing into a car finishes getting in and is arrested
//               in the seat (bGonnaKillTheCarJacker, +15Ah bit 6)
//   0x004CF022  cmp [esi+224h],38h / je          PedSetDraggedOutCarCB: for an
//               arrested ped the drag animation is NOT faded out, so a player
//               a cop pulled out of a car stays lying where he landed
//
// ArrestPlayer sets CPlayerInfo::m_WBState (+0xD8, a byte) to 2 and
// m_nWBTime (+0xDC) to CTimer's clock. CGameLogic::Update's BUSTED arm
// (0x0042165E, jump table 0x005ECDA4 entry 2) fades to black at 0x800 ms and
// at 0x1000 ms takes the fine (table 0x005ECD88: 100,100,200,400,600,900,1500
// by wanted level) and the weapons, empties the car seat, then calls
// RestorePlayerStuffDuringResurrection (0x00421A60) with the nearest police
// station. That ends in CPlayerPed::SetInitialState (0x004EFC40), which writes
// PED_IDLE at 0x004EFD13 - the way back out of 38h.
constexpr uint32_t PEDSTATE_ARRESTED = 56;

static_assert(PEDSTATE_ARRESTED > PEDSTATE_TABLE_MAX,
              "ProcessControl's per-state switch stops at 55, so an arrested "
              "ped takes its default arm");

static_assert(PEDSTATE_ENTER_CAR != PEDSTATE_DRIVING &&
                  PEDSTATE_EXIT_CAR != PEDSTATE_DRIVING &&
                  PEDSTATE_CARJACK != PEDSTATE_DRIVING,
              "entering and leaving are not the same state as sitting there");

// Who actually runs the entry, and it is not CPed::ProcessControl.
//
// ProcessControl's state switch (the jump table at 0x005F8778, indexed by
// m_nPedState - 1) sends PED_ENTER_CAR, PED_CARJACK and PED_EXIT_CAR to an
// empty arm at 0x004CB9F7 - it does nothing for any of them, exactly as re3
// Ped.cpp:2751-2752 says. The state machine is driven from CWorld::Process's
// walk over ms_listMovingEntityPtrs instead (re3 World.cpp:1967-1999): the
// same walk that calls SetPedPositionInCar on a seated ped calls EnterCar()
// on an entering one, and ExitCar() on a leaving one.
//
// Two consequences, and both are load-bearing:
//
//  - a remote ped only animates into a car while it is on the moving list,
//    which is the same condition docs/protocol.md 1.13.2 already relies on
//    for a seated ped to be positioned at all. Nothing extra is needed.
//  - the engine cleans up after itself if the car goes away underneath:
//    that walk does `bInVehicle = false; QuitEnteringCar()` the moment
//    m_pMyVehicle reads null. The reference WarpPedIntoCar and
//    SetEnterCar_AllClear both register nils that pointer when the car is
//    destroyed, so this is the ordinary case rather than a corner.

// ---- script ---------------------------------------------------------------
//
// Not used by v1 (docs/protocol.md §3 scopes the campaign out). Recorded now
// because they're verified, and docs/campaign.md Tiers 2-3 need exactly
// these two.

// Script.cpp:2050, the opcode dispatcher. Tier 3 intercepts
// IS_PLAYER_IN_AREA_2D/_3D and IS_PLAYER_IN_ZONE here.
constexpr uintptr_t CRunningScript__ProcessCommands = 0x00439500;

// The 1100..1154 range handler. Its `cmp eax,36h` bound is one of five
// anchors that pinned this build as 1.0 rather than 1.1.
constexpr uintptr_t CRunningScript__ProcessCommands1100To1199 = 0x00589D00;
constexpr uintptr_t g_ScriptOpcodeTable_1100                  = 0x00610AB4;

// Two more range handlers, dug out for the combat work below the same way
// the 800..899 one was: the dispatcher at 0x00439500 is a chain of
// `cmp dx,<limit> / jge / movsx eax,dx / push eax / call <range>`, and each
// range handler opens `lea eax,[base] / cmp eax,<count> / ja default /
// jmp [eax*4 + <table>]`. Worth recording because the bases aren't round
// numbers and working one out again costs a round trip - the 600 handler's
// base is opcode 657, not 600.
constexpr uintptr_t CRunningScript__ProcessCommands500To599 = 0x004429C0;
constexpr uintptr_t g_ScriptOpcodeTable_500                 = 0x005EF298;  // base 500
constexpr uintptr_t CRunningScript__ProcessCommands600To699 = 0x00444B20;
constexpr uintptr_t g_ScriptOpcodeTable_600                 = 0x005EF424;  // base 657

// ---- combat ---------------------------------------------------------------
//
// Verified 2026-09-21 (M3). Everything here was found by disassembling the
// retail image. Where a rel32 had to be resolved, it was resolved with a
// tool and not by hand; where a function had to be identified, it was
// matched against reference/re3 statement for statement.
//
// The chain, worth reusing: COMMAND_ADD_EXPLOSION (opcode 524) gives
// CExplosion::AddExplosion. Scanning .text for E8 rel32s to it finds the two
// call sites inside CProjectileInfo, which sit in source order in the image
// exactly as ProjectileInfo.cpp declares them (GetProjectileInfo,
// AddProjectile, RemoveProjectile, RemoveNotAdd, Update). Scanning for calls
// to AddProjectile/RemoveNotAdd finds CWeapon::FireProjectile, and scanning
// for calls to that finds CWeapon::Fire. Five functions, none named anywhere
// in the binary, all from one script opcode.

// __thiscall bool CWeapon::Fire(CEntity *shooter, CVector *fireSource).
//
// `ret 8` - two stack arguments, `this` in ecx, bool in al. Its body is re3
// Weapon.cpp:100-310, and three things in it identify it beyond doubt:
//
//   0x0055C391  mov  [esp+18h], 3F19999Ah   CVector fireOffset(0,0,0.6f)
//   0x0055C3DF  cmp  byte [edi+14h], 0      m_bAddRotOffset
//   0x0055C3ED  fld [ebp+14h] / fchs / fld [ebp+18h] / fpatan
//                                           shooter->GetForward().Heading(),
//                                           i.e. Atan2(-fwd.x, fwd.y), which
//                                           re-confirms offs::MATRIX_FWD = 0x14
//   0x0055C4A2  cmp  dword [edi+8], 0 / jg  if (m_nAmmoInClip <= 0) return false
//                                           (re-confirms WEAPON_AMMO_IN_CLIP)
//
// This is the function an observer calls to replay somebody else's shot.
// docs/protocol.md §1.9.2 argues for using the engine's own fire path rather
// than a hand-built effect, and covers what's done about the fact that it
// also decides what it hit.
constexpr uintptr_t CWeapon__Fire = 0x0055C380;

// CWeapon::m_bAddRotOffset, the byte after m_nTimer. Read off the `cmp byte
// [edi+14h],0` above. It was the only CWeapon member this file was missing,
// and it's what makes sizeof(CWeapon) 0x18 rather than 0x14.
constexpr size_t WEAPON_ADD_ROT_OFFSET = 0x14;

// eWeaponState (re3 Weapon.h:11). READY/FIRING/RELOADING/OUT_OF_AMMO were
// already half-pinned by CPed::GiveWeapon's `cmp dword [..+360h],3` reset of
// OUT_OF_AMMO to READY. Fire's own `m_eWeaponState = WEAPONSTATE_FIRING` and
// its RELOADING branch fix the two in between.
constexpr uint32_t WEAPONSTATE_READY       = 0;
constexpr uint32_t WEAPONSTATE_FIRING      = 1;
constexpr uint32_t WEAPONSTATE_RELOADING   = 2;
constexpr uint32_t WEAPONSTATE_OUT_OF_AMMO = 3;

// eWeaponType, the whole inventory range, read straight out of
// CWeapon::Fire's own switch rather than transcribed from re3's enum:
//
//     0x0055C4B8  mov  eax,[edi]        m_eWeaponType
//     0x0055C4BC  sub  eax,2            <- the table starts at COLT45
//     0x0055C4BF  cmp  eax,0Bh / ja default
//     0x0055C4C8  jmp  [eax*4 + 603184h]
//
// and the twelve entries of that table land exactly where re3's cases do:
// 2/3/5 share FireInstantHit (0x0055D2E0), 4 is FireShotgun (0x00560620), 6
// is the M16's `PlayerWeaponMode.Mode == 0x22 && shooter == FindPlayerPed()`
// test before FireM16_1stPerson (0x00562180) or FireInstantHit, 7 is
// FireSniper (0x00561FE0), 8 and 10/11 reach FireProjectile, 9 is
// FireAreaEffect and 12 is the detonator. MOLOTOV and GRENADE sharing one
// entry - re3's `case WEAPONTYPE_MOLOTOV: case WEAPONTYPE_GRENADE:` - is the
// cheapest single confirmation that the numbering is right.
constexpr uint8_t WEAPONTYPE_BASEBALLBAT    = 1;
constexpr uint8_t WEAPONTYPE_COLT45         = 2;
constexpr uint8_t WEAPONTYPE_UZI            = 3;
constexpr uint8_t WEAPONTYPE_SHOTGUN        = 4;
constexpr uint8_t WEAPONTYPE_AK47           = 5;
constexpr uint8_t WEAPONTYPE_M16            = 6;
constexpr uint8_t WEAPONTYPE_SNIPERRIFLE    = 7;
constexpr uint8_t WEAPONTYPE_ROCKETLAUNCHER = 8;
constexpr uint8_t WEAPONTYPE_FLAMETHROWER   = 9;
constexpr uint8_t WEAPONTYPE_MOLOTOV        = 10;
constexpr uint8_t WEAPONTYPE_GRENADE        = 11;

// Three sub-paths of Fire, recorded not because CoopIII calls them but
// because they're the reason two weapons get refused a replay
// (docs/protocol.md §1.9.2):
//
//   FireSniper opens by reading TheCamera.Cams[ActiveCam].Mode and returns
//   false unless it's one of six first-person modes, then fires along that
//   camera's Front. On an observer that's *their* camera - so a remote
//   player's sniper shot is either nothing or a bullet out of the wrong eye.
//
//   FireAreaEffect hands the flamethrower to CShotInfo, which keeps damaging
//   for as long as the shot lives - well after the call CoopIII guards has
//   returned.
constexpr uintptr_t CWeapon__FireInstantHit = 0x0055D2E0;
constexpr uintptr_t CWeapon__FireShotgun    = 0x00560620;
constexpr uintptr_t CWeapon__FireSniper     = 0x00561FE0;
constexpr uintptr_t CWeapon__FireProjectile = 0x00561900;

// __cdecl bool CExplosion::AddExplosion(CEntity *explodingEntity,
//                                       CEntity *culprit, eExplosionType type,
//                                       const CVector &pos, uint32 lifetime).
//
// Straight out of COMMAND_ADD_EXPLOSION (opcode 524 - the 500..599 range
// handler at 0x004429C0, table 0x005EF298 entry 24, handler 0x00442E19),
// matching re3 Script3.cpp:146-149 in four instructions:
//
//     CollectParameters(&m_nIp, 4)
//     push 0 / push edx(&pos) / push eax(ScriptParams[3]) / push 0 / push 0
//     call 0x005591C0 / add esp,14h
//
// Five arguments, caller-cleaned, in that order. Every explosion in the game
// goes through it - grenade, molotov, rocket, car, barrel - which makes it
// one seam instead of three for sampling what the local player blew up, and
// the same function an observer calls to replay it.
constexpr uintptr_t CExplosion__AddExplosion = 0x005591C0;

// eExplosionType, witnessed by CProjectileInfo::RemoveProjectile mapping
// weapon -> explosion (below): GRENADE(11) -> 0, MOLOTOV(10) -> 1,
// ROCKETLAUNCHER(8) -> 2. re3 Explosion.h's enum continues CAR, CAR_QUICK,
// HELI, MINE, BARREL, TANK_GRENADE, HELI_BOMB; only the three CoopIII can
// produce from a player's own weapon are named here.
constexpr uint8_t EXPLOSION_GRENADE = 0;
constexpr uint8_t EXPLOSION_MOLOTOV = 1;
constexpr uint8_t EXPLOSION_ROCKET  = 2;
constexpr uint8_t EXPLOSION_TYPE_COUNT = 10;   // re3 Explosion.h, for bounding

// The one explosion a bullet makes without going through a car, and the one
// the culprit of which is always the local player whoever fired.
//
// CWeapon::BlowUpExplosiveThings (0x00564A60) is called with the victim at the
// tail of CWeapon::DoBulletImpact (0x005605F2), hit or miss, for every round.
// It tests the victim's model against the two indices at [0x005F5B08] and
// [0x005F5B4C] (re3: the exploding barrel and the petrol pump), sets
// bHasBeenDamaged at [ebx+175h] bit 5, and then:
//
//   00564ACE  push 64h                 lifetime 100
//   00564AD0  push eax                 &pos, 0.5 above the object
//   00564AD5  push 7                   the type
//   00564AD7  call 004A1150            FindPlayerPed()
//   00564ADC  push eax                 culprit = the local player, always
//   00564ADD  push ebx                 the object
//   00564ADE  call 005591C0            CExplosion::AddExplosion
//
// Nothing reads AddExplosion's result: the next instruction reloads the model
// index for the upward kick. FireShotgun does not call this function.
constexpr uint8_t EXPLOSION_BARREL  = 7;
// CAutomobile::BlowUpCar's two, from its own call to AddExplosion (the block
// on BlowUpCar above: `push 3` at 0x0053BF20, `push 4` on the arm at
// 0x0053BF15).
constexpr uint8_t EXPLOSION_CAR       = 3;
constexpr uint8_t EXPLOSION_CAR_QUICK = 4;
constexpr uintptr_t CWeapon__BlowUpExplosiveThings = 0x00564A60;

// ---- projectiles ----
//
// gaProjectileInfo and its stride come from CProjectileInfo::GetProjectileInfo
// (0x0055B010), which is six instructions and states both:
//
//     mov  ecx,[esp+4]
//     lea  eax,[ecx+ecx*8] / lea eax,[eax+eax*2] / add eax,ecx   ; ecx * 28
//     add  eax,64ED50h
//     ret
//
// 28 = 0x1C = eWeaponType(4) + m_pSource(4) + m_nExplosionTime(4) +
// m_bInUse(1, padded to 4) + m_vecPos(12), matching re3 ProjectileInfo.h's
// declaration order exactly. Three of those five offsets then get witnessed
// directly by AddProjectile's tail and RemoveProjectile's body.
constexpr uintptr_t gaProjectileInfo   = 0x0064ED50;
constexpr size_t    SIZEOF_PROJECTILEINFO = 0x1C;
constexpr int       NUM_PROJECTILES    = 32;   // re3 config.h:109

constexpr size_t PROJINFO_WEAPON_TYPE    = 0x00;
constexpr size_t PROJINFO_SOURCE         = 0x04;   // CEntity*, who threw it
constexpr size_t PROJINFO_EXPLOSION_TIME = 0x08;   // CTimer ms
constexpr size_t PROJINFO_IN_USE         = 0x0C;   // bool
constexpr size_t PROJINFO_POS            = 0x10;   // CVector

// CProjectileInfo::ms_apProjectile[32] - the CProjectile* parallel to the
// array above. From AddProjectile's tail, which reaches every one of them
// as `mov eax,[esi*4 + 0087C748h]` with esi = i.
constexpr uintptr_t CProjectileInfo__ms_apProjectile = 0x0087C748;

// __cdecl bool CProjectileInfo::AddProjectile(CEntity *ped, eWeaponType weapon,
//                                             CVector pos, float speed)
// pos is by value, so six stack arguments in all. Its last twenty
// instructions match re3 ProjectileInfo.cpp:158-176 field for field:
//
//     [edi+64ED50h] = weapon          [edi+64ED54h] = ped
//     CMatrix::operator=              (0x004B8F40)
//     [eax+78h/7Ch/80h] = velocity    <- re-confirms offs::MOVE_SPEED
//     [edx+122h] bit 1  = gravity     bAffectedByGravity
//     [edi+64ED58h] = time            [eax+0D0h] = elasticity
//     [ecx+17Dh] = collision response
//     [edi+64ED5Ch] = 1               m_bInUse
//     CWorld::Add                     (0x004AE930) <- already in this file
//     [edi+64ED60h..68h] = pos        m_vecPos
constexpr uintptr_t CProjectileInfo__AddProjectile = 0x0055B030;

// __cdecl void CProjectileInfo::RemoveProjectile(CProjectileInfo *info,
//                                                CProjectile *projectile).
//
// 101 bytes, and all of it matters to CoopIII, so here it is in full:
//
//     esi = info, ebx = projectile
//     eax = [esi]                      info->m_eWeaponType
//     if (eax == 0Bh) push 0 / push ebx+34h / push 0    GRENADE -> EXPLOSION_GRENADE
//     else if (eax == 0Ah) ...         push 1           MOLOTOV -> EXPLOSION_MOLOTOV
//     else if (eax == 8)   ...         push 2           ROCKET  -> EXPLOSION_ROCKET
//         push [esi+4] / push 0 / call CExplosion::AddExplosion / add esp,14h
//     [esi+0Ch] = 0                    info->m_bInUse = false
//     CWorld::Remove(projectile)       call 0x004AE9D0
//     projectile->vtable[0](1)         the deleting destructor, flag 1
//
// (re3 splits the explosion half out as RemoveNotAdd, which also exists
// standalone at 0x0055B770 and is called from CWeapon::FireProjectile - the
// compiler just inlined a copy here.)
//
// Note the shape: the three `cmp`s fall through to the teardown. A
// projectile whose m_eWeaponType is none of 8/10/11 gets freed exactly like
// the engine frees any other, with no explosion - which is how an observer
// ends a projectile it was only animating, without deciding where it went
// off. docs/protocol.md §1.9.3.
constexpr uintptr_t CProjectileInfo__RemoveProjectile = 0x0055B700;
constexpr uintptr_t CProjectileInfo__RemoveNotAdd     = 0x0055B770;
constexpr uintptr_t CProjectileInfo__GetProjectileInfo = 0x0055B010;

// ---- damage, death and respawn --------------------------------------------
//
// Verified 2026-09-21. Two functions, and between them they are the whole
// feature: one decides that a ped was hurt, the other that it died.
//
// __thiscall bool CPed::InflictDamage(CEntity *damagedBy, eWeaponType method,
//                                     float damage, ePedPieceTypes pedPiece,
//                                     uint8 direction).
//
// Found by working backwards from CPed::SetDie, which was already located
// (docs/addresses-unverified.md) and is now proved below: scanning .text for
// E8 rel32s to it gives eleven call sites, and the pair at 0x004EAD15 /
// 0x004EADBD sits inside one function whose tail is re3 PedFight.cpp:2444-2470
// statement for statement. 0x004EA410 is a `ret` followed by zero padding to
// the next 16-byte boundary, so 0x004EA420 is a real function start and not a
// landing halfway into one.
//
// The prologue names it beyond argument:
//
//   0x004EA420  fld  [0x005F9AB8] -> [esp+10h]    float dieDelta = 4.0f
//   0x004EA435  fld  [0x005F9A88] -> [esp+14h]    float dieSpeed = 0.0f
//   0x004EA43F  mov  ebx, 0Dh                     dieAnim = ANIM_STD_KO_FRONT
//   0x004EA448  mov  byte [esp+8], 0              headShot = false
//   0x004EA44F  mov  byte [esp+0Ch], 0            willLinger = false
//   0x004EA454  call FindPlayerPed / cmp ebp,eax  if (player == this)
//   0x004EA462  cmp  byte [eax+584h], 0           !player->m_bCanBeDamaged
//   0x004EA485  [ebp+224h] == 30h or 31h          DyingOrDead()
//   0x004EA4A1  mov al,[ebp+51h] / and al,1       bUsesCollision, which
//                                                 re-confirms ENTITY_FLAGS_A
//                                                 and ENTITY_USES_COLLISION
//   0x004EA4A8  cmp  dword [esp+38h], 14h         method != WEAPONTYPE_DROWNING
//
// and the tail closes it:
//
//   fld [ebp+2C0h] / fsub healthImpact / fcomp [0x005F9ADC]
//                                       m_fHealth - healthImpact >= 1.0f
//   cmp byte [ebp+314h],0               bInVehicle
//   mov ecx,[ebp+310h] / [ecx+50h] &= 7 |= 58h
//                                       m_pMyVehicle->SetStatus(
//                                           STATUS_PLAYER_DISABLED), 58h>>3 = 11
//   push [0x005F9A88] / push [0x005F9AB8] / push 0ADh / call CPed::SetDie
//                                       SetDie(ANIM_STD_NUM, 4.0f, 0.0f)
//   mov al,1 / ret 14h                  returns true when the ped died
//
// `ret 14h` is five stack arguments with `this` in ecx, and the 21 call sites
// land in exactly the modules re3 says call it: CGarages, CFire, CWorld's
// explosion sweep, CPed's own collision and drowning paths, KillPedWithCar,
// CAutomobile's drowning, CExplosion::Update and five places inside CWeapon.
//
// CoopIII detours this to take a hit on a remote player's ped away from the
// local engine and put it on the wire instead, and calls it directly to apply
// somebody else's hit to the local player. docs/protocol.md §1.10.
constexpr uintptr_t CPed__InflictDamage = 0x004EA420;

// __thiscall void CPed::SetDie(AnimationId anim, float delta, float speed).
//
// `ret 0Ch`, three stack arguments, and re3 Ped.cpp:6303-6353 in order:
// FindPlayerPed / `cmp byte [eax+584h],0` (m_bCanBeDamaged), `m_threatEntity
// = nil` at +0x18C, the DyingOrDead pair 30h/31h, `delta *= 0.5f` off
// 0x005F84B0 for PED_FALL/PED_GETUP, CPed::SetStoredState (0x004C5DB0),
// ClearAll (0x004C7F20), `m_fHealth = 0` at +0x2C0, the PED_DRIVING (2Ch)
// arm calling CPed::IsPlayer (0x004D48E0) and then vtable slot 0x40,
// `bInVehicle` at +0x314 with `m_pVehicleAnim->blendDelta = -1000.0f` at
// +0x1D8/+0x1C, `m_nPedState = 30h` (PED_DIE), and finally `cmp esi,0ADh`
// for the ANIM_STD_NUM case.
//
// Two things to know before calling it on a remote ped. It sets health to 0
// and clears the ped's collision through ClearAll, so it's a one-way trip:
// the way back is a new ped, not an undo. And its PED_DRIVING arm calls
// FlagToDestroyWhenNextProcessed for anything that isn't the player, so a
// seated remote ped has to be taken out of its car before it is killed or
// the engine deletes it on the next frame.
//
// **Re-read 2026-09-22, whole function, 0x004D37D0..0x004D3945.** The
// destroy arm is four instructions and the gate is exactly one state:
//
//   004D3848  mov  eax,[ebx+224h] / cmp eax,2Ch    m_nPedState == PED_DRIVING
//   004D3855  call 004D48E0                        CPed::IsPlayer
//   004D385C  test al,al / jnz                      the player is spared
//   004D3860  mov ebp,[ecx] / call [ebp+40h]        vtable +0x40,
//                                       FlagToDestroyWhenNextProcessed
//
// Nothing else reaches it. A ped that is merely `bInVehicle` without being
// in PED_DRIVING takes the other arm (004D3867), which only kills its
// vehicle animation - `[m_pVehicleAnim+1Ch] = 0xC47A0000`, -1000.0f. So
// testing `m_nPedState == PED_DRIVING` before the call is both necessary and
// sufficient; testing `bInVehicle` would unseat peds that did not need it.
//
// **SetDie and CPed::RemoveBodyPart commute, and that is measured rather
// than reasoned.** Both functions were disassembled end to end and the sets
// of fields they touch are disjoint:
//
//   SetDie           +0x18C +0x224 +0x2C0 +0x314 +0x1D8 +0x4C +0x157
//                    +0x228 +0x4EC
//   RemoveBodyPart   +0x1A4 (m_pFrames[node]) +0x156 +0x4F2, the RwFrame
//                    visibility walk, and the particle system
//
// Neither reads what the other writes, so a limb taken off before a death
// and a limb taken off after one leave the ped in the same state. The two
// flag bytes are +0x157 and +0x156, adjacent and *not* the same byte, which
// is the near-miss this note exists for. CoopIII still sends the limb before
// the death because that is the order `CPed::InflictDamage` does it in - see
// docs/population.md §5.4.
constexpr uintptr_t CPed__SetDie = 0x004D37D0;

// __thiscall void CPed::RemoveBodyPart(PedNode nodeId, int8 direction).
//
// Found from CPed::InflictDamage above, which calls 0x004EAEE0 ten times and
// nothing else more than that - re3 PedFight.cpp has exactly ten
// RemoveBodyPart calls in InflictDamage, and the pushed node arguments come in
// re3's order to the instruction:
//
//   0x004EA7F0..0x004EA889  push 2 / 3 / 4 / 7 / 8, each after a `mov ebx`
//                           of 11h/13h/14h/15h/16h - the bullet arm, head
//                           first, with its ANIM_STD_KO_SHOT_* die animation
//   0x004EA932..0x004EA984  push 3 / 8 / 2 / 4 / 7 - the explosion arm,
//                           `random & 1/2/4/8/10h` in that order
//
// and in every one the direction is `mov al,[esp+44h] / push eax`, the int8
// fifth argument of InflictDamage passed straight through.
//
// The body is re3 PedFight.cpp:2522-2566 statement for statement:
//
//   0x004EAEE7  mov eax,[ebp+eax*4+1A4h] / cmp [eax+10h],0
//                                  m_pFrames[nodeId]->frame, PED_FRAMES
//   0x004EAEF8  push 5F9C98h / call  the "Trying to remove ped component"
//                                  printf; the string is at that address
//   0x004EAF10  cmp byte [5F4DD4h],0  CGame::nastyGame
//   0x004EAF21  cmp [esp+34h],2 / je  nodeId != PED_HEAD
//   0x004EAF33  call 004EB060         CPed::SpawnFlyingComponent(node, dir)
//   0x004EAF49  call 004EAE20         RecurseFrameChildrenVisibilityCB
//   0x004EAFA1  call 00474CC0         CEntity::GetIsOnScreen
//   0x004EB02E  cmp bl,10h / jb       the sixteen PARTICLE_BLOOD_SMALLs
//   0x004EB033  [ebp+156h] &= DFh |= 20h   bBodyPartJustCameOff
//   0x004EB047  mov [ebp+4F2h],al          m_bodyPartBleeding = nodeId
//   0x004EB052  ret 8                 two stack arguments, `this` in ecx
//
// Everything it does is to the ped and the particle system: it does not
// damage, kill or move anybody. So calling it on a replica is exactly as safe
// as its effect is visible, and it is how an observer shows a limb its host's
// engine took off. docs/protocol.md, version 17.
constexpr uintptr_t CPed__RemoveBodyPart = 0x004EAEE0;

// The defaults every caller of SetDie passes, read off the two globals the
// call sites push rather than copied from re3's declaration.
constexpr float PED_DIE_DELTA = 4.0f;   // 0x005F9AB8
constexpr float PED_DIE_SPEED = 0.0f;   // 0x005F9A88

// AnimationId. ANIM_STD_KO_FRONT is InflictDamage's `mov ebx,0Dh` default;
// ANIM_STD_NUM is SetDie's `cmp esi,0ADh` and means "no die animation at
// all", which is the arm that only clears bIsPedDieAnimPlaying.
//
// Note the retail number: 0ADh is 173, while re3's `main` AnimationId enum
// puts ANIM_STD_NUM at 174. re3 has one animation this build does not, so
// anything that bounds an id must use the group's own numAssociations read at
// runtime (ped.cpp AnimGroupCount) and never a count taken from re3.
constexpr uint16_t ANIM_STD_KO_FRONT = 13;
constexpr uint16_t ANIM_STD_NUM      = 173;

// eWeaponType past the inventory range. These aren't weapons, they're damage
// *causes*, and they never have a CPed::m_weapons slot - IsInventoryWeapon in
// pedanim.h is what keeps one of them from indexing that array.
//
// WEAPONTYPE_DROWNING is the one the binary states directly: InflictDamage
// tests `cmp dword [esp+38h],14h` twice, once for the bUsesCollision bypass
// and once for the in-vehicle branch, and 14h is 20. The rest follow from
// re3's enum with that as the anchor.
//
// DROWNING and the default arm matter more than they look. Neither checks a
// proof flag at all, so the four proofs a remote ped carries do not cover
// them: a remote ped standing in water on an observer's machine drowns
// locally, whatever its owner says. That hole is why CoopIII refuses damage
// at InflictDamage rather than relying on the flags.
constexpr uint8_t WEAPONTYPE_LAST_WEAPONTYPE = 14;
constexpr uint8_t WEAPONTYPE_ARMOUR          = 15;
constexpr uint8_t WEAPONTYPE_RAMMEDBYCAR     = 16;
constexpr uint8_t WEAPONTYPE_RUNOVERBYCAR    = 17;
constexpr uint8_t WEAPONTYPE_EXPLOSION       = 18;
constexpr uint8_t WEAPONTYPE_UZI_DRIVEBY     = 19;
constexpr uint8_t WEAPONTYPE_DROWNING        = 20;
constexpr uint8_t WEAPONTYPE_FALL            = 21;
constexpr uint8_t WEAPONTYPE_UNIDENTIFIED    = 22;

// ePedPieceTypes, InflictDamage's fourth argument. Seven of them, and it
// switches on all seven in the bullet arm to pick which limb comes off
// (re3 PedFight.cpp:2200-2240). Nothing indexes an array with it, but it
// still gets bounded on the way in: an unknown piece would take the shot
// down the `default:` path and leave dieAnim at whatever it was.
constexpr uint8_t PEDPIECE_TORSO    = 0;
constexpr uint8_t PEDPIECE_MID      = 1;
constexpr uint8_t PEDPIECE_LEFTARM  = 2;
constexpr uint8_t PEDPIECE_RIGHTARM = 3;
constexpr uint8_t PEDPIECE_LEFTLEG  = 4;
constexpr uint8_t PEDPIECE_RIGHTLEG = 5;
constexpr uint8_t PEDPIECE_HEAD     = 6;
constexpr uint8_t PEDPIECE_COUNT    = 7;

// InflictDamage's fifth argument: which way the hit came from, 0 front,
// 1 left, 2 back, 3 right. It only picks between the four
// ANIM_STD_HIGHIMPACT_* animations (25..28), so a bad value is cosmetic
// rather than dangerous, but it gets bounded anyway because it arrives off a
// socket.
constexpr uint8_t PED_DAMAGE_DIRECTIONS = 4;

// ---- fire -----------------------------------------------------------------
//
// Verified 2026-09-21, against the binary and not against re3's NUM_FIRES.
// docs/roadmap.md §5.7 asked two questions before any of this could be
// designed, and this block is the answer to both.
//
// **Is the fire table a fixed size in retail, and what size?** Yes, and 40.
// Three separate functions carry the bound and all three agree, which is what
// makes it a fact rather than a reading:
//
//   0x00479336  cmp ebp,28h / jl      CFireManager::Update
//   0x00479304  cmp eax,28h / jl      CFireManager::GetNextFreeFire
//   0x004794C4  cmp ebx,28h / jl      FindFurthestFire_NeverMindFireMen
//
// and all three walk it with `add <reg>,30h`, with GetNextFreeFire and
// FindFurthestFire also doing `imul <reg>,<reg>,30h` to turn an index back
// into a pointer. So NUM_FIRES is 40 and sizeof(CFire) is 0x30.
//
// re3 also says 40 (src/core/config.h:107). That is a coincidence worth
// naming rather than leaning on: the last three bugs in this project were all
// an re3 constant that did not match retail (the four-entry anim group, the
// twelve-entry node array, the ANIM_STD_NUM off-by-one), so this one was
// measured the same way and happens to have come back equal.
//
// **Can a CFire point at an entity rather than just a position?** Yes, and
// the answer has more in it than a yes. CFire::ProcessFire opens on
// `mov eax,[ebx+10h] / test eax,eax` and, when m_pEntity is set, *rewrites
// m_vecPos from that entity's matrix every single frame* before doing
// anything else:
//
//   0x004798D8  mov eax,[ebx+10h]             m_pEntity
//   0x004798E3  add eax,34h                   ->GetPosition()
//   0x004798E6  fld [eax] / fstp [ebx+4]      m_vecPos.x
//                     ... y, z                m_vecPos = entity position
//   0x004798FA  mov al,[esi+50h] / and al,7   entity type
//   0x00479904  cmp eax,3                     ENTITY_TYPE_PED
//   0x0047990D  cmp [esi+4B4h],ebx            ped->m_pFire != this -> Extinguish
//   0x004795DD  cmp [ecx+1E4h],0              veh->m_pCarFire (StartFire's arm)
//
// So the two kinds of fire are genuinely different sync problems, which is
// what §5.7 suspected. A fire on the pavement has a position that *is* its
// state. A fire on an entity has a position that is *derived* from the
// entity, a life tied to that entity, and a two-way link the engine asserts
// every frame - and it cannot exist at all on a machine where that entity
// does not. That is also why CReplay's two memcpys are not the precedent they
// look like: replay is one process, so m_pEntity and m_pSource still point at
// something. Across the wire they are per-process pool pointers (§1.5).
//
// CFire, 0x30 bytes, every field pinned by CFire::CFire at 0x00479220 unless
// noted, and cross-confirmed by StartFire and ProcessFire:
//
//   +0x00  bool  m_bIsOngoing              ctor 0, Update's `cmp byte,1`
//   +0x01  bool  m_bIsScriptFire           ctor 0, GetNextFreeFire's second test
//   +0x02  bool  m_bPropagationFlag        ctor 1, StartFire writes the arg
//   +0x03  bool  m_bAudioSet               ctor 1
//   +0x04  CVector m_vecPos                ctor (0,0,0); ProcessFire rewrites
//   +0x10  CEntity *m_pEntity              ctor nil
//   +0x14  CEntity *m_pSource              ctor nil; ProcessFire passes it
//                                          straight to InflictDamage
//   +0x18  uint32 m_nExtinguishTime        StartFire: GetTimeInMilliseconds()
//                                          + 2710h (10000)
//   +0x1C  uint32 m_nStartTime             StartFire: + 190h (400)
//   +0x20  int32  field_20                 ctor 1
//   +0x24  uint32 m_nNextTimeToAddFlames   StartFire 0; ProcessFire compares
//                                          it against [0x00885B48]
//   +0x28  uint32 m_nFiremenPuttingOut     ctor 0
//   +0x2C  float  m_fStrength              ctor 3F4CCCCDh = 0.8f
constexpr uintptr_t gFireManager      = 0x008F31D0;
constexpr size_t    FIREMGR_TOTAL     = 0x00;   // uint32 m_nTotalFires
constexpr size_t    FIREMGR_FIRES     = 0x04;   // CFire m_aFires[NUM_FIRES]
constexpr size_t    NUM_FIRES         = 40;     // 28h, three witnesses above
constexpr size_t    SIZEOF_CFIRE      = 0x30;

constexpr size_t FIRE_ONGOING     = 0x00;
constexpr size_t FIRE_SCRIPT      = 0x01;
constexpr size_t FIRE_POS         = 0x04;
constexpr size_t FIRE_ENTITY      = 0x10;
constexpr size_t FIRE_SOURCE      = 0x14;
constexpr size_t FIRE_EXTINGUISH  = 0x18;
constexpr size_t FIRE_STRENGTH    = 0x2C;

// gFireManager's m_nTotalFires is the dword CFire::ReportThisFire increments
// and CFire::Extinguish decrements, which is how the global was found in the
// first place:
//
//   0x004798B0  inc dword [0x008F31D0]        ReportThisFire
//   0x00479D4F  dec dword [0x008F31D0]        Extinguish, !m_bIsScriptFire arm
//
// It is not a count of burning fires. Extinguish only decrements it for a
// fire that is not a script fire, and StartScriptFire never reports one at
// all, so anything that wants "how many fires are alive" has to walk the
// array. combat.cpp's BurningFires() does.
static_assert(FIREMGR_FIRES == 4, "m_aFires starts one dword into the manager");

// The back-pointers the engine keeps on the burning entity itself. Both are
// asserted by ProcessFire every frame: a fire whose entity no longer points
// back at it extinguishes itself on the spot.
constexpr size_t PED_FIRE     = 0x4B4;   // CFire *CPed::m_pFire
constexpr size_t VEHICLE_FIRE = 0x1E4;   // CFire *CVehicle::m_pCarFire

// __thiscall void CFireManager::Update() - the loop above, called once a
// frame from 0x0048C936 with ecx = 0x008F31D0.
constexpr uintptr_t CFireManager__Update = 0x00479310;

// __thiscall CFire *CFireManager::GetNextFreeFire() - first slot with neither
// m_bIsOngoing nor m_bIsScriptFire set, nil if all 40 are taken.
constexpr uintptr_t CFireManager__GetNextFreeFire = 0x004792E0;

// __thiscall void CFireManager::StartFire(CVector pos, float size,
//                                         bool propagation)   [ret 14h]
//
// A fire on the pavement: m_pEntity and m_pSource are both nilled, the
// extinguish time is fixed at ten seconds, and nothing owns it.
//
// **It has exactly one caller in the whole image**, at 0x0055957E inside
// CExplosion::AddExplosion. That single fact retires half of §5.7's phase
// two: in retail 1.0 the only thing that puts an ownerless fire on the ground
// is an explosion, and CoopIII already replays every player's explosion at a
// fixed world position every machine agrees on (§1.9.3). Pavement fires are
// therefore already the same on every machine, and syncing them again would
// double them.
constexpr uintptr_t CFireManager__StartFirePos = 0x00479500;

// __thiscall CFire *CFireManager::StartFire(CEntity *entityOnFire,
//                                           CEntity *fleeFrom, float strength,
//                                           bool propagation)   [ret 10h]
//
// A fire on a ped or a car. Returns nil rather than starting one when the
// target already has a fire, when a ped is not IsPedInControl, or when a car's
// engine is already past 225:
//
//   004795A6  cmp dword [ebx+4B4h],0 / je         ped: m_pFire must be nil
//   004795C2  call 004CE6C0 / test al,al / jne    ped: IsPedInControl
//   004795DD  cmp dword [ecx+1E4h],0 / je         car: m_pCarFire must be nil
//   00479601  call 00545960 / cmp eax,0E1h / jb   automobile: engine status < 225
//
// So an entity holds one CFire at most, and a second StartFire on it is a
// no-op. That is what makes a doubled ignition harmless where a doubled hit
// is not: fire damage is dealt per CFire, never per ignition.
//
// Six callers, each resolved from its rel32 (this used to name CExplosion as
// one of them, which was wrong - the explosion reaches it through the two
// CWorld functions):
//
//   0x00479AA0  CFire::ProcessFire, spreading to FindPlayerPed()
//   0x004B3E3C  CWorld::SetPedsOnFire     (0x004B3D30)
//   0x004B3F9C  CWorld::SetCarsOnFire     (0x004B3E90)
//   0x0053BEFE  CAutomobile::BlowUpCar, the car lighting itself
//   0x00558985  CBulletInfo::Update's car arm, behind `cmp [ebp],9`. Dead in
//               retail: CBulletInfo::AddBullet (0x00558470) has one caller,
//               FireSniper at 0x00562107, so no bullet is ever cause 9
//   0x0055C232  CShotInfo::Update, the flamethrower reaching a ped
constexpr uintptr_t CFireManager__StartFireEntity = 0x00479590;

// ---- the flamethrower, from trigger to fire ---------------------------------
//
// The flamethrower never calls InflictDamage. Every health point it takes is
// taken later by a CFire it lit, and CFire::ProcessFire calls InflictDamage
// with m_pSource and cause 9 and has forgotten everything else. By then a
// flame, a molotov's blast and a car's own explosion all look the same. The
// last place the flamethrower is still itself is CShotInfo::Update.
//
//   CWeapon::Fire (0x0055C380), table 0x00603184 entry 7 (weapon 9 - 2)
//     -> 0x0055C70F  call CWeapon::FireAreaEffect (0x00561E00)
//        a heading from the shooter's forward; the camera's aim instead when
//        the shooter is FindPlayerPed() and the camera allows it (0x00561E54,
//        0x00561E63), so an observer's replay is always flat
//        -> 0x00561FA2  call CShotInfo::AddShot(shooter, 9, source, target)
//        -> 0x00561FC2  call 0x00561C70, the particles
//   every frame, CGame::Process (0x0048C940) -> CWeapon::UpdateWeapons
//     (0x0055C310), whose first instruction is call CShotInfo::Update
//     (0x0055BFF0)
//
// CShotInfo::Update, per in-use slot (0x0055C047 tests m_inUse):
//
//   0055C05E  weapon info flags: bit 1 slows it down, bit 4 widens it
//   0055C11D  m_startPos += m_areaAffected * timestep
//   0055C133  cmp [edi+20h],0 / je        no source, no peds
//   0055C148  fcomp 1.0f                  r = max(m_radius, 1.0f)
//   0055C170  for each of the source's m_nearPeds:
//   0055C179    call 004D4930             CPed::IsPointerValid
//   0055C1A8    call 004CE6C0             IsPedInControl
//   0055C1CB    fcomp r                   |ped - m_startPos|^2 < r (not r^2)
//   0055C1D9    [esi+53h] shr 1 / and 1   bFireProof -> skip
//   0055C1E4    call 004D48E0             IsPlayer; if not: SetFindPathAndFlee
//               (0x004D1D70, source, 10000), SetMoveState(SPRINT)
//   0055C232    call StartFire(ped, source, 0.8f [0x00603028], 1)
//   0055C24A  every fourth frame, (frame + slot) & 3 == 0:
//   0055C26C    call CWorld::SetCarsOnFire(m_startPos, 4.0f [0x0060302C], source)
//               -> for a car not wrecked, not burning, not bFireProof, within
//                  5.0 in z and 4.0 in x and y: StartFire(car, source, 0.8f, 1)
//                  at 0x004B3F9C
//
// The bFireProof test at 0x0055C1D9 corrects a note in the fire section
// below, which said this function checks nothing but IsPedInControl and a
// distance. It checks the proof flag like everything else that lights a ped.
//
// Nothing else calls CShotInfo::Update and nothing else calls AddShot, so
// every slot in gaShotInfo is a flamethrower and every StartFire made while
// Update runs is one of the two above. The only other thing the window
// contains is the flee AI, which starts no fires.
constexpr uintptr_t CWeapon__FireAreaEffect = 0x00561E00;
constexpr uintptr_t CShotInfo__AddShot      = 0x0055BD70;   // __cdecl, 8 dwords
constexpr uintptr_t CShotInfo__Update       = 0x0055BFF0;   // __cdecl void(), ret
constexpr uintptr_t CWorld__SetPedsOnFire   = 0x004B3D30;   // CExplosion::Update only
constexpr uintptr_t CWorld__SetCarsOnFire   = 0x004B3E90;   // CExplosion + CShotInfo

// gaShotInfo, 100 slots of 0x2C. The base and the stride are Update's own loop
// (`mov edi,64F0D0h` ... `add edi,2Ch` ... `cmp [esp+0Ch],64h`); the fields
// are AddShot's writes, in its order:
//
//   0055BDD7  mov byte [esi+64F0F8h],1       +0x28 m_inUse
//   0055BDDE  mov [esi+64F0D0h],ebx          +0x00 m_weapon
//   0055BDE4  fstp [esi+64F0D4h] ..          +0x04 m_startPos
//   0055BE06  fstp [esi+64F0E0h] ..          +0x10 m_areaAffected
//   0055BE2B  fstp [esi+64F0ECh]             +0x1C m_radius, off the weapon info
//   0055BF9B  mov [esi+64F0F0h],eax          +0x20 m_sourceEntity
//   0055BFDE  fstp [esi+64F0F4h]             +0x24 m_timeout, a float of ms
constexpr uintptr_t gaShotInfo        = 0x0064F0D0;
constexpr size_t    NUM_SHOT_INFOS    = 100;
constexpr size_t    SIZEOF_SHOTINFO   = 0x2C;
constexpr size_t    SHOT_WEAPON       = 0x00;
constexpr size_t    SHOT_POS          = 0x04;
constexpr size_t    SHOT_RADIUS       = 0x1C;
constexpr size_t    SHOT_SOURCE       = 0x20;
constexpr size_t    SHOT_IN_USE       = 0x28;

// CPed::m_nearPeds and m_numNearPeds, the list CShotInfo::Update walks.
// CPed::BuildPedLists fills them (`mov [ebp+edx*4+4F4h],eax / inc word
// [ebp+51Ch]` at 0x004C55D0) and stops at 0Ah (0x004C55E5), then nils the rest
// up to 0Ah (0x004C5605). The count is a word.
constexpr size_t PED_NEAR_PEDS     = 0x4F4;
constexpr size_t PED_NUM_NEAR_PEDS = 0x51C;
constexpr size_t PED_NEAR_PEDS_MAX = 10;
static_assert(PED_NEAR_PEDS + PED_NEAR_PEDS_MAX * 4 == PED_NUM_NEAR_PEDS,
              "the count sits right after the ten pointers");

// __thiscall int32 CFireManager::StartScriptFire(const CVector &pos,
//                                                CEntity *target,
//                                                float strength,
//                                                bool propagation)
//
// The script's own fire, and the one kind that is not tied to an explosion.
// Only the host runs CTheScripts::Process (docs/campaign.md), so this is
// Area D's problem rather than this one's; it is recorded because §5.7's
// phase three has to account for it.
constexpr uintptr_t CFireManager__StartScriptFire = 0x00479E60;

// __thiscall void CFire::ProcessFire() - the whole of the burning rule, and
// the only producer of WEAPONTYPE_FLAMETHROWER damage in the image. See the
// two call sites named in combat.h.
constexpr uintptr_t CFire__ProcessFire = 0x004798D0;

// __thiscall void CFire::Extinguish() - clears m_bIsOngoing, nils the
// entity's back-pointer and, for a ped, calls CPed::RestorePreviousState
// (0x004C5E30). Safe on a fire that is not burning: the whole body is inside
// `cmp byte [ebx],0 / je end`.
constexpr uintptr_t CFire__Extinguish = 0x00479D40;

// How many of the 40 slots are actually alight. Pure arithmetic over the
// table geometry above so tools/clienttest can check the walk without the
// game; the caller supplies the reader.
//
// Bounded by NUM_FIRES and by nothing else - there is no count to trust. This
// is the shape the last three crashes said to use: the engine's own bound,
// read out of the engine's own loop, applied by us.
template <typename ReadByte>
inline uint32_t CountOngoingFires(ReadByte readByte) {
	uint32_t n = 0;
	for (size_t i = 0; i < NUM_FIRES; ++i) {
		if (readByte(gFireManager + FIREMGR_FIRES + i * SIZEOF_CFIRE + FIRE_ONGOING))
			++n;
	}
	return n;
}

// ---- lighting a ped without waking its AI (roadmap §5.7 phase three) -------
//
// **Why an observer sees no flame on a burning remote player, exactly.** Not
// a missing feature, a working guard. Everything in retail that sets peds
// alight in a radius tests bFireProof first, and every remote player is
// bFireProof:
//
//   004B3D9A  mov al,[ecx+53h] / shr al,1 / and al,1 / jne skip   the ped
//             loop in CWorld::SetPedsOnFire, right after its m_pFire test
//   004B3EFB  the same three instructions in the car loop
//   004EA898  the same three, as InflictDamage's entry for cause 9
//             (WEAPONTYPE_FLAMETHROWER, jump table 0x005F9EB8 entry 9):
//             fire proof returns false before anything else happens
//
// So the rocket that lands between two players lights the victim's own ped
// on the victim's machine and is refused by every observer, which is the
// "observers never decide damage" rule doing its job. The flame has to be
// replicated deliberately or not at all - and the same bFireProof test is
// why replicating it cannot cost the remote ped any health.
//
// CShotInfo::Update checks it too, at 0x0055C1D9, right after its distance
// test. This note used to say it was the one path that did not, and it was
// wrong: nothing in retail 1.0 lights a bFireProof ped. ped.cpp still puts out
// a fire on a remote ped that it did not light itself (PlanRemoteFire), which
// now covers fires lit before the proof flag was set, or by another mod.
//
// Verified 2026-09-22. §5.7 said the entity arm of StartFire "calls SetFlee,
// SetMoveState(PEDMOVE_SPRINT), SetMoveAnim() and SetPedState(PED_ON_FIRE)"
// and treated that as the price of a flame. It is not the price: it is one
// branch of the function, and **the engine itself skips it**.
//
// CFireManager::StartFire(entity, fleeFrom, strength, propagation), the ped
// arm, reads:
//
//   00479640  mov [ebp+4B4h], esi           ped->m_pFire = fire
//   00479646  call 004A1150                 FindPlayerPed()
//   0047964B  cmp ebp, eax
//   0047964F  je  00479890                  ...which jumps straight to 004796C7
//   -------- everything between is the AI, and only a non-player ped runs it
//   0047965C  call 004D1D70 / 00479690 call 004D1C40    CPed::SetFlee
//   004796A7  and al,0DFh                   clear bit 5 of [ped+157h]
//   004796B1  call 004C5A30 (push 4)        CPed::SetMoveState(PEDMOVE_SPRINT)
//   004796BA  call [vtable+48h]             CPed::SetMoveAnim()
//   004796BD  mov [ebp+224h], 20h           m_nPedState = PED_ON_FIRE
//   -------- 004796C7 onward is shared, and is nothing but field writes
//
// So there is a second, already-shipping way for a ped to be on fire in GTA
// III with no burning-ped AI attached to it at all, and it is the way the
// engine uses for the one ped whose movement is not the engine's to decide.
// A remote player is exactly that ped on this machine. CoopIII takes the
// same branch rather than inventing one: the writes below are the common
// tail, transcribed, in its order.
//
//   0047971E  m_vecPos       = entity->GetPosition()      FIRE_POS
//   00479739  m_bIsOngoing   = 1                          FIRE_ONGOING
//   0047973C  m_bIsScriptFire= 0                          FIRE_SCRIPT
//   00479764  call 004D48E0  CPed::IsPlayer -> m_nExtinguishTime is
//             now + 0D05h (3333 ms) for a player ped, now + 2710h plus a
//             random spread for anything else                FIRE_EXTINGUISH
//   0047982E  m_nStartTime   = now + 190h (400 ms)         FIRE_START_TIME
//   0047983D  m_pEntity      = entity                      FIRE_ENTITY
//   00479844  call 004A7480  CEntity::RegisterReference(&m_pEntity)
//   0047984D  m_pSource      = fleeFrom                    FIRE_SOURCE
//   0047985F  call 004A7480  the same, for m_pSource, when it is not nil
//   00479866  call 004798B0  CFire::ReportThisFire
//   0047986B  m_nNextTimeToAddFlames = 0                   FIRE_NEXT_FLAMES
//   00479872  m_fStrength    = strength                    FIRE_STRENGTH
//   0047987D  m_bPropagationFlag = propagation             FIRE_PROPAGATION
//   00479880  m_bAudioSet    = 1                           FIRE_AUDIO_SET
//
// What the tail does *not* write is as load-bearing as what it does:
// field_20 (+0x20) and m_nFiremenPuttingOut (+0x28) keep whatever the slot's
// last tenant left in them. Only CFire::CFire touches those, and it runs
// once at startup, so a fire that "resets" them is doing something retail
// never does. CoopIII writes exactly this list and nothing else.
//
// And the AI is the *only* thing StartFire puts on a ped. CFire::ProcessFire
// writes nothing to a burning ped but the position it reads back out of it:
// it asserts the m_pFire back-pointer, nudges m_vecPos.z, sets a burning
// car's engine damage, and calls InflictDamage - which for a remote player
// is refused twice over (below). Nothing in the per-frame path touches
// m_nPedState, m_nMoveState or the clump. That is why this is a visual
// replication and not a second authority over the pose stream.
constexpr uint32_t PEDSTATE_ON_FIRE = 32;   // 20h, StartFire's own write

// __thiscall bool CPed::IsPedInControl() - StartFire's gate for a ped, at
// 0x004795C2, and the reason §5.7 called this a reconciliation loop rather
// than an event handler: a seated, dying or dead remote ped is refused a
// fire and has to be offered one again on a later frame.
//
//   004CE6C0  cmp dword [ecx+224h],22h / jg fail    m_nPedState <= 34
//   004CE6C9  [ecx+155h] bit 3 / bit 4 set -> fail
//   004CE6E3  fld [ecx+2C0h] / fcomp [0x5F8438]     m_fHealth > 0.0f
//
// Both field offsets are ones this file already had - PED_STATE at 0x224 and
// PED_HEALTH at 0x2C0 - which is what identifies the function. PED_DRIVING
// is 44 and PED_DIE/PED_DEAD are 48/49, so all three fail the first test.
constexpr uintptr_t CPed__IsPedInControl = 0x004CE6C0;

// __thiscall void CFire::ReportThisFire() - ten instructions, and both of
// them matter:
//
//   004798B0  inc dword [0x008F31D0]                   m_nTotalFires++
//   004798B6  push 3E8h / push [ecx+0Ch],[ecx+8],[ecx+4] / push 7
//   004798C6  call 00475E10                            an EVENT_FIRE at m_vecPos
//
// The count is what CFire::Extinguish decrements, so skipping this call
// would leave the manager's total drifting down every time a remote player
// stopped burning. The event is the engine's own AI noticing a fire that is
// really in this world, which it is.
constexpr uintptr_t CFire__ReportThisFire = 0x004798B0;

// StartFire registers both &m_pEntity and &m_pSource through
// CEntity::RegisterReference (0x004A7480, recorded above with the seating
// work), so a burning ped that gets deleted leaves the fire pointing at
// nothing instead of at a freed pool slot. The fire seam does the same.

// The rest of the CFire layout, from the same transcription. FIRE_ONGOING,
// FIRE_SCRIPT, FIRE_POS, FIRE_ENTITY, FIRE_SOURCE, FIRE_EXTINGUISH and
// FIRE_STRENGTH are above.
constexpr size_t FIRE_PROPAGATION = 0x02;
constexpr size_t FIRE_AUDIO_SET   = 0x03;
constexpr size_t FIRE_START_TIME  = 0x1C;
constexpr size_t FIRE_NEXT_FLAMES = 0x24;

// 0.8f, and it is the same number three different ways: CFire::CFire's own
// initialiser (3F4CCCCDh), the constant CWorld::SetPedsOnFire pushes
// (0x005F7A50) and the one CShotInfo::Update pushes (0x00603028). There is
// no second strength for a ped in retail 1.0.
constexpr float FIRE_PED_STRENGTH = 0.8f;

// Which slot a CFire pointer is, and where a slot lives. Pure arithmetic
// over the table geometry, so tools/clienttest can check the round trip
// without the game.
//
// CoopIII remembers the *index*, never the pointer, for the same reason
// RemotePlayer::poolHandle is a pool reference and not a CPed* - the slot
// outlives the tenant, and an index that has been re-let is a question the
// contents can answer (m_pEntity still points at our ped) where a pointer
// would just keep matching.
inline uintptr_t FireSlot(size_t index) {
	return gFireManager + FIREMGR_FIRES + index * SIZEOF_CFIRE;
}

// -1 for anything that is not the start of one of the 40 slots.
inline int FireSlotIndex(const void *fire) {
	const uintptr_t addr = reinterpret_cast<uintptr_t>(fire);
	const uintptr_t base = gFireManager + FIREMGR_FIRES;
	if (addr < base)
		return -1;
	const uintptr_t delta = addr - base;
	if (delta % SIZEOF_CFIRE != 0)
		return -1;
	const uintptr_t index = delta / SIZEOF_CFIRE;
	if (index >= NUM_FIRES)
		return -1;
	return static_cast<int>(index);
}

// ---- the moving entity list -----------------------------------------------
//
// CWorld::ms_listMovingEntityPtrs (0x008F433C) is a CPtrList of every physical
// the engine still has to simulate, and CWorld::Process walks it four times a
// frame. The very first of those walks, at 0x004B1B20, is three instructions:
//
//     mov ebp, [edi]        node->item, the entity
//     mov edi, [edi+8]      node->next
//     mov eax, [ebp+4Ch]    entity->m_rwObject
//
// so a node whose entity has been freed faults on the third one, at
// 0x004B1B25, with nothing in the call stack to say who freed it. This block
// exists because CoopIII destroys peds the engine is still holding, and
// getting that wrong is invisible until it is fatal.

// __thiscall void CPhysical::AddToMovingList(void).
//
//     push 0Ch / call CPtrNode::operator new
//     test eax,eax / je +2 / mov [eax],ebx     node->item = this
//     mov [ebx+0E8h], eax                      m_movingListNode = node
//     ... links it at the head of [0x008F433C]
//
// Note what isn't there: any check for an entity that is already in the list.
// Call it twice and the first node is simply forgotten by the entity while
// staying in the list, which is the other way to end up at 0x004B1B25.
constexpr uintptr_t CPhysical__AddToMovingList = 0x004958F0;

// __thiscall void CPhysical::RemoveFromMovingList(void).
//
// The safe half of the pair: it opens `mov ecx,[ebx+0E8h] / test ecx,ecx /
// je end`, so calling it on an entity that isn't in the list costs four
// instructions and does nothing. That is what makes it usable as an
// unconditional belt before a teardown.
constexpr uintptr_t CPhysical__RemoveFromMovingList = 0x00495940;

// CPed::FlagToDestroyWhenNextProcessed, vtable slot 16 (+0x40). Recorded for
// the evidence rather than to be called: it is what CAutomobile::BlowUpCar,
// CPed::SetDie's PED_DRIVING arm and CopPed all use to hand a ped to the
// engine to destroy on its own schedule.
constexpr uintptr_t CPed__FlagToDestroyWhenNextProcessed = 0x004D6570;
constexpr size_t    VTABLE_FLAG_TO_DESTROY               = 16;

// Will the engine's own CWorld::Remove take this entity out of the moving
// list, or quietly leave it there?
//
// Both CWorld::Add (0x004AE930) and CWorld::Remove (0x004AE9D0) end with the
// same three tests, byte for byte:
//
//     mov al,[ebx+50h] / and al,7
//     cmp al,1 / je skip          ENTITY_TYPE_BUILDING
//     cmp al,5 / je skip          ENTITY_TYPE_DUMMY
//     mov al,[ebx+51h] / shr al,2 / and al,1      bIsStatic
//     jne skip
//     call AddToMovingList        (in Add)
//     call RemoveFromMovingList   (in Remove)
//
// which also re-confirms ENTITY_FLAGS_A and ENTITY_IS_STATIC. The gate is
// symmetric on paper and asymmetric in practice, because the flag can change
// between the two calls: an entity linked in while it was moving, and static
// by the time it is removed, never gets unlinked. ~CPed's first statement is
// CWorld::Remove(this), so that is the path every ped teardown takes.
inline bool WorldRemoveUnlinksFromMovingList(uint8_t entityFlagsA) {
	return (entityFlagsA & offs::ENTITY_IS_STATIC) == 0;
}

// Does this entity need the unlink doing by hand before it is destroyed?
//
// `linked` is m_movingListNode != null. The dangerous combination is the one
// this returns true for: still in the list, and static, so the engine's own
// teardown will walk straight past it.
inline bool NeedsMovingListUnlink(uint8_t entityFlagsA, bool linked) {
	return linked && !WorldRemoveUnlinksFromMovingList(entityFlagsA);
}

// Is this address inside the game image? Every CEntity vtable is, because
// they all live in the exe's read-only data.
inline bool IsImageAddress(uintptr_t value) {
	return value >= IMAGE_BASE && value < IMAGE_BASE + IMAGE_SIZE;
}

// Could this value be a pointer to a game object, or is it something that
// was never a pointer at all?
//
// Deliberately weak. It is not trying to prove a pointer is good, only to
// reject a value that cannot possibly be one, because that is the shape the
// crash at 0x004B1B25 actually had: `node->item` was 0x0000020E, 526, which
// is a small integer sitting where an entity pointer belongs.
//
// Three cheap tests. Below 64K is the reserved null region no allocation ever
// lands in. Above 2 GB is kernel space for a 32-bit process without
// /LARGEADDRESSAWARE, and gta3.exe is not. And every C++ object with a vtable
// is at least 4-byte aligned, which 526 is not.
inline bool LooksLikeGameObject(uintptr_t value) {
	constexpr uintptr_t USER_MIN = 0x00010000;
	constexpr uintptr_t USER_MAX = 0x80000000;
	return value >= USER_MIN && value < USER_MAX && (value & 3) == 0;
}

// Is a node in CWorld::ms_listMovingEntityPtrs safe for CWorld::Process to
// dereference?
//
// `item` is the node's entity pointer and `vtable` is the first dword at that
// address, which the caller only reads once `LooksLikeGameObject(item)` has
// said it is worth reading. The vtable of a live entity always points into
// the image; a block that has been recycled for something else almost never
// does.
inline bool MovingListNodeIsSane(uintptr_t item, uintptr_t vtable) {
	return LooksLikeGameObject(item) && IsImageAddress(vtable);
}
// ---- the HUD: bitmap font, 2D sprites, world to screen --------------------
//
// Everything a nametag needs to be drawn with the game's own font and the
// game's own weapon icons rather than an imitation of them. Verified
// 2026-09-21 by walking the retail image and matching it against
// reference/re3 src/render/Hud.cpp, src/render/Font.cpp and
// src/render/Sprite.cpp statement for statement.
//
//   CHud::Initialise (0x005048F0) is re3 Hud.cpp:179-205. It pushes "hud"
//   (0x005FDB28) and "MODELS/HUD.TXD" (0x005FDB2C) into the txd store, then:
//     mov  ebp,0095CB9Ch
//     mov  eax,[ebx*8+005FDA70h] / mov edx,[ebx*8+005FDA74h]   WeaponFilenames
//     mov  ecx,ebp / call 0051EA70              CSprite2d::SetTexture(n, mask)
//     inc  ebx / add ebp,4 / cmp ebx,17h
//   so Sprites lives at 0x0095CB9C, a CSprite2d is 4 bytes wide (it is one
//   RwTexture*), and there are 23 of them. CHud::Shutdown (0x00504C50) walks
//   the same base with the same stride and the same count.
//
//   CHud::Draw (0x005052A0) opens with re3 Hud.cpp:320-329 exactly:
//     push 1 / call 00492F60                    CPad::GetPad(1)
//     cmp  word [eax+18h],0                     NewState.Start
//     cmp  word [eax+42h],0                     OldState.Start (0x2A + 0x18)
//     mov  byte [0095CD89h],al                  m_Wants_To_Draw_Hud = !it
//     cmp  byte [0095CD5Bh],1                   CReplay::Mode == PLAYBACK
//     cmp  byte [0095CD89h],1 / test al,al      && !TheCamera.m_WideScreenOn
//   with al loaded from 0x006FAD68, which is TheCamera + 0x70. Its one and
//   only caller is Render2dStuff, at 0x0048E420, and the four calls after it
//   are OnscnTimer / CMessages::Display / CDarkel::DrawMessages /
//   CGarages::PrintMessages / CPad::PrintErrorMessage / CFont::DrawFonts, in
//   re3 main.cpp:1510-1519 order.
//
//   CFont::Initialise's tail (0x00500B00-0x00500B90) calls sixteen setters in
//   re3 Font.cpp:127-143 order, which is what names them one by one, and each
//   setter body writes a different member of the struct at 0x008F317C. That
//   struct matches re3's CFontDetails field for field, so the setter list
//   below is proved twice over: by the order they are called in and by the
//   offset each one writes.
//
//   CFont::InitPerFrame (0x00500BE0) is re3 Font.cpp:105-112:
//     mov eax,[0095CC04h] / push 1Eh / call 0051EB70 / mov [008F31B4h],eax
//     mov eax,[0095CC08h] / push 0Fh / call 0051EB70
//     mov eax,[0095CC0Ch] / push 0Fh / call 0051EB70
//   i.e. Details.bank = GetBank(30, Sprite[0].m_pTexture) and two more banks
//   of 15. This was the "high confidence, unverified" row in
//   docs/addresses-unverified.md; it holds, so it lives here now.
//
//   CSprite::CalcScreenCoors (0x0051C3A0) is re3 Sprite.cpp:37-58:
//     fld [008E2DC4h] / fld [009434F0h]         far clip, near clip
//     push 7095F0h / call 004BA4D0              TheCamera.m_viewMatrix * in
//     fld 1.0f / fadd nearclip / fcomp z        z <= nearZ + 1 -> false
//     fcom farclip / cmp byte [esp+38h],0       farclip argument
//     fld 1.0f / fdiv z                         recip
//     fild [008F436Ch] / fmul / fmul [edi]      out->x *= SCREEN_WIDTH*recip
//     fild [008F4370h] / fmul / fmul [edi+4]    out->y *= SCREEN_HEIGHT*recip
//     fild width  -> [esi]                      *outw
//     fild height -> [ebx]                      *outh
//     fld [005FF280h] / fdiv [005FBC6Ch]        DefaultFOV / CDraw::ms_fFOV
//   It is __cdecl (plain ret) and takes five dwords.

constexpr uintptr_t CHud__Draw     = 0x005052A0;   // __cdecl, no arguments
constexpr uintptr_t CHud__Sprites  = 0x0095CB9C;   // CSprite2d[23]
constexpr size_t    SIZEOF_SPRITE2D          = 0x04;
constexpr size_t    NUM_HUD_SPRITES          = 23;
constexpr uintptr_t CHud__m_Wants_To_Draw_Hud = 0x0095CD89;   // bool

// TheCamera + this is the byte CHud::Draw tests before drawing anything.
// Cutscenes and the widescreen bars set it.
constexpr size_t CAMERA_WIDESCREEN_ON = 0x70;   // -> 0x006FAD68

// A HUD sprite index IS the eWeaponType: Hud.cpp:539 is
// `Sprites[WeaponType].Draw(...)` and the retail does the same indexing off
// the value it read out of m_weapons[m_currentWeapon] (0x00506216,
// `lea ebx,[ebx*4] / add ebx,0095CB9Ch`). 0..12 are the twelve weapons plus
// the detonator; 13 and 14 are blank entries.
constexpr uint8_t HUD_SPRITE_LAST_WEAPON = 12;

// CSprite2d::Draw(const CRect &r, const CRGBA &col, float u0, float v0,
//                 float u1, float v1, float u2, float v2, float u3, float v3)
// __thiscall, ret 28h. The retail call at 0x0050625E passes the rect pointer
// first and the colour pointer second, and the callee hands both straight to
// CSprite2d::SetVertices (0x0051F220) with the colour repeated four times,
// which is re3 Sprite2d.cpp's Draw.
constexpr uintptr_t CSprite2d__Draw          = 0x0051ED90;
constexpr uintptr_t CSprite2d__SetRenderState = 0x0051F950;
constexpr uintptr_t CSprite2d__DrawBank      = 0x0051EC50;
constexpr uintptr_t CSprite2d__GetBank       = 0x0051EB70;

// The UV inset the HUD itself draws a weapon icon with, read out of
// 0x0050622C-0x0050625C: 0.015 at 0x005FDBBC, 1.0 at 0x005FDB5C, 0.0 at
// 0x005FDB60. Same eight numbers as re3 Hud.cpp:539-551, same order.
constexpr float HUD_ICON_U0 = 0.015f, HUD_ICON_V0 = 0.015f;
constexpr float HUD_ICON_U1 = 1.0f,   HUD_ICON_V1 = 0.0f;
constexpr float HUD_ICON_U2 = 0.015f, HUD_ICON_V2 = 1.0f;
constexpr float HUD_ICON_U3 = 1.0f,   HUD_ICON_V3 = 1.0f;

// CRect is { left, bottom, right, top } in memory, in that order, even
// though its constructor takes (l, t, r, b). CRect::CRect at 0x004BA330 is
// four fld/fstp pairs and says so outright: arg1 -> +0x00, arg2 -> +0x0C,
// arg3 -> +0x08, arg4 -> +0x04. re3 math/Rect.h declares the same order.
// Build one by hand rather than calling the constructor and getting the
// argument order backwards.
constexpr size_t RECT_LEFT = 0x00, RECT_BOTTOM = 0x04, RECT_RIGHT = 0x08,
                 RECT_TOP = 0x0C;

// CSprite::CalcScreenCoors(const RwV3d *in, RwV3d *out, float *outw,
//                          float *outh, bool farclip) -> bool
// __cdecl. out->z comes back as the view-space depth, which is the only
// distance a nametag needs, so nothing here has to know where the camera is.
constexpr uintptr_t CSprite__CalcScreenCoors = 0x0051C3A0;
constexpr uintptr_t CDraw__ms_fFarClipZ      = 0x008E2DC4;
constexpr uintptr_t CDraw__ms_fNearClipZ     = 0x009434F0;
constexpr uintptr_t CDraw__ms_fFOV           = 0x005FBC6C;
constexpr uintptr_t CCamera__m_viewMatrix    = 0x007095F0;

// RsGlobal, from re3 skel/skeleton.h: appName, width, height, maximumWidth,
// maximumHeight, maxFPS, quit. The analysis pass already had quit at
// 0x008F4378, which puts the struct at 0x008F4360 and the two members below
// where CalcScreenCoors and every SCREEN_SCALE_* in the HUD read them.
// SCREEN_WIDTH is the int at 0x008F436C; the HUD's own scaling is
// `value * SCREEN_WIDTH / 640` (1/640 = 0.0015625 at 0x005FDB58) on x and
// `value * SCREEN_HEIGHT / 448` (1/448 at 0x005FDB4C) on y, so the retail
// build does scale its HUD with the resolution.
constexpr uintptr_t RsGlobal__maximumWidth  = 0x008F436C;   // int32
constexpr uintptr_t RsGlobal__maximumHeight = 0x008F4370;   // int32
constexpr float     HUD_REF_WIDTH  = 640.0f;
constexpr float     HUD_REF_HEIGHT = 448.0f;

// CFont. Details is the struct every setter writes into; the offsets in the
// comments are from re3's CFontDetails and each one is witnessed by the
// setter beside it.
constexpr uintptr_t CFont__Details      = 0x008F317C;
constexpr uintptr_t CFont__Sprite       = 0x0095CC04;   // CSprite2d[3]
constexpr uintptr_t CFont__InitPerFrame = 0x00500BE0;
constexpr uintptr_t CFont__DrawFonts    = 0x00501B50;   // recorded, not called

// PrintString(float x, float y, wchar *s), __cdecl, three dwords. It returns
// immediately if the first character is '*' (cmp word [esi],2Ah at
// 0x00500F64) and it treats '~' as the start of a colour token, so a nickname
// has to be filtered before it gets here.
constexpr uintptr_t CFont__PrintString = 0x00500F50;

// GetStringWidth(wchar *s, bool spaces) -> float in st0, __cdecl. The width
// comes back in screen pixels with whatever scale is currently set, so set
// the style, the scale and prop on/off first. PrintString itself calls it
// this way at 0x00501005 (`push 0 / push esi / call 005018A0`).
constexpr uintptr_t CFont__GetStringWidth = 0x005018A0;

constexpr uintptr_t CFont__SetScale               = 0x00501B80;   // +0x04,+0x08
constexpr uintptr_t CFont__SetSlantRefPoint       = 0x00501BA0;   // +0x10,+0x14
constexpr uintptr_t CFont__SetSlant               = 0x00501BC0;   // +0x0C
constexpr uintptr_t CFont__SetColor               = 0x00501BD0;   // +0x00
constexpr uintptr_t CFont__SetJustifyOn           = 0x00501C60;   // +0x18
constexpr uintptr_t CFont__SetJustifyOff          = 0x00501C80;
constexpr uintptr_t CFont__SetCentreOn            = 0x00501C90;   // +0x19
constexpr uintptr_t CFont__SetCentreOff           = 0x00501CB0;
constexpr uintptr_t CFont__SetWrapx               = 0x00501CC0;   // +0x28
constexpr uintptr_t CFont__SetCentreSize          = 0x00501CD0;   // +0x2C
constexpr uintptr_t CFont__SetBackgroundOn        = 0x00501CE0;   // +0x1B
constexpr uintptr_t CFont__SetBackgroundOff       = 0x00501CF0;
constexpr uintptr_t CFont__SetBackgroundColor     = 0x00501D00;   // +0x24
constexpr uintptr_t CFont__SetBackGroundOnlyTextOn  = 0x00501D30; // +0x1C
constexpr uintptr_t CFont__SetBackGroundOnlyTextOff = 0x00501D40;
constexpr uintptr_t CFont__SetRightJustifyOn      = 0x00501D50;   // +0x1A
constexpr uintptr_t CFont__SetRightJustifyOff     = 0x00501D70;
constexpr uintptr_t CFont__SetPropOff             = 0x00501D90;   // +0x1D
constexpr uintptr_t CFont__SetPropOn              = 0x00501DA0;
constexpr uintptr_t CFont__SetFontStyle           = 0x00501DB0;   // +0x34, int16
constexpr uintptr_t CFont__SetRightJustifyWrap    = 0x00501DC0;   // +0x30
constexpr uintptr_t CFont__SetAlphaFade           = 0x00501DD0;   // +0x20
constexpr uintptr_t CFont__SetDropShadowPosition  = 0x00501E70;   // +0x3C, int16
constexpr uintptr_t CFont__SetDropColor           = 0x00501DE0;   // +0x3E
// Details.bank sits at +0x38 between style and dropShadowPosition, which is
// what CFont::DrawFonts reads (`mov eax,[008F31B4h]` at 0x00501B50).

// Details.wrapX. CFont::PrintString starts a new line as soon as x passes it
// (`fld [008F31A4h]` at 0x00501040), and CFont::Initialise leaves it at a flat
// 640.0f whatever the resolution is (0x00500B10 pushes the constant at
// 0x005FD708). So anything printing past x=640 on a wider screen has to raise
// it first, and put it back if it does.
constexpr size_t FONTDETAILS_WRAPX = 0x28;

// re3 Font.h's enum. FONT_HEADING is the face the clock, the money and the
// health counter are drawn in, and the retail proves the value: the
// DrawHealth setup at 0x005063E8 is `push 2 / call 00501DB0`.
constexpr int16_t FONT_BANK    = 0;
constexpr int16_t FONT_PAGER   = 1;
constexpr int16_t FONT_HEADING = 2;

// A glyph cell in FONT_BANK/FONT_HEADING is 32*scaleX wide and 40*scaleY*0.5
// tall in screen pixels (re3 Font.cpp CFont::PrintChar). Line height is the
// only one a caller needs.
constexpr float FONT_CELL_HEIGHT = 20.0f;

// SetAlphaFade is multiplied into whatever SetColor is handed next, and
// CHud::Draw leaves it at whatever its zone-name fade last computed
// (0x0050939C). Anything that prints its own text has to set it.
constexpr float FONT_ALPHA_OPAQUE = 255.0f;

// ---- can this point see that point ----------------------------------------
//
// CWorld has two ray tests and they are not interchangeable.
// ProcessLineOfSight walks every sector the line crosses to find the *nearest*
// hit and fills in a CColPoint and the entity that owns it.
// GetIsLineOfSightClear answers yes or no and returns the moment anything
// blocks. For "is this player behind something" the second one is the right
// tool and it is several times cheaper on a blocked line, which is the case
// that happens in a city.
//
//   CWorld::GetIsLineOfSightClear (0x004AEAA0) is re3 World.cpp:462-560:
//     push ebx/esi/edi/ebp / sub esp,260h
//     cmp word [0095CC64h],0FFFFh / inc it, or call 004B1F60 and set it to 1
//                                            AdvanceCurrentScanCode, inlined
//     mov eax,[esp+274h] / fld [eax]         point1.x, i.e. argument 1
//     fmul 0.025 / fadd 50.0                 GetSectorIndexX: x/40 + 2000/40,
//                                            which agrees with the
//                                            WORLD_MIN_XY and SECTOR_SIZE_XY
//                                            already recorded above
//   and the four-branch sector walk after it calls
//   GetIsLineOfSightSectorClear seventeen times, once per arm and once per
//   step of the diagonal. All 57 call sites in the image clean `add esp,24h`,
//   which is the nine arguments re3 declares. One of them, at 0x0042BC51,
//   pushes six zeroes then `push 1 / push ecx / push eax`, i.e.
//   (p1, p2, true, false, false, false, false, false) with a defaulted ninth,
//   the same shape as re3 PathFind.cpp:1462.
//
//   CWorld::GetIsLineOfSightSectorClear (0x004B2000) is re3 World.cpp:563-598:
//     cmp byte [esp+0Ch],0                   checkBuildings
//     push 0 / push eax / push ebp / push esi / call 004B2160
//     lea eax,[esi+4]                        the BUILDINGS_OVERLAP list
//     cmp byte [esp+1Ch],0 / lea eax,[esi+10h]  checkVehicles, list index 4
//   so a CPtrList is 4 bytes and the lists are in re3's eEntityList order.
//
// The two flags worth understanding before choosing them, both from
// GetIsLineOfSightSectorListClear (0x004B2160), re3 World.cpp:601-622:
//   ignoreSeeThrough goes straight to CCollision::TestLineOfSight, which then
//     skips surfaces the collision data marks see-through. Glass and railings.
//   ignoreSomeObjects skips anything CameraToIgnoreThisObject says yes to,
//     which is every temporary object plus every object flagged
//     m_bCameraToIgnoreThisObject. That flag is the level's own list of props
//     that are not allowed to get between you and what you are looking at.
// It also skips entities with bUsesCollision clear, and it stamps
// m_scanCode on everything it touches.
constexpr uintptr_t CWorld__GetIsLineOfSightClear          = 0x004AEAA0;
constexpr uintptr_t CWorld__GetIsLineOfSightSectorClear    = 0x004B2000;
constexpr uintptr_t CWorld__GetIsLineOfSightSectorListClear = 0x004B2160;
constexpr uintptr_t CWorld__ClearScanCodes                 = 0x004B1F60;

// **Refuted, 2026-09-22.** This line used to read
// `CWorld__ProcessLineOfSight = 0x004B0DE0`, on the evidence that the function
// takes 11 arguments (`add esp,2Ch` at every call site) and opens with the
// same inlined AdvanceCurrentScanCode. Both are true and neither separates it
// from `CWorld::ProcessVerticalLine`, which also takes 11
// (point1, z2, point, entity, six bools, poly) and also advances the scan
// code. Three things say it is the vertical one:
//
//   - it reads its *first* argument only and derives one sector from it
//     (`mov esi,[esp+50h] / fld [esi] / fmul 0.025 / fadd 50.0 / fistp`, then
//     the same for [esi+4]). A line between two points needs the sector of
//     both ends and a walk between them; a vertical line needs one;
//   - its frame is 0x40 bytes with three saved registers. The real
//     ProcessLineOfSight's is 0x260 with four, the size the sector walk needs
//     and the same shape as GetIsLineOfSightClear above;
//   - CWeapon::ProcessLineOfSight (0x00564C00) forwards eleven of its
//     thirteen arguments to 0x004AF970 and to nothing else, and re3
//     Weapon.cpp:2301-2304 says that call is CWorld::ProcessLineOfSight.
//
// Nothing in the tree had used the old constant, so this is a trap that was
// never sprung rather than a bug that was fixed. The names are now what the
// functions are.
constexpr uintptr_t CWorld__ProcessVerticalLine = 0x004B0DE0;
constexpr uintptr_t CWorld__ProcessLineOfSight  = 0x004AF970;

// The camera's own position, for casting from. CCamera derives from
// CPlaceable, so it is the matrix position at the usual offs::POSITION:
// CSprite::CalcHorizonCoors (0x0051C4A0) opens `mov eax,6FAD2Ch` and then
// `fld [eax] / fld [eax+4]` with the z forced to zero, which is re3
// Sprite.cpp:19-21's `CVector p = TheCamera.GetPosition(); p.z = 0.0f;`.
// 0x006FAD2C is TheCamera + 0x34.
//
// The engine casts a ray from exactly here for the same kind of question: re3
// AudioLogic.cpp:3646 decides whether a sound is occluded with
// GetIsLineOfSightClear(TheCamera.GetPosition(), soundPos, true, ...).

// ---- the radar: CRadar's blip table ---------------------------------------
//
// Everything needed to put something on the minimap through the game's own
// radar rather than by painting a sprite over the top of it. Verified
// 2026-09-22 against the retail image and matched against reference/re3
// src/core/Radar.cpp statement for statement.
//
// The route in was the usual one: walk the script opcode that already does
// the thing. COMMAND_ADD_BLIP_FOR_CHAR is opcode 391, so the 300..399
// dispatcher table at 0x005EEE30 (base opcode 304) entry 87 is the handler,
// at 0x0044079E, and it is re3 Script3.cpp:1380-1390 call for call:
//
//     call 004382E0                     CollectParameters(&m_nIp, 1)
//     mov  ecx,[008F2C60] / call 0043EB30   CPools::GetPedPool()->GetAt
//     call 00438460 / call 004A41C0     the handler's own "useless call" to
//                                       GetActualBlipArrayIndex
//     push 3 / push 1 / push eax / push 2
//     call 004A5640 / add esp,10h       SetEntityBlip(BLIP_CHAR, handle,
//                                       RADAR_TRACE_GREEN, BOTH), __cdecl
//     push 3 / push ebx / call 004A57E0 ChangeBlipScale(blip, 3)
//
// which is also where the game's own answer to "what does a person look like
// on the radar" comes from: colour 1, scale 3, no sprite.
//
// ---- the array, and its size, measured three separate ways ----------------
//
// CRadar::SetEntityBlip (0x004A5640) opens with the free-slot search, and it
// is the whole of the bound:
//
//     xor ecx,ecx / xor eax,eax
//     add eax,30h / inc ecx                     stride 0x30
//     cmp byte [eax + 006ED603h],0              m_bInUse
//     je  found
//     cmp ecx,20h / jb loop                     32 slots
//
// Confirmed twice more, independently:
//
//   - CRadar::Initialise (0x004A3EF0) clears the table with `add ebp,30h /
//     cmp ebx,20h / jl`, the same stride and the same count;
//   - CReplay's save and restore (0x0059632C, 0x00596874) both do
//     `push 600h / push 006ED5E0h / call memcpy`, and 0x600 is 32 * 0x30.
//
// re3's NUMRADARBLIPS is also 32, so this is one of the few places the
// decompilation and retail agree - but the number below is read off the
// binary, not off re3. It had to be: the last fixed-size array this project
// took from re3 was declared as 16 and was 12 in the retail build, and
// writing past it corrupted the engine's own stack.
//
// ---- and the array has no bounds check ------------------------------------
//
// If all 32 slots are in use the search loop falls out with ecx == 32 and
// SetEntityBlip writes the 33rd entry anyway. re3 guards that behind
// #ifdef FIX_BUGS; retail does not, so the write lands at
// 0x006ED5E0 + 32*0x30 = 0x006EDBE0, which is CDarkel::RegisteredKills - the
// uint16[200] kill register, zeroed eight words at a time by the loop at
// 0x00421310 and incremented per model id at 0x00421019. It does not crash.
// It quietly rewrites the kill-frenzy counters, and GetNewUniqueBlipIndex
// then hands back a handle for slot 32 that every later ChangeBlip* call
// happily writes through.
//
// So nothing may call SetEntityBlip without counting the free slots first.
// RADAR_TRACE_OVERFLOW_TARGET is here so a test can pin that arithmetic
// rather than trusting a comment.
constexpr uintptr_t CRadar__ms_RadarTrace = 0x006ED5E0;
constexpr size_t    SIZEOF_RADAR_TRACE    = 0x30;
constexpr size_t    NUM_RADAR_BLIPS       = 32;
constexpr uintptr_t CDarkel__RegisteredKills        = 0x006EDBE0;   // uint16[200]
constexpr uintptr_t RADAR_TRACE_OVERFLOW_TARGET     = 0x006EDBE0;

// re3's NUM_DEFAULT_MODELS, and read off the reset loop rather than off re3:
// CDarkel::ResetModelsKilledByPlayer at 0x00421310 clears eight words per
// iteration and stops at `cmp ax,0C8h` (0x00421318). 200 entries, uint16 each.
// The array has no bounds check anywhere - `inc word [eax*2+006EDBE0h]` at
// 0x00421019 is the whole of the increment - so anything that indexes it with
// a number that came off a socket has to bound it here first.
constexpr size_t NUM_DEFAULT_MODELS = 200;

// sRadarTrace, every member witnessed by the instruction that writes it
// inside SetEntityBlip. The absolute address in each comment is what the
// disassembly actually shows, since the retail build folds the base in.
constexpr size_t TRACE_COLOR         = 0x00;   // mov [edx+006ED5E0h],eax  arg3
constexpr size_t TRACE_BLIP_TYPE     = 0x04;   // mov [edx+006ED5E4h],eax  arg1
constexpr size_t TRACE_ENTITY_HANDLE = 0x08;   // mov [edx+006ED5E8h],eax  arg2
constexpr size_t TRACE_POS_2D        = 0x0C;   // CVector2D, coord blips only
constexpr size_t TRACE_POS           = 0x14;   // CVector,   coord blips only
constexpr size_t TRACE_BLIP_INDEX    = 0x20;   // GetNewUniqueBlipIndex's word
constexpr size_t TRACE_DIM           = 0x22;   // mov byte [..006ED602h],1
constexpr size_t TRACE_IN_USE        = 0x23;   // mov byte [..006ED603h],1
constexpr size_t TRACE_RADIUS        = 0x24;   // mov dword [..006ED604h],3F800000h
constexpr size_t TRACE_SCALE         = 0x28;   // mov word [..006ED608h],1
constexpr size_t TRACE_BLIP_DISPLAY  = 0x2A;   // mov word [..006ED60Ah],ax  arg4
constexpr size_t TRACE_RADAR_SPRITE  = 0x2C;   // mov word [..006ED60Ch],0
static_assert(TRACE_RADAR_SPRITE + 2 <= SIZEOF_RADAR_TRACE,
              "sRadarTrace members must fit the stride the engine strides by");

// eBlipType. BLIP_CHAR is 2 and BLIP_CAR is 1, and both are witnessed in the
// engine's own destructors rather than taken from re3's enum order:
//   ~CPed     (0x004C50D0) does CWorld::Remove, then
//             `mov ecx,[008F2C60h] / call 0043EB70`, then
//             `push eax / push 2 / call 004A56C0`
//   ~CVehicle (0x00551060) does `mov ecx,[009430DCh] / call 00429050`, then
//             `push eax / push 1 / call 004A56C0`
//
// That pair also settles the thing that makes this feature nearly free.
// CPools::GetPedRef (0x004A1A80) is *literally* `mov ecx,[008F2C60h] / call
// 0043EB70 / ret`, the same pool and the same function ~CPed uses, so **the
// handle a BLIP_CHAR entry holds is byte for byte what CPools::GetPedRef
// returns** - which is exactly what RemotePlayer::poolHandle already is. And
// the engine clears an entity's blip in the destructor, so a ped CoopIII
// destroys takes its blip with it without being asked.
constexpr uint32_t BLIP_NONE          = 0;
constexpr uint32_t BLIP_CAR           = 1;
constexpr uint32_t BLIP_CHAR          = 2;
constexpr uint32_t BLIP_OBJECT        = 3;
constexpr uint32_t BLIP_COORD         = 4;
constexpr uint32_t BLIP_CONTACT_POINT = 5;

// eBlipDisplay.
constexpr uint16_t BLIP_DISPLAY_NEITHER     = 0;
constexpr uint16_t BLIP_DISPLAY_MARKER_ONLY = 1;
constexpr uint16_t BLIP_DISPLAY_BLIP_ONLY   = 2;
constexpr uint16_t BLIP_DISPLAY_BOTH        = 3;

// The trace colours, which are indices into GetRadarTraceColour and not
// RGBA. 0..6, proved by that function's own jump table (`cmp eax,6 / ja` then
// `jmp [eax*4 + 005F71F4h]`) and by the RGBA constants each arm returns,
// which match re3 Radar.cpp:909-955 exactly - e.g. GREEN is 0x5FA06AFF when
// m_bDim is set and 0x007F00FF when it is not.
//
// Note the inversion, which is easy to get backwards: GetRadarTraceColour's
// second argument is m_bDim, and a *set* m_bDim selects the lighter of the
// two. SetEntityBlip leaves m_bDim = 1, so the lighter pair is the default
// and it is what every script blip in the game uses.
constexpr uint32_t RADAR_TRACE_RED        = 0;
constexpr uint32_t RADAR_TRACE_GREEN      = 1;
constexpr uint32_t RADAR_TRACE_LIGHT_BLUE = 2;
constexpr uint32_t RADAR_TRACE_GRAY       = 3;
constexpr uint32_t RADAR_TRACE_YELLOW     = 4;
constexpr uint32_t RADAR_TRACE_MAGENTA    = 5;
constexpr uint32_t RADAR_TRACE_CYAN       = 6;

// eRadarSprite. Only the four the draw special-cases are named, because the
// retail proves those four and nothing else: DrawBlips' second loop opens
// `mov bx,[ebp+006ED60Ch]` and then skips 2, 0x11, 0x12 and 0x14 - which is
// BOMB, SAVE, SPRAY and WEAPON in re3's enum order, so the numbering holds.
// A blip with RADAR_SPRITE_NONE is the plain coloured square, which is what
// every mission target in the game looks like.
constexpr uint16_t RADAR_SPRITE_NONE   = 0;
constexpr uint16_t RADAR_SPRITE_BOMB   = 2;
constexpr uint16_t RADAR_SPRITE_SAVE   = 17;
constexpr uint16_t RADAR_SPRITE_SPRAY  = 18;
constexpr uint16_t RADAR_SPRITE_WEAPON = 20;

// All __cdecl. GetActualBlipArrayIndex is the gate every ChangeBlip* goes
// through: it returns -1 unless `handle >> 16` still matches the slot's
// m_BlipIndex, which is what makes a handle stale once the slot is reused.
// It does NOT range-check the low half (`and eax,0FFFFh` and straight into
// `[edx*8 + 006ED600h]`), so a fabricated handle reads and writes wherever
// its low word points. Only ever pass one SetEntityBlip returned.
constexpr uintptr_t CRadar__SetEntityBlip           = 0x004A5640;
constexpr uintptr_t CRadar__SetCoordBlip            = 0x004A5590;
constexpr uintptr_t CRadar__ClearBlip               = 0x004A5720;
constexpr uintptr_t CRadar__ClearBlipForEntity      = 0x004A56C0;
constexpr uintptr_t CRadar__ChangeBlipColour        = 0x004A5770;
constexpr uintptr_t CRadar__ChangeBlipScale         = 0x004A57E0;
constexpr uintptr_t CRadar__ChangeBlipDisplay       = 0x004A5810;
constexpr uintptr_t CRadar__SetBlipSprite           = 0x004A5840;
constexpr uintptr_t CRadar__GetActualBlipArrayIndex = 0x004A41C0;
constexpr uintptr_t CRadar__GetNewUniqueBlipIndex   = 0x004A4180;
constexpr uintptr_t CRadar__SetRadarMarkerState     = 0x004A5C60;
constexpr uintptr_t CRadar__GetRadarTraceColour     = 0x004A5BB0;

// Recorded rather than called. DrawMap and DrawBlips are the two halves of
// the radar the HUD runs, in that order, from CHud::Draw (0x0050838D and
// 0x00508499). Initialise is called from CGame::Initialise (0x0048C1DD),
// CGame::ReInitialise (0x0048C4F6) and LoadAllRadarBlips (0x004A6F35), and
// it wipes every m_bInUse in the table - which is why nothing may trust a
// blip handle it is holding across a game load. Ask the table.
constexpr uintptr_t CRadar__DrawMap     = 0x004A4200;
constexpr uintptr_t CRadar__DrawBlips   = 0x004A42F0;
constexpr uintptr_t CRadar__Initialise  = 0x004A3EF0;

// How far the radar reaches, in metres, which is the distance at which
// LimitRadarPoint (0x004A4F30) stops moving a blip and starts pinning it to
// the rim. DrawMap computes it: 120.0f (0x42F00000, written at 0x004A426A
// and 0x004A42B4) on foot, ramping to 350.0f (0x43AF0000 at 0x004A42A8) in a
// car, over the speed band 0.3 to 0.9 at 0x005F70F8 and 0x005F70FC.
constexpr uintptr_t CRadar__m_radarRange    = 0x008E281C;   // float
constexpr float     RADAR_RANGE_ON_FOOT_M   = 120.0f;
constexpr float     RADAR_RANGE_AT_SPEED_M  = 350.0f;

// The byte DrawBlips tests before drawing the 3D marker half of a
// BLIP_DISPLAY_BOTH or MARKER_ONLY blip: `cmp byte [0095CD87h],0 / je` at
// 0x004A490A. It is CTheScripts::DbgFlag, and it is off in a retail build,
// which is why BLIP_DISPLAY_BOTH and BLIP_DISPLAY_BLIP_ONLY look identical in
// a normal game and why asking for BLIP_ONLY costs nothing.
constexpr uintptr_t CTheScripts__DbgFlag = 0x0095CD87;

// ---- the radar: how the game draws the player's own arrow -----------------
//
// The local player is not in ms_RadarTrace and never was. CRadar::DrawBlips
// draws them separately, at the centre of the radar, before it walks the
// table at all, and it is a *rotating* sprite - which is where the arrowhead
// and the direction it points both come from. Verified 2026-09-22 against
// the retail image; every address below is witnessed by the instruction that
// uses it, and the whole preamble is quoted so the order is not guesswork.
//
// CRadar::DrawBlips (0x004A42F0) opens:
//
//     cmp  byte [006FAD68h],1 / je ret       TheCamera.m_WideScreenOn
//     cmp  byte [0095CD89h],0 / jne draw     CHud::m_Wants_To_Draw_Hud
//     ... six RwRenderStateSet calls (0x005A43C0) ...
//     lea  eax,[esp+50h] / lea esi,[esp+68h]
//     mov  dword [esp+68h],0 / mov [esp+6Ch],0       CVector2D in = (0,0)
//     push esi / push eax / call 004A5040 / pop / pop
//                                           TransformRadarPointToScreenSpace
//     call 004A1220                         FindPlayerHeading(), st0
//     movzx eax,byte [006FAD6Eh] / imul eax,eax,69h
//     cmp  word [eax*4 + 006FAEA8h],1       TheCamera.Cams[ActiveCam].Mode
//     jne  else                             MODE_TOPDOWN
//   then:                                   angle = PI + FindPlayerHeading()
//     fld  [005F710Ch] / fadd [esp+24h]     005F710C is 3.14159274
//     push 0FFh / push eax / fstp [esp]
//     push [esp+54h] / push [esp+54h]       out.y, out.x
//     push 008F6268h                        &CRadar::CentreSprite
//   else:                                   angle = heading - (PI + fwd.Heading())
//     fldz / fld [006FAD0Ch] / fchs / fld [006FAD10h] / fpatan
//                                           Atan2(-fwd.x, fwd.y)
//     fld  [005F710Ch] / fadd st(1) / fsubr [esp+24h]
//     push 0FFh / push eax / ... / push 008F6268h
//     call 004A5D10 / add esp,14h           DrawRotatingRadarSprite, __cdecl,
//                                           (sprite, x, y, angle, alpha)
//
// and then, for the N marker, the same four calls every blip in the table
// goes through - which is the sequence this feature copies:
//
//     fld  [006299B8h] / fadd 0.0           vec2DRadarOrigin.x
//     fld  [005F7114h] / fmul [008E281Ch]   M_SQRT2 * m_radarRange
//     fadd [006299BCh]                      + vec2DRadarOrigin.y
//     push ebx / push eax / call 004A50D0   TransformRealWorldPointToRadarSpace
//     push eax / call 004A4F30              LimitRadarPoint, returns dist in st0
//     push edx / push eax / call 004A5040   TransformRadarPointToScreenSpace
//     push 0FFh / push y / push x / push 0Eh
//     call 004A5EF0                         DrawRadarSprite(RADAR_SPRITE_NORTH)
//
// The blip loop at 0x004A4750 is the same four, and it is where the alpha
// comes from as well:
//
//     call 004A50D0 / pop / pop
//     lea  eax,[esp+48h] / push eax / call 004A4F30 / pop ecx
//     fstp [esp+34h] / push [esp+34h] / call 004A4F90 / mov [esp+0Ch],al
//                                           CalculateBlipAlpha(dist) -> uint8
//     push esi / push eax / call 004A5040
//     mov  ax,[ebp+006ED60Ch] / test ax,ax / jne sprite
//     ... ShowRadarTrace (004A5870) for RADAR_SPRITE_NONE ...
//   sprite:
//     push [esp+8] / push y / push x / push eax / call 004A5EF0
//
// All five are __cdecl (the caller pops), and the two transforms take
// (out, in) in that order - proved by DrawBlips passing the freshly zeroed
// CVector2D as the *second* argument to 0x004A5040 and reading the result
// out of the first.
constexpr uintptr_t CRadar__TransformRealWorldPointToRadarSpace = 0x004A50D0;
constexpr uintptr_t CRadar__TransformRadarPointToScreenSpace    = 0x004A5040;
constexpr uintptr_t CRadar__LimitRadarPoint          = 0x004A4F30;   // -> float
constexpr uintptr_t CRadar__CalculateBlipAlpha       = 0x004A4F90;   // -> uint8
constexpr uintptr_t CRadar__DrawRadarSprite          = 0x004A5EF0;
constexpr uintptr_t CRadar__DrawRotatingRadarSprite  = 0x004A5D10;
constexpr uintptr_t CRadar__ShowRadarTrace           = 0x004A5870;
constexpr uintptr_t FindPlayerHeading                = 0x004A1220;   // -> float
constexpr uintptr_t CRadar__vec2DRadarOrigin         = 0x006299B8;   // CVector2D

// The centre arrow's sprite, and it is NOT a special case hiding outside the
// sprite table - it is RadarSprites[4]. Both facts are witnessed:
//
//   CRadar::LoadTextures (0x004A4074) does
//     mov ecx,008F6268h / push 005F7020h / call 0051EA40
//   and the string at 0x005F7020 is "radar_centre", so CentreSprite is the
//   CSprite2d at 0x008F6268;
//
//   the pointer table at 0x005F6CA4 reads 0, 008F1A40, 008F5FB4, 00885B24,
//   008F6268, ... and its entry 14 is 008F6274, which is the sprite
//   DrawBlips draws the compass with as `push 0Eh / call 004A5EF0`. So the
//   table is CRadar::RadarSprites, entry 4 is CentreSprite and entry 14 is
//   NorthSprite - re3's enum order, measured rather than trusted.
//
// Knowing it is in the table is what rules out the cheap version of this
// feature. A blip with m_eRadarSprite = 4 *would* draw the arrowhead, but
// DrawBlips reaches DrawRadarSprite for it, not DrawRotatingRadarSprite, so
// it would come out pointing the same way forever. The rotation only exists
// on the path the local player's own marker takes.
constexpr uintptr_t CRadar__CentreSprite  = 0x008F6268;
constexpr uintptr_t CRadar__RadarSprites  = 0x005F6CA4;   // CSprite2d*[21]
constexpr uint16_t  RADAR_SPRITE_CENTRE   = 4;
constexpr uint16_t  RADAR_SPRITE_NORTH    = 14;

// CSprite2d::Draw(float x1, float y1, float x2, float y2, float x3, float y3,
//                 float x4, float y4, const CRGBA &col)
// __thiscall, `ret 24h` - nine dwords, the colour last. It pushes that one
// pointer four times into CSprite2d::SetVertices (0x0051F070), calls
// SetRenderState (0x0051F950) and then RwIm2DRenderPrimitive with 4 vertices
// as a TRIFAN. This is the overload DrawRotatingRadarSprite ends in, and the
// *only* reason CoopIII calls it directly rather than calling
// DrawRotatingRadarSprite: that function builds its CRGBA in place at
// 0x004A5E98 as `push alpha / push 0FFh / push 0FFh / push 0FFh`, so its
// colour is hardcoded white and only the alpha is a parameter.
constexpr uintptr_t CSprite2d__DrawFourCorners = 0x0051EE40;

// DrawRotatingRadarSprite's own geometry, read out of 0x004A5D10 rather than
// out of re3, because retail differs from the decompilation in one way that
// matters. The half-extents are
//
//     fild [008F436Ch] / fmul [005F7148h] / fmul [005F7174h]   x
//     fild [008F4370h] / fmul [005F7158h] / fmul [005F7174h]   y
//
// i.e. SCREEN_SCALE_X(8.0) and SCREEN_SCALE_Y(8.0) - but each is then put
// through `fnstcw / or byte [esp+5],0Ch / fistp qword / fldcw`, which is the
// compiler's round-toward-zero truncation to an integer. re3 keeps them as
// floats. So the retail arrow's half-extents are whole pixels, and anything
// drawn beside it has to truncate too or it will be a fraction of a pixel
// bigger at most resolutions.
//
// The corners are four points at i*HALFPI + (angle - PI/4), with
// x = cx + sin(a) * halfX and y = cy + cos(a) * halfY, handed to
// CSprite2d::Draw in the order 3, 2, 0, 1.
//
//     005F7148  0.0015625      1/640      005F7158  0.002232143  1/448
//     005F7174  8.0                       005F7178  0.78539819   PI/4
//     005F717C  1.5707964      HALFPI     005F710C  3.1415927    PI
//     005F7110  0.0                       005F711C  1.0
//
// The two reciprocals are read at runtime rather than compiled in. They are
// the same two constants TransformRadarPointToScreenSpace uses for the whole
// radar, and a mod that rescales the HUD by patching them has to move our
// arrow with the game's or the two stop matching - which is the entire point
// of drawing this one.
constexpr uintptr_t RADAR_RECIP_REF_WIDTH  = 0x005F7148;   // float, 1/640
constexpr uintptr_t RADAR_RECIP_REF_HEIGHT = 0x005F7158;   // float, 1/448
constexpr float     RADAR_SPRITE_HALF_REF  = 8.0f;         // 0x005F7174

// TransformRadarPointToScreenSpace's own numbers, for the record and for the
// install-time sanity check. Retail is re3's non-FIX_BUGS form: x is
// (in.x + 1) * 0.5 * SCREEN_SCALE_X(94) + 40, with RADAR_LEFT *not* scaled,
// and y is (1 - in.y) * 0.5 * SCREEN_SCALE_Y(76) + (height - 123/448*height).
constexpr float RADAR_LEFT_PX   = 40.0f;    // 0x005F7154
constexpr float RADAR_WIDTH_REF = 94.0f;    // 0x005F714C
constexpr float RADAR_HEIGHT_REF = 76.0f;   // 0x005F715C
// ---- where a bullet goes, and the trail it leaves -------------------------
//
// Four symbols, and they exist because of one report: a remote player's
// bullet trail is drawn in a different place on each screen. The question
// "does the trail come from the source CoopIII hands the engine, or from the
// muzzle of the ped it happens to be rendering" is answered here, and the
// answer is the first one - which is why the fix is about the *direction*.
//
// The trail is CBulletTraces, and it is world-space from end to end. Nothing
// in it reads a screen dimension: AddTrace stores two CVectors, CBulletTrace
// ::Update walks the near end toward the far one by 0.8 m a frame, and Render
// builds the quad's width as CrossProduct(TheCamera.GetForward(), sup - inf)
// normalised and divided by 20 - a fixed 5 cm half-width in metres. A trail
// cannot move with the resolution. Only its thickness in pixels can.
//
//   CBulletTraces::AddTrace   0x00518E90   __cdecl(CVector *start, CVector *target)
//     `xor eax,eax` then `cmp eax,10h / cmp byte [ebx+72B1D0h],0 / add ebx,1Ch`
//     is the free-slot search: sixteen entries, 28 bytes each, m_bInUse at
//     +0x18. `lea ebx,[eax+eax*8] / lea ebx,[ebx+ebx*2] / add ebx,eax` is that
//     28 again as a multiply. It then copies three floats to +0x00 and three
//     to +0x0C, sets +0x18, clears +0x19 and finishes
//     `call CGeneral::GetRandomNumber / and al,1Fh / add al,19h` into +0x1A,
//     which is re3 SpecialFX.cpp:284-298's `m_lifeTime = 25 + rand % 32`
//     exactly. Six call sites, and they are re3's six: two in DoBulletImpact,
//     two in FireShotgun, two in FireInstantHitFromCar.
//
//     Retail agrees with re3's NUMBULLETTRACES (16) here. It does not always
//     agree - the animation node array is twelve where re3 declares sixteen,
//     and that one crashed the game - so the fact is the `cmp eax,10h`, not
//     the header.
//
//   CWeapon::ProcessLineOfSight  0x00564C00  __cdecl, 13 arguments
//     A thunk and nothing else: it re-pushes eleven of its thirteen arguments
//     and drops argument 5 (type) and argument 6 (shooter) on the way to
//     CWorld::ProcessLineOfSight, which is re3 Weapon.cpp:2301-2304 statement
//     for statement. `add esp,2Ch` inside, `add esp,34h` at every call site.
//     Seven call sites and all seven are weapon fire: four in FireInstantHit
//     (its four branches), one each in FireShotgun, FireM16_1stPerson and
//     FireInstantHitFromCar. That makes it the one place in the engine where
//     every instant-hit path states the line it is about to test, before
//     anything has decided what the line hit.
//
//   CWeapon::DoBulletImpact      0x0055F950  __thiscall, `ret 1Ch`
//     (shooter, victim, source, target, point, ahead.x, ahead.y) - seven
//     stack dwords, which is the 1Ch, with `this` in ecx. Opens
//     `push ebx/esi/edi/ebp / sub esp,128h`, then GetWeaponInfo(m_eWeaponType)
//     off [ecx], then `cmp [esp+144h],0` for `if (victim)`, then
//     CGlass::WasGlassHitByBullet with the col point's three floats, then
//     `push eax / push ebx / call 00518E90` - AddTrace(source, &traceTarget).
//     re3 Weapon.cpp:855-1124.
//
//   CWeapon::DoDoomAiming        0x00562EB0  __cdecl(CEntity*, CVector*, CVector*)
//     `ret` with no immediate, so the caller cleans: re3 declares it static
//     (Weapon.h:54) and the binary agrees. Its fingerprint is the unused
//     `CEntity entity;` re3 keeps under #ifndef FIX_BUGS - a stack CEntity
//     constructed at 0x00473C30 and destructed at 0x00473E40 with nothing in
//     between. Then `[ebx+50h] and 7 cmp 3` (IsPed) and `[ebx+158h] shr 3
//     and 1` (bCrouchWhenShooting) for the early return, then
//     `(*target - *source).Magnitude()` for FindObjectsInRange. Exactly two
//     call sites, which are re3's two: FireInstantHit (0x0055DAA4) and
//     FireShotgun (0x00560E20). Everything it does is write `target->z`.
//
// **The two facts the whole tracer fix rests on**, both read out of
// FireInstantHit's fourth branch - the one every ped that is not the local
// player takes, and therefore the one every remote player takes:
//
//   1. The shot's direction is the ped's own matrix forward, flattened.
//      0x0055D9AC is `fld [ebp+14h] / fchs / fld [ebp+18h] / fpatan`, i.e.
//      Atan2(-fwd.x, fwd.y), and the sin/cos of that back out to exactly
//      normalise2D(fwd). Nothing in that branch reads an aim, a look
//      direction or a camera.
//
//   2. The shot is flat. 0x0055DA47 is `mov eax,[esp+98h]` (source.z) and
//      0x0055DA6F is `mov [esp+8Ch],eax` (target.z) - a dword copy, not an
//      arithmetic. Only DoDoomAiming above ever puts a slope on it, and that
//      is this machine's auto-aim at whatever ped it can see, not the
//      shooter's aim.
//
// Which is the whole bug: an observer replaying somebody else's shot got the
// origin off the wire and then derived the direction from a ped that is
// interpolated, whose heading is not their aim, and whose pitch is never
// applied at all (docs/protocol.md 1.8.3). DoDoomAiming is the seam that
// fixes it, because it is handed `target` as a pointer and the engine's own
// purpose for it is "move this shot".
//
// **A third fact, and it is why the direction is now sampled at AddTrace
// rather than at ProcessLineOfSight.** On the 3rd-person mouse camera branch
// the ray and the streak are not the same line, because the ray does not start
// at the barrel. 0x0055D898 builds `&src` at [esp+0E8h] and `&trgt` at
// [esp+0F4h] and hands both to Find3rdPersonCamTargetVector, whose mouse arm
// ends `source += Dot(pos - source, target) * target` - the muzzle projected
// onto the camera's own axis. The ProcessLineOfSight at 0x0055D907 is then
// passed that `src` as point1 (the `lea eax,[esp+114h]` at 0x0055D8FE is
// eleven pushes deep, so it is [esp+0E8h]), while the DoBulletImpact at
// 0x0055F73A is passed [esp+90h] - the local copy of CWeapon::Fire's
// fireSource, made at 0x0055D316 and never written again.
//
// So the engine tests one line and draws another, and the two are a parallel
// offset apart: however far the hand is off the camera axis. An observer given
// the *ray's* direction and told to fire from the *muzzle* reconstructs
// neither. Given the *trail's* direction it reconstructs the trail exactly,
// because both of its ends then come from the same two points.
//
// **And one retail bug found on the way**, which is what the shooter sees on
// their own screen. FireInstantHit's third branch - the local player using
// the 3rd-person mouse camera - calls ProcessLineOfSight with its own `src`
// and `trgt` locals (0x0055D898: `lea eax,[esp+0E8h] / lea edi,[esp+0F4h]`)
// and never writes the `target` local at [esp+84h]. The shared tail at
// 0x0055F709 then does `lea eax,[esp+84h]` and passes it to DoBulletImpact,
// whose no-victim arm is `AddTrace(source, target)`. So a mouse-aimed shot
// that hits nothing draws its trail from the muzzle to whatever was left in
// that stack slot. The other three branches all pass `&target` to
// ProcessLineOfSight itself (branch 1 at 0x0055D79D, its else at 0x0055D859,
// branch 4 at 0x0055DACD, all `lea ...,[esp+84h]` once the pushes are
// accounted for), so pointer identity between "the point2 the engine tested"
// and "the target it is about to draw" is an exact test for whether the
// branch that ran wrote it.
// **And the part of the report that was about the resolution after all**,
// which is worth writing down because the answer is "yes, but not to the
// question that was asked".
//
// Nothing in CBulletTraces reads a screen dimension, so a trail cannot move
// with the resolution - that stands. But the *aim* can, on the one branch a
// mouse-aiming player is always on, and the retail binary says so plainly.
// CCamera::Find3rdPersonCamTargetVector (0x0046B580) builds the shot's
// direction from the crosshair's screen fraction, and its horizontal half has
// one factor its vertical half does not:
//
//   0046B611  cmp  byte [0095CD23h],0      the widescreen preference
//   0046B61A  fld  [005F0A64h]             1.777778   (16:9)
//   0046B622  fld  [005F0A68h]             1.333333   (4:3)
//   0046B62E  fld  [ebp+19Ch]              m_f3rdPersonCHairMultX
//   0046B634  fsub [005F0464h]               - 0.5
//   0046B640  fmul [005F0A6Ch]               * 1.8
//   0046B646  fmul st,st(4)                  * that aspect     <-- X only
//   0046B648  fmul [005F0464h]               * 0.5
//   0046B64E  fld  [ebp+eax+254h]            * FOV
//
//   0046B662  fld  [005F0464h]             0.5
//   0046B668  fsub [ebp+1A0h]                - m_f3rdPersonCHairMultY
//   0046B66E  fmul [005F0A6Ch]               * 1.8
//   0046B674  fmul [005F0464h]               * 0.5
//   0046B67A  fmul st,st(3)                  * FOV             <-- no aspect
//
// Two things follow. The aspect ratio is **a two-valued constant picked by a
// menu toggle**, not the shape of the window: 16:9 if the widescreen option is
// on, 4:3 if it is off, at any resolution. And the crosshair itself is drawn
// at a fixed fraction of the screen, which in retail is 0.53 across and 0.40
// down - off centre horizontally, which is the only reason the factor bites at
// all. With the toggle not matching what is actually being rendered, every
// mouse-aimed shot leaves the barrel about 0.8 degrees to one side of the
// crosshair, always the same side: `(0.53 - 0.5) * 1.8 * 0.5 * FOV * (16/9 -
// 4/3)`. At a 70 degree FOV that is 15 cm at ten metres.
//
// That is world space, so it is the same on every screen and it is not why two
// machines disagree. It is, exactly, "el que dispara ve el trail de las balas
// hacia un lado". Retail's, not ours, and not fixable from here without
// deciding what the player's aspect ratio really is.
// Which is also the one thing on this install that another mod is very likely
// to have already changed: `docs/compat.md` lists ThirteenAG's Widescreen Fix
// in `_ESSENTIALS`, and making the game aim at a real aspect ratio instead of
// one of these two constants is exactly what that mod is for. So CoopIII does
// not assume either way - it reads the site at startup and says what it found.
// The seven bytes are the `cmp byte ptr ds:[0095CD23h],0` at the head of the
// selection; anything else there means somebody rewrote it.
constexpr uintptr_t CCamera__Find3rdPersonCamTargetVector = 0x0046B580;
constexpr uintptr_t CCamera__AimAspectSelect              = 0x0046B611;
constexpr uintptr_t CMenuManager__PrefsUseWideScreen      = 0x0095CD23;
constexpr uintptr_t kAimAspectWide                        = 0x005F0A64;   // 1.777778
constexpr uintptr_t kAimAspectStandard                    = 0x005F0A68;   // 1.333333
constexpr uint8_t   kAimAspectSelectBytes[]               = {0x80, 0x3D, 0x23,
                                                             0xCD, 0x95, 0x00, 0x00};

constexpr uintptr_t CBulletTraces__AddTrace     = 0x00518E90;
constexpr uintptr_t CBulletTraces__aTraces      = 0x0072B1B8;
constexpr size_t    SIZEOF_BULLETTRACE          = 0x1C;
constexpr int       NUM_BULLET_TRACES           = 16;
constexpr uintptr_t CWeapon__ProcessLineOfSight = 0x00564C00;
constexpr uintptr_t CWeapon__DoBulletImpact     = 0x0055F950;
constexpr uintptr_t CWeapon__DoDoomAiming       = 0x00562EB0;

// ---- what a bullet does to a ped before it hurts him ------------------------
//
// Every reaction a round causes runs in the fire path, ahead of the damage
// call, and none of it tests a proof flag. DoBulletImpact's ped arm, after the
// type gate at 0x0055FA1D and DoesLOSBulletHitPed (0x004EB5C0):
//
//   0055FAC8  call 004CCE20                 GetLocalDirection -> ebx (0..3)
//   0055FADA  call 004DDEC0                 ReactToAttack(shooter)
//   0055FAE3  call 004CE6C0                 IsPedInControl
//   0055FAF2  [victim+158h] bit 4           bIsDucking
//             either fails -> straight to the damage call
//   0055FB36  weapon 4 or 0Dh:              shotgun / helicannon
//     0055FB84  [victim+154h] &= ~1           bIsStanding = false
//     0055FBBB  call 004959A0                 ApplyMoveForce(-5x, -5y, 5)
//     0055FBCF  call 004D09B0                 SetFall(1500, 19h + dir, 0)
//   0055FC06  call 004D48E0                 IsPlayer:
//     0055FC17  [victim+55Ch] vs now, jae     hit anim delay not over -> skip
//     0055FC2E  call 004E67F0                 ClearAttackByRemovingAnim
//     0055FC41  call 00403620                 AddAnimation(clump, 0, 1Dh + dir)
//     0055FC71 / 0055FC8A                     delay = now + 2500 (AK, M16)
//                                                   or now + 1000
//   0055FC9C  not a player: the same two calls, no delay
//   0055FCEA  call 004EA420                 CPed::InflictDamage
//
// FireShotgun's ped arm has no in-control or ducking gate:
//
//   00560FEF  call 004DDEC0                 ReactToAttack(shooter)
//   00561050  [victim+4C8h] vs now - 3000   m_getUpTimer, jbe -> may fall
//   0056105E  [victim+154h] bit 0           bIsStanding, and may fall:
//     00561070  bIsStanding = false, ApplyMoveForce(-6x, -6y, 5)
//             otherwise                     ApplyMoveForce(-2x, -2y, 0)
//   005610E6  call 004D09B0                 SetFall(1500, 19h + dir, 0), may fall only
//   00561112  call 004EA420                 CPed::InflictDamage
//
// x, y is the flat unit vector from the ped to the fire source. The floats are
// -6, -2, 5 and 0 at 0x00603130, 0x00603134, 0x006030DC and 0x00603060.
//
// Two more fire paths flinch a ped the same way, and both matter for a hit
// that arrives as C_Damage instead:
//
//   CBulletInfo::Update (sniper)   005586ED IsPedInControl, 005586FA ducking,
//                                  0055870B ClearAttackByRemovingAnim,
//                                  00558717 `push 1Dh` - always the front one,
//                                  then InflictDamage at 00558785
//   FireInstantHitFromCar          00562B8B ReactToAttack(FindPlayerPed()),
//                                  00562B92 ClearAttackByRemovingAnim,
//                                  00562BA5 AddAnimation(1Dh + dir), no gate,
//                                  then InflictDamage at 00562BE0 (cause 13h)
//
// And CPed::InflictDamage (0x004EA420, last `ret 14h` at 0x004EADE3) plays
// none of it. Its calls are FindPlayerPed, FindPlayerVehicle, IsPlayer,
// AnnoyPlayerPed (0x004F3700, a temper byte), IsPedHeadAbovePos (0x004EB670),
// an association lookup (0x00405750), GetRandomNumber, RemoveBodyPart, SetDie
// and CDarkel's two registers. No AddAnimation, SetFall, ApplyMoveForce or
// ClearAttackByRemovingAnim, so a hit that only reaches InflictDamage costs
// health and nothing else unless it kills.
//
// ReactToAttack on a player whose attacker is a ped is three calls and a
// return (0x004DDECC-0x004DDF02): InformMyGangOfAttack, SetLookFlag(attacker)
// and SetLookTimer(700). The first one is not cosmetic. For every nearby ped
// whose m_leader (+0x180) is this player and whose fear is under his temper
// (m_pedStats +0x24 < +0x25), it calls SetObjective(KILL_CHAR_ON_FOOT,
// attacker) and SetObjectiveTimer(30000) (0x004E4B30-0x004E4B6D). Anyone
// following the player turns on whoever shot him, for thirty seconds.
constexpr uintptr_t CPed__ReactToAttack             = 0x004DDEC0;   // (CEntity*), ret 4
constexpr uintptr_t CPed__InformMyGangOfAttack      = 0x004E4AD0;   // for the record
constexpr uintptr_t CPed__ClearAttackByRemovingAnim = 0x004E67F0;   // (), ret
constexpr uintptr_t CPed__SetFall                   = 0x004D09B0;   // (int, AnimationId, bool), ret 0Ch
constexpr uintptr_t CPhysical__ApplyMoveForce       = 0x004959A0;   // (float, float, float), ret 0Ch
constexpr uintptr_t CPed__GetLocalDirection         = 0x004CCE20;   // for the record

constexpr uint32_t ANIM_KO_SKID_FRONT      = 0x19;   // + direction, 0055FBC0 / 005610D7
constexpr uint32_t ANIM_SHOT_FRONT_PARTIAL = 0x1D;   // + direction, 0055FC33 / 00562B9B
constexpr int32_t  SHOTGUN_FALL_MS         = 1500;   // `push 5DCh`
constexpr uint32_t GETUP_GRACE_MS          = 3000;   // `add esi,0FFFFF448h`
constexpr uint32_t HIT_ANIM_HOLD_RIFLE_MS  = 2500;   // `add edi,9C4h`, AK47 and M16
constexpr uint32_t HIT_ANIM_HOLD_MS        = 1000;   // `add esi,3E8h`, the rest
constexpr float    SHOTGUN_PUSH_FALL       = -6.0f;
constexpr float    SHOTGUN_PUSH_FALL_Z     = 5.0f;
constexpr float    SHOTGUN_PUSH_STAND      = -2.0f;
constexpr float    HIT_ANIM_BLEND_DELTA    = 8.0f;   // 41000000h into +0x1C

namespace offs {
// CPed::m_getUpTimer. SetFall writes -1 here for a timeless fall
// (`or dword [ebp+4C8h],-1` at 0x004D0A5C), and FireShotgun reads it as above.
constexpr size_t PED_GETUP_TIMER = 0x4C8;
// CPlayerPed::m_nHitAnimDelayTimer, past the end of CPed. Only read after
// IsPlayer said yes, which is why it can be.
constexpr size_t PLAYER_HIT_ANIM_DELAY = 0x55C;
// Byte A bit 0 and byte E bit 4 of the flag block at PED_FLAGS.
constexpr size_t  PED_FLAGS_E     = 0x158;
constexpr uint8_t PED_IS_STANDING = 0x01;   // byte A
constexpr uint8_t PED_IS_DUCKING  = 0x10;   // byte E
} // namespace offs

// ---- fists and the bat ------------------------------------------------------
//
// Read off the retail image with dumpbin /disasm, 2026-09-23. re3's
// PedFight.cpp:1530-1691 and Weapon.cpp:355-505 were the map; the image
// differs from the second in one place, marked. Two paths and they share
// nothing but the victim's functions. Neither touches a vehicle: both walk the
// striker's m_nearPeds (PED_NEAR_PEDS) and nothing else, so a punch or a bat
// never reaches a car in retail 1.0.
//
// **Fists: CPed::FightStrike** (0x004E8EC0, thiscall(CVector &node), `ret 4`),
// from CPed::Fight once the move's anim is inside its fire window. The first
// near ped whose hit spheres the node reaches, then:
//   004E915F  IsPlayer(victim) && state 25h (GETUP): return, nothing happens
//   004E9189  old health = victim+2C0h
//   004E91CC  damageMult = move.damage * (2 + rand&1) + 1, a byte
//   004E9237  dir = victim->GetLocalDirection(striker - victim)
//   004E9242  striker IsPlayer: adrenaline (+57Ch) makes it 20; otherwise
//             it is scaled by m_pedStats->m_attackStrength (+28h)
//   004E92A2  move 8 (KICK): dir + 1 half the time
//   004E92CD  victim->ReactToAttack(striker)
//   004E92D2  StartFightDefend(dir, move.hitLevel, x) at 004E932C, where x is
//             65h when the striker holds a weapon and the victim is not a
//             player, damageMult otherwise
//   004E9337  PlayHitSound(victim) (004E8E20), m_fightState (+4B1h) = -1
//   004E93A7  not dying or dead: InflictDamage(striker, 0, damageMult * 3.0f,
//             piece, dir) - cause 0 whatever the striker holds
//   004E9482  victim not FALL/DIE/DEAD and health > 0, and any of
//               health < 40 && old > 40 && victim not a player
//               health < 20 && old > 20
//               striker holds a weapon && striker is a player
//               victim's m_pedStats->m_flags (+30h, a word) & 20h
//             -> SetFall(0, 19h + dir, 0) at 004E956F, and bIsStanding = 0 if
//             that left it in FALL
//   004E9598  victim DIE, or not standing: bIsStanding = 0 and
//             ApplyMoveForce((victim - striker, z 0, normalised, z 1) * k * 0.6)
//             with k = min(dm * 0.6, 4) for move 12 (GROUNDKICK),
//             min(dm * 2, 14) for a dying victim under dm 20, dm otherwise
//   004E9744  CEventList::RegisterEvent(assault)
//
// **The bat: CWeapon::FireMelee** (0x0055CA20, thiscall(CEntity *shooter,
// CVector &source), `ret 8`), the WEAPON_FIRE_MELEE arm of CWeapon::Fire
// (0x0055C8DB). Every near ped in reach, not just the first:
//   0055CA57  anim2Playing = RpAnimBlendClumpGetAssociation(clump,
//             info->m_Anim2ToPlay)
//   0055CC6A  IsPlayer(victim) && state 25h: skip him
//   0055CD50  not dying or dead: victim->ReactToAttack(shooter)
//   0055CD7F  StartFightDefend(dir, lvl, 0Ah), lvl 1 (GROUND) for the bat
//             against a ped in FALL/DIE/DEAD, 4 (HIGH) otherwise
//   0055CE0E  not dying or dead, first arm that fits:
//               shooter a player, bat, anim2Playing   100.0f (006030C0)
//               shooter a player, adrenaline          3.5 * m_nDamage (006030C4)
//               victim a player, bat                  2.0 * m_nDamage (006030C8)
//               otherwise                             m_nDamage (info+14h)
//   0055D06E  victim not FALL/DIE/DEAD, health > 0, and
//             (health < 20 && old > 20) or (bat && victim not a player):
//             bIsStanding = 0, ApplyMoveForce(-5 * flat unit toward the
//             shooter, 3.0), SetFall(ms, 19h + dir, 0) with ms 3000 for the
//             bat on a non-player and 1500 otherwise. re3 has those two the
//             other way round. Then shooter->m_pSeekTarget = victim
//   0055D1C0  otherwise, a victim in DIE and no anim2Playing: the same shove
//   0055D249  weapon state 4, RegisterEvent(assault)
//
// So the victim's whole reaction lives in the striker's call, and only the
// health goes through InflictDamage. On another machine's ped CoopIII keeps
// the health off (HookedInflictDamage) and now keeps the reaction off too,
// and the owner plays it from the two melee bytes on the wire (melee.h).
//
// CPed::StartFightDefend (0x004E7780, thiscall(uint8 dir, uint8 hitLevel,
// uint8 x), `ret 0Ch`): DEAD (31h) gets the floor hit and blood, FALL (24h)
// the floor hit, IsPedInControl the defend or a SetFall. Its other caller,
// 0x004DD354, is not melee.
//
// tFightMoves, 24 rows of 18h from 0x005F9844: animId +0, strikeRadius +10h
// (0x005F9854 in FightStrike), hitLevel +14h (0x005F9858), damage +15h
// (0x005F9859). Rows 1..12 are the moves that strike; 12 is the only one at
// level 1.
//
// (Not melee, but found on the way: object.h's third uproot arm,
// CWeapon__FireMeleeObjectArm at 0x00558A64, is inside CBulletInfo::Update
// (0x00558550), the sniper round, not in FireMelee. FireMelee has no object
// arm at all.)
constexpr uintptr_t CPed__FightStrike       = 0x004E8EC0;   // thiscall(CVector &), ret 4
constexpr uintptr_t CWeapon__FireMelee      = 0x0055CA20;   // thiscall(CEntity *, CVector &), ret 8
constexpr uintptr_t CPed__StartFightDefend  = 0x004E7780;   // thiscall(u8, u8, u8), ret 0Ch
constexpr uintptr_t CPed__PlayHitSound      = 0x004E8E20;   // for the record
constexpr uintptr_t CVector__Normalise      = 0x004BA560;   // thiscall(), on the vector
constexpr uintptr_t tFightMoves             = 0x005F9844;
constexpr size_t    SIZEOF_FIGHTMOVE        = 0x18;
constexpr size_t    FIGHTMOVE_HIT_LEVEL     = 0x14;
constexpr int32_t   NUM_FIGHTMOVES          = 24;
constexpr int32_t   FIGHTMOVE_KICK          = 8;
constexpr int32_t   FIGHTMOVE_GROUNDKICK    = 12;

constexpr uint8_t HITLEVEL_GROUND = 1;
constexpr uint8_t HITLEVEL_HIGH   = 4;

constexpr uint8_t  STRIKE_ARMED_DEFEND    = 0x65;     // `push 65h` at 004E9320
constexpr uint8_t  SWING_DEFEND           = 0x0A;     // `push 0Ah` at 0055CD79
constexpr float    STRIKE_DAMAGE_PER_MULT = 3.0f;     // 0x005F9B00
constexpr float    STRIKE_KNOCK_NPC_BELOW = 40.0f;    // 0x005F9C00
constexpr float    MELEE_KNOCK_BELOW      = 20.0f;    // 0x005F9C04, 0x006030CC
constexpr float    STRIKE_PUSH_SCALE      = 0.6f;     // 0x005F9C10, a double
constexpr float    STRIKE_GROUNDKICK_MULT = 0.6f;     // 0x005F9AE0
constexpr float    STRIKE_GROUNDKICK_CAP  = 4.0f;     // 0x005F9AB8
constexpr float    STRIKE_DYING_MULT      = 2.0f;     // 0x005F9A84
constexpr float    STRIKE_DYING_CAP       = 14.0f;    // 0x005F9C08
constexpr uint8_t  STRIKE_DYING_BELOW     = 20;       // `cmp byte [esp+24h],14h`
constexpr float    SWING_PUSH             = -5.0f;    // 0x006030B8
constexpr float    SWING_PUSH_Z           = 3.0f;     // 0x006030B4
constexpr float    SWING_HEAVY_DAMAGE     = 100.0f;   // 0x006030C0
constexpr float    SWING_ADRENALINE_MULT  = 3.5f;     // 0x006030C4
constexpr float    SWING_PLAYER_BAT_MULT  = 2.0f;     // 0x006030C8
constexpr int32_t  SWING_FALL_MS          = 1500;     // `push 5DCh`
constexpr int32_t  SWING_FALL_NPC_BAT_MS  = 3000;     // `push 0BB8h`
constexpr uint16_t STAT_ONE_HIT_KNOCKDOWN = 0x20;

namespace offs {
constexpr size_t PED_FIGHT_MOVE       = 0x4AC;   // int32, m_curFightMove
constexpr size_t PED_FIGHT_STATE      = 0x4B1;   // int8, m_fightState
constexpr size_t PEDSTATS_FLAGS       = 0x30;    // uint16, CPedStats::m_flags
constexpr size_t PLAYER_ADRENALINE    = 0x57C;   // bool, CPlayerPed only
} // namespace offs

constexpr size_t WEAPONINFO_DAMAGE = 0x14;   // int32 m_nDamage, `fild [ebp+14h]`

// ---- the drive-by ------------------------------------------------------------
//
// Read off the retail image with dumpbin /disasm, 2026-09-23. re3's
// Automobile.cpp:3036-3104 and Weapon.cpp:316-345/1640-1760 were the map, and
// the image differs from them in two places, both marked.
//
// **The trigger.** CAutomobile::ProcessControl's STATUS_PLAYER arm calls
// DoDriveByShootings (0x00564000) at 0x00531A5D, so only the car the local pad
// drives ever runs it; a copy of somebody else's car is status 3 here
// (game/carstatus.h) and never does. In order:
//   00564008  pDriver = [esi+1A4h], weapon = &pDriver->m_weapons[m_currentWeapon]
//   00564029  `cmp dword [ebx],3 / jne out` - an uzi or nothing
//   00564038  CWeapon::Update(pDriver->m_audioEntityId)
//   00564049  looking left / right: CPad::GetLookLeft / GetLookRight in the
//             top-down camera, the active CCam's LookingLeft (+1A4h+6FAD00h)
//             and LookingRight otherwise
//   005640C2  left: RpAnimBlendClumpGetAssociation (0x004055C0) for 78h, and
//             blendDelta (+1Ch) = -1000.0f if there is one; then 77h, and
//             CAnimManager::AddAnimation(clump, 0, 77h) if there is none or
//             its blendDelta is below 0.0, else `or [edx+30h],1` (SetRun)
//   00564120  right: the same with the two ids swapped, only when the front
//             passenger seat (+1A8h) is empty or the camera is first-person
//   005641A2  CPad::GetCarGunFired, and now > weapon->m_nTimer:
//             FireFromCar(this, left) at 0x005641BF, m_nTimer = now + 46h
//   005641D1  neither: CWeapon::Reload, and both ids get blendDelta -1000.0f
//
// **One round.** CWeapon::FireFromCar (0x0055C940, thiscall(CAutomobile *,
// bool), `ret 8`) refuses unless the state is 0 or 1 and the clip is > 0, then
// calls FireInstantHitFromCar (0x005624D0, the same two args) and, when that
// says yes, DMAudio.PlayOneShot(car->m_audioEntityId, 2Fh, 0.0) at 0x0055C996
// and the ammo bookkeeping. FireInstantHitFromCar:
//   source    the car's matrix times (-/+ (bbox.max.x + 0.2), seat.y + rand *
//             0.001, seat.z + 0.5), plus timestep * m_vecMoveSpeed
//   target    the same times (-/+ range, seat.y, seat.z + 0.5), plus a random
//             +-1.28 on each axis (0x00562902..0x0056296C)
//   00562972  DoDriveByAutoAiming(FindPlayerPed(), &source, &target)
//   0056298F  CEventList::RegisterEvent(GUNSHOT, PED, FindPlayerPed() x2)
//   005629E5  CParticle::AddParticle(0Ch GUNFLASH, source, zero, ...), unless
//             TheCamera.GetLookingLRBFirstPerson (0x0046BA20)
//   00562A7A  CPointLights::AddLight(0, source, zero, 5.0, 1.0, 0.8, 0.0, 0, 0)
//   00562AB2  CWeapon::ProcessLineOfSight(source, target, ..., type,
//             car->pDriver, 1, 1, 1, 1, 1, 1, 0). The driver, not the car:
//             re3 passes the shooter here and the image does not
//   victim    CBulletTraces::AddTrace(source, point) at 0x00562B07, then
//     ped     not dying: ReactToAttack(FindPlayerPed()), the flinch, and
//             CPed::InflictDamage(car, 13h, 3 * m_nDamage, piece, dir) at
//             0x00562BE0 - `push ebx` at 0x00562BDF, and ebx is the car. The
//             only one of the image's 21 InflictDamage sites that pushes 13h
//     vehicle CVehicle::InflictDamage(FindPlayerPed(), 13h, m_nDamage) at
//             0x00562D2B, again the only 13h among its eight sites
//     other   CGlass::WasGlassHitByBullet (0x00504670)
//     then    table 0x00603214 by type: building script sound 6Ah, vehicle
//             PlayOneShot(victim, 38h, 1.0), ped 37h plus CPed::Say(65h),
//             object 6Bh, dummy 6Ch. No smoke, unlike the helicopter's round
//   no victim AddTrace(source, source + (target - source) * 30.0 / range) at
//             0x00562E4F. re3 draws nothing here; the image draws 30 m
//
// So a drive-by is not CWeapon::Fire and nothing combat.cpp hooked on foot
// ever saw one. And an observer cannot replay it through the engine: every
// culprit and every event in it is FindPlayerPed(), which on the observer's
// machine is the observer.
constexpr uintptr_t CAutomobile__DoDriveByShootings = 0x00564000;   // for the record
constexpr uintptr_t CWeapon__FireFromCar            = 0x0055C940;   // thiscall(CAutomobile *, bool), ret 8
constexpr uintptr_t CWeapon__FireInstantHitFromCar  = 0x005624D0;   // for the record
constexpr uintptr_t RpAnimBlendClumpGetAssociation  = 0x004055C0;   // for the record

constexpr uint16_t ANIM_STD_CAR_DRIVEBY_LEFT  = 0x77;   // `push 77h` at 0x005640DD
constexpr uint16_t ANIM_STD_CAR_DRIVEBY_RIGHT = 0x78;   // `push 78h` at 0x005640C5
constexpr float    DRIVEBY_ANIM_DROP_DELTA    = -1000.0f;   // 0C47A0000h into +0x1C
constexpr float    DRIVEBY_MISS_TRAIL         = 30.0f;      // 0x00603138
constexpr float    DRIVEBY_LIGHT_RADIUS       = 5.0f;       // 0x006030DC
constexpr uint32_t DRIVEBY_ROUND_MS           = 70;         // `add eax,46h` at 0x005641C9

// Cross-confirmation for a ped offset this file already had, from a function
// that had nothing to do with how it was first found. FireInstantHit's first
// branch is `[ebp+50h] and 7 cmp 3` (IsPed) then `cmp dword [ebp+49Ch],0`
// (m_pPointGunAt), and then it dereferences that pointer as a CEntity: type
// byte at +0x50, position at +0x34, and +0x1F0 handed to
// CPedIK::GetComponentPosition - which is offs::PED_IK, already in this file.
// The byte below it at +0x49B is m_wepAccuracy (`movzx eax,byte [ebp+49Bh]`
// then `100 - eax`), so the run m_currentWeapon / m_maxWeaponTypeAllowed /
// m_wepSkills / m_wepAccuracy / m_pPointGunAt lands on 0x498..0x49C exactly
// as re3 Ped.h:480-484 declares it.
static_assert(offs::PED_POINT_GUN_AT == 0x49C,
              "m_pPointGunAt, witnessed again by CWeapon::FireInstantHit");

// ---- pickups --------------------------------------------------------------
//
// Found the usual way: opcode 531 CREATE_PICKUP, through the dispatcher at
// CRunningScript__ProcessCommands and the 500-range table at 0x005EF298 (base
// opcode 500) entry 31, to the handler at 0x0044328D. That handler's one
// interesting call names CPickups::GenerateNewOne, and GenerateNewOne names
// the array, the stride, both bounds and every field offset in one function.
// docs/pickups.md is the write-up; this block is the evidence.
//
// NOTHING HERE CAME OFF re3's DECLARATIONS. re3 happens to agree on 336, 320
// and 20 - that was checked afterwards, it is not where the numbers came from.
// Four re3 constants have already been refuted on this project.

// CPickup aPickUps[336], stride 0x1C. Slots [0,320) are the general range;
// [320,336) are reserved for PICKUP_FLOATINGPACKAGE and the nautical mines,
// which GenerateNewOne allocates by scanning *downwards* from 335.
//
// Four independent witnesses for the bound and the stride, which is the
// standard the fire array was held to:
//
//   1. CPickups::GenerateNewOne (0x004304B0)
//        0x004304C8  mov ebp,24A4h / mov ebx,14Fh      slot 335, 335*28=9380
//        0x004304D4  sub ebp,1Ch   / dec ebx           descending, stride 28
//        0x004304FA  cmp ebx,140h                      general range = 320
//        0x00430568  cmp ebx,150h                      total = 336
//   2. CPickups::Update (0x004303D0)
//        0x0043044A  mov esi,87AF98h / mov ebx,140h
//                    87AF98h - 878C98h = 2300h = 8960 = 320 * 28
//   3. CPickups::RemoveAllFloatingPickups (0x004307FF)
//        add esi,1Ch / cmp ebp,150h / jl
//   4. CPickups::Save (0x00433E40)
//        mov dword [edx],24C0h    = 9408 = 336 * 28, the save block size,
//        then a field-by-field copy with each field's real width - which is
//        the struct layout below, witnessed a second time by a function that
//        had nothing to do with how any of it was found.
constexpr uintptr_t CPickups__aPickUps  = 0x00878C98;
constexpr size_t    NUM_PICKUPS         = 336;
constexpr size_t    NUM_GENERAL_PICKUPS = 320;
constexpr size_t    SIZEOF_PICKUP       = 0x1C;

// The 20-deep ring that HAS_PICKUP_BEEN_COLLECTED reads, and the *only* thing
// it reads. CPickups::AddToCollectedPickupsArray (0x00433D60) pushes
// `slot | (m_nIndex << 16)` and wraps at 14h; CPickups::IsPickUpPickedUp
// (0x00430770) scans all 20, and **zeroes the entry it matched** - it is a
// consuming read. Bound witnessed twice (cmp ecx,14h, and cmp word [..],14h).
//
// This matters more than it looks: rampage.sc and rewards.sc poll opcode 532,
// so a pickup somebody else collected has to be pushed into *this* machine's
// ring too or the observer's rampage never starts. docs/pickups.md section 6.
constexpr uintptr_t CPickups__aPickUpsCollected    = 0x0087C538; // int32[20]
constexpr uintptr_t CPickups__CollectedPickUpIndex = 0x0095CC8A; // int16
constexpr size_t    NUM_COLLECTED_PICKUPS          = 20;

namespace offs {

// CPickup, 0x1C bytes. Every offset below is written by GenerateNewOne at the
// address in the comment and read back by CPickup::Update off `esi`, and the
// whole set is copied again with its widths by CPickups::Save.
constexpr size_t PICKUP_TYPE     = 0x00; // uint8  0 = PICKUP_NONE = free slot
constexpr size_t PICKUP_REMOVED  = 0x01; // uint8  0x0043058F writes 0
constexpr size_t PICKUP_QUANTITY = 0x02; // uint16 0x00430596, mov word [..9Ah],cx
constexpr size_t PICKUP_OBJECT   = 0x04; // CObject*, 0x0043063A from GiveUsAPickUpObject
constexpr size_t PICKUP_TIMER    = 0x08; // uint32 absolute ms, 0x004305AB
constexpr size_t PICKUP_MODEL    = 0x0C; // int16  0x00430614
constexpr size_t PICKUP_INDEX    = 0x0E; // uint16 generation, 0x00433DC8
constexpr size_t PICKUP_POS      = 0x10; // CVector, 0x0043061D/0623/062B

} // namespace offs

// ePickupType. Read off GenerateNewOne's timer arms and the two switch bounds,
// not off re3's enum: `cmp al,4` -> +4E20h (20 s), `cmp al,7` -> +7530h
// (30 s), `al == 8 || 9` -> forced to 8 with +5DCh, `al == 0Ah || 0Bh` ->
// forced to 0Ah with +5DCh. The award switch at 0x00430ED3 indexes
// `m_eType - 1` into a 14-entry table and the mine switch at 0x004308B2
// indexes `m_eType - 8` into a 6-entry one, which pins both ends of the range.
enum ePickupType : uint8_t {
	PICKUP_NONE                     = 0,
	PICKUP_IN_SHOP                  = 1,
	PICKUP_ON_STREET                = 2,
	PICKUP_ONCE                     = 3,
	PICKUP_ONCE_TIMEOUT             = 4,
	PICKUP_COLLECTABLE1             = 5,
	PICKUP_IN_SHOP_OUT_OF_STOCK     = 6,
	PICKUP_MONEY                    = 7,
	PICKUP_MINE_INACTIVE            = 8,
	PICKUP_MINE_ARMED               = 9,
	PICKUP_NAUTICAL_MINE_INACTIVE   = 10,
	PICKUP_NAUTICAL_MINE_ARMED      = 11,
	PICKUP_FLOATINGPACKAGE          = 12,
	PICKUP_FLOATINGPACKAGE_FLOATING = 13,
	PICKUP_ON_STREET_SLOW           = 15,
};

// Once a frame out of CGame::Process (the call at 0x0048C98E, between
// gAccidentManager.Update and CGarages::Update). Scans only a sixth of the
// general range per frame - `mov eax,0AAAAAAABh / mul [m_FrameCounter] /
// shr edx,2 / imul edx,6` then `imul ebx,35h` (53 = 320/6), so it reaches 318
// of the 320 general slots, the same off-by-two retail has always had. Slots
// 320..335 are scanned in full every frame.
//
// **This is CoopIII's detour point and it is complete.** A whole-image scan
// for E8/E9 rel32 targets equal to CPickup::Update found exactly two call
// sites, 0x0043042F and 0x0043047F, and both are inside this function. There
// is no other way to collect a pickup in this build.
constexpr uintptr_t CPickups__Update = 0x004303D0;

// CPickup::Update(CPlayerPed *player, CVehicle *vehicle, int playerId),
// __thiscall, returns true when it collected something. The argument order is
// read off the call site rather than off re3: CPickups::Update pushes
// CWorld::PlayerInFocus, then the result of FindPlayerVehicle, then the
// result of FindPlayerPed - so the ped is first.
//
// The shape of it, which is what the design in docs/pickups.md section 4
// turns on:
//
//   0x00430872  cmp byte [esi+1],0 / jne 0x004313D5    m_bRemoved -> respawn
//   0x00430880  mov ecx,[esi+4] / test ecx,ecx / je    m_pObject nil ->
//                                                      return false
//   0x004308B2  jmp [eax*4+5EE1D8h]                    mines, types 8..13
//   0x00430C58  the touch test
//   0x00430E19  CanBePickedUp, inlined
//   0x00430EB3  CPad::GetPad(0)->StartShake(120,100)
//   0x00430ED3  jmp [eax*4+5EE1A0h]                    the award, m_eType-1
//
// The nil check at 0x00430880 sits *below* the respawn branch and *above*
// everything else, which is why nilling m_pObject blocks the award without
// blocking the respawn.
constexpr uintptr_t CPickup__Update = 0x00430860;

// The award switch's jump table, indexed by m_eType - 1, bound `cmp eax,0Dh`.
// Entries 5 and 7..12 point at the common exit 0x00431382, and entry 13
// (ON_STREET_SLOW) shares ON_STREET's arm - which is re3's switch exactly,
// fallthrough for fallthrough.
constexpr uintptr_t CPickup__UpdateAwardTable = 0x005EE1A0;
constexpr uintptr_t CPickup__UpdateMineTable  = 0x005EE1D8;

// CPickups::GenerateNewOne(CVector pos, uint32 modelIndex, uint8 type,
//                          uint32 quantity) -> slot | (m_nIndex << 16)
// cdecl. Argument order read off COMMAND_CREATE_PICKUP's call at 0x0044336B:
// the pushes are 0 (quantity), ScriptParams[1] (type), the model, then z, y, x.
// **It never calls the RNG.** That is what makes every script pickup identical
// on every machine, which is the whole premise of docs/pickups.md section 1.
constexpr uintptr_t CPickups__GenerateNewOne = 0x004304B0;

// CPickups::GetNewUniquePickupIndex(int32 slot) bumps m_nIndex (wrapping
// 0xFFFE -> 1) and returns `slot | (m_nIndex << 16)`.
// CPickups::GetActualPickupIndex(int32 handle) undoes it and returns -1 when
// the generation no longer matches. Same stale-handle scheme as the blips,
// and like them it is a *per process* handle - docs/pickups.md section 2 is
// why a slot index must never go on the wire.
constexpr uintptr_t CPickups__GetNewUniquePickupIndex = 0x00433DB0;
constexpr uintptr_t CPickups__GetActualPickupIndex    = 0x00433DF0;

// CPickups::AddToCollectedPickupsArray(int32 slot) - note it takes the raw
// slot and composes the handle itself off m_nIndex.
// CPickups::IsPickUpPickedUp(int32 handle) - consuming, see above.
constexpr uintptr_t CPickups__AddToCollectedPickupsArray = 0x00433D60;
constexpr uintptr_t CPickups__IsPickUpPickedUp           = 0x00430770;

// CPickups::RemovePickUp(int32 handle). CWorld::Remove, the deleting
// destructor, m_pObject = nil, m_eType = PICKUP_NONE, m_bRemoved = 1.
// **It frees the slot**, so it is the script's REMOVE_PICKUP and NOT what an
// observer wants when somebody else collected a respawning pickup - that has
// to keep m_eType. docs/pickups.md section 4.
constexpr uintptr_t CPickups__RemovePickUp             = 0x004307A0;
constexpr uintptr_t CPickups__RemoveAllFloatingPickups = 0x004307FF;

// CPickups::GivePlayerGoodiesWithPickUpMI(int16 modelIndex, int playerIndex).
// Returns true when it handled the model, which is how the award switch tells
// a weapon from everything else. A flat chain of `cmp dx,[MI_...]`, and each
// arm is what identifies the global above it - see the model index block.
constexpr uintptr_t CPickups__GivePlayerGoodiesWithPickUpMI = 0x004339F0;

// CPickups::WeaponForModel(int32 model) -> eWeaponType, 0 when it is not a
// weapon. Opens `cmp edx,[MI_PICKUP_BODYARMOUR] / mov eax,0Fh` (armour is
// weapon slot 15), then a jump table at 0x005EE170 over `model - 0AAh`.
constexpr uintptr_t CPickups__WeaponForModel = 0x004306F0;

// uint16[] each, indexed by eWeaponType. Read off the IN_SHOP and ONCE arms of
// the award switch: `movsx eax,word [eax*2+5ED924h]` is compared against and
// subtracted from CPlayerInfo::m_nMoney, so 0x005ED924 is the cost table;
// `movzx eax,word [eax*2+5ED8D4h]` is handed to CPed::GiveWeapon, so
// 0x005ED8D4 is the ammo table.
constexpr uintptr_t AmmoForWeapon = 0x005ED8D4;
constexpr uintptr_t CostOfWeapon  = 0x005ED924;

// The pickup model indices. int16 globals, filled at *runtime* by CModelInfo's
// name lookup - they all read 0xFFFF in the file on disk, so nothing here can
// be checked statically and they must be read in-process.
//
// The names below are not taken from re3's declaration order. Each one is
// identified by what the code does with it, which is a stronger claim:
//
//   0x005F5B10  ADRENALINE  sets CPed+57Ch and a +20000 ms deadline at +580h,
//                           and swaps +54Ch into +548h        (0x00433A2A)
//   0x005F5B14  BODYARMOUR  `mov dword [ebx+2C4h],42C80000h` = m_fArmour =
//                           100.0f, and CanBePickedUp refuses it above 99.5
//   0x005F5B18  INFO        plays a sound and nothing else    (0x00433A85)
//   0x005F5B1C  HEALTH      `mov dword [ebx+2C0h],42C80000h` = m_fHealth =
//                           100.0f, and CanBePickedUp refuses it above 99.5
//   0x005F5B20  BONUS       sound only                        (0x00433AC5)
//   0x005F5B24  BRIBE       takes a wanted star off; and it is the one model
//                           whose touch test uses a 2 m vehicle sphere
//   0x005F5B28  KILLFRENZY  CanBePickedUp gates it on IsPlayerOnAMission,
//                           CDarkel::FrenzyOnGoing and CGame::nastyGame
//   0x005F5B2C  CAMERA      collectable *only* from a vehicle, and its arm at
//                           0x004310A6 sets the static-camera globals - so
//                           retail 1.0 does have the pickup camera that re3
//                           keeps behind CAMERA_PICKUP
constexpr uintptr_t MI_PICKUP_ADRENALINE = 0x005F5B10;
constexpr uintptr_t MI_PICKUP_BODYARMOUR = 0x005F5B14;
constexpr uintptr_t MI_PICKUP_INFO       = 0x005F5B18;
constexpr uintptr_t MI_PICKUP_HEALTH     = 0x005F5B1C;
constexpr uintptr_t MI_PICKUP_BONUS      = 0x005F5B20;
constexpr uintptr_t MI_PICKUP_BRIBE      = 0x005F5B24;
constexpr uintptr_t MI_PICKUP_KILLFRENZY = 0x005F5B28;
constexpr uintptr_t MI_PICKUP_CAMERA     = 0x005F5B2C;

// CDarkel::FrenzyOnGoing() and CTheScripts::IsPlayerOnAMission(), both called
// from the inlined CanBePickedUp at 0x00430E8C/0x00430E99. Recorded because
// the first is half of why a rampage is a session-wide thing and not a
// per-player one (docs/pickups.md section 6): the engine already refuses a
// second killfrenzy pickup while one is running, and there is exactly one
// CDarkel.
constexpr uintptr_t CDarkel__FrenzyOnGoing          = 0x00420E60;
constexpr uintptr_t CTheScripts__IsPlayerOnAMission = 0x00439410;

// ---------------------------------------------------------------------------
// CDarkel - the whole of a rampage (docs/rampage.md)
// ---------------------------------------------------------------------------
//
// Every address below was read out of `dumpbin /disasm` of
// reference/bin/gta3.exe and is quoted at the instruction that proves it. The
// region needs realigning by hand before it makes sense: the padding runs
// between these functions are zeroes, `dumpbin` decodes them as `add [eax],al`
// and comes out of the run one byte late, so a naive grep for a function's
// address finds nothing at all. Every entry point here was re-decoded from the
// bytes in the listing.
//
// ---- the statics, and what each one is pinned by --------------------------
//
// Status is the anchor. CDarkel::ReadStatus at 0x00420E50 is the whole
// function `66 A1 B4 CC 95 00 / C3` - `mov ax,[0095CCB4] / ret` - and
// CDarkel::FrenzyOnGoing at 0x00420E60 is `66 83 3D B4 CC 95 00 01 /
// 0F 94 C0 / C3`, `cmp word [0095CCB4],1 / sete al / ret`. So Status is a
// uint16 at 0x0095CCB4 and KILLFRENZY_ONGOING is 1. StartFrenzy writes 1 into
// it (0x0042110C), Update writes 2 at 0x00420819 and 3 at 0x004206B6, which
// puts PASSED at 2 and FAILED at 3 - the three numbers rampage.sc compares
// against after `01FA: $FRENZY_STATUS = rampage_status`.
//
// The rest fall out of StartFrenzy (0x004210E0) and RegisterKillByPlayer
// (0x00420F60) writing and reading them in the order re3's Darkel.cpp does.
constexpr uintptr_t CDarkel__Status         = 0x0095CCB4;   // uint16
constexpr uintptr_t CDarkel__KillsNeeded    = 0x008F1AB8;   // int32, counts DOWN
constexpr uintptr_t CDarkel__TimeLimit      = 0x00885BAC;   // int32 ms, <0 = none
constexpr uintptr_t CDarkel__TimeOfFrenzyStart = 0x009430D8; // int32, CTimer ms
constexpr uintptr_t CDarkel__PreviousTime   = 0x00885B00;   // int32 s, tick sound
constexpr uintptr_t CDarkel__WeaponType     = 0x009430F0;   // int32 eWeaponType
constexpr uintptr_t CDarkel__ModelToKill    = 0x008F2C78;   // int32, -1 any ped
constexpr uintptr_t CDarkel__ModelToKill2   = 0x00885B40;   // int32          -2 any car
constexpr uintptr_t CDarkel__ModelToKill3   = 0x00885B3C;   // int32
constexpr uintptr_t CDarkel__ModelToKill4   = 0x00885B34;   // int32
constexpr uintptr_t CDarkel__bNeedHeadShot  = 0x0095CDCA;   // bool
constexpr uintptr_t CDarkel__bStandardSound = 0x0095CDB6;   // bool
constexpr uintptr_t CDarkel__bProperKillFrenzy = 0x0095CD98; // bool
constexpr uintptr_t CDarkel__pStartMessage  = 0x008F2C08;   // wchar*

// FRENZY_ANY_PED and FRENZY_ANY_CAR, and they are read off the comparisons
// rather than off re3's #defines: `cmp dword [008F2C78h],0FFFFFFFFh` at
// 0x00420FC1 inside RegisterKillByPlayer, and `cmp dword [008F2C78h],
// 0FFFFFFFEh` at 0x0042107F inside RegisterCarBlownUpByPlayer. Same global,
// two sentinels, one per kind of rampage.
constexpr int32_t FRENZY_ANY_PED = -1;
constexpr int32_t FRENZY_ANY_CAR = -2;

// The four values rampage.sc compares `$FRENZY_STATUS` against.
constexpr uint16_t KILLFRENZY_NONE    = 0;
constexpr uint16_t KILLFRENZY_ONGOING = 1;
constexpr uint16_t KILLFRENZY_PASSED  = 2;
constexpr uint16_t KILLFRENZY_FAILED  = 3;

// __cdecl void CDarkel::StartFrenzy(eWeaponType weaponType, int32 time,
//         uint16 kill, int32 modelId0, wchar *text, int32 modelId2,
//         int32 modelId3, int32 modelId4, bool standardSound,
//         bool needHeadShot)
//
// Ten arguments, all on the stack, caller-cleaned - the function ends
// `pop ebp / pop ebx / ret` with no operand at 0x0042130B. The order is
// witnessed by the writes:
//
//   0x004210F8  mov [009430F0],eax        arg1  -> WeaponType
//   0x00421193  mov [00885BAC],ebx        arg2  -> TimeLimit
//   0x00421115  mov [008F1AB8],eax        arg3  -> KillsNeeded (movzx from 16)
//   0x0042111E  mov [008F2C78],eax        arg4  -> ModelToKill
//   0x00421142  mov [008F2C08],eax        arg5  -> pStartMessage
//   0x00421127  mov [00885B40],eax        arg6  -> ModelToKill2
//   0x00421130  mov [00885B3C],eax        arg7  -> ModelToKill3
//   0x00421139  mov [00885B34],eax        arg8  -> ModelToKill4
//   0x00421174  mov [0095CDB6],al         arg9  -> bStandardSoundAndMessages
//   0x0042117D  mov [0095CDCA],al         arg10 -> bNeedHeadShot
//
// **Two callers in the whole image**, and both are the script: 0x00442BD2
// behind opcode 01F9 `init_rampage`, and 0x0044B9FE behind 0x0367
// `init_headshot_rampage`. Nothing else in GTA III starts a frenzy, which is
// what makes this a safe place to learn that one has started.
constexpr uintptr_t CDarkel__StartFrenzy = 0x004210E0;

// __cdecl void CDarkel::RegisterKillByPlayer(CPed *victim, eWeaponType weapon,
//                                            bool headshot)
//
// The one function in the engine that means "a kill that counts". Three
// stack arguments, caller-cleaned - CPed::InflictDamage pushes them at
// 0x004EAD30..0x004EAD39 and then `add esp,0Ch`.
//
// The qualification test, transcribed:
//
//   0x00420F60  cmp word [0095CCB4],1     Status == ONGOING, else skip to
//   0x00420F76  jne 0042100F              the statistics at the bottom
//   0x00420F7C  cmp eax,[009430F0]        weapon == WeaponType
//   0x00420F84  cmp eax,12h               || WEAPONTYPE_EXPLOSION
//   0x00420F89  cmp eax,13h / WeaponType==3    || UZI_DRIVEBY with UZI
//   0x00420F97  cmp eax,10h / WeaponType==11h  || RAMMEDBYCAR with RUNOVERBYCAR
//   0x00420FA5  cmp eax,11h / WeaponType==10h  || the same pair reversed
//   0x00420FB3  cmp eax,9   / WeaponType==0Ah  || FLAMETHROWER with MOLOTOV
//   0x00420FC1  cmp [008F2C78],-1         ModelToKill == FRENZY_ANY_PED
//   0x00420FCA  movsx eax,word [ebp+5Ch]  victim->m_modelIndex - the ONLY
//                                         field of the victim this function
//                                         reads before the statistics
//   0x00420FCE  cmp [008F2C78],eax        .. or one of the four model ids
//   0x00420FD6  cmp [00885B40],eax
//   0x00420FDE  cmp [00885B3C],eax
//   0x00420FE6  cmp [00885B34],eax
//   0x00420FEE  cmp byte [0095CDCA],0     bNeedHeadShot
//   0x00420FF7  test bl,bl                .. and the headshot argument
//   0x00421000  dec dword [008F1AB8h]     KillsNeeded--
//   0x0042100A  DMAudio.PlayFrontEndSound(5Ch, 0)      SOUND_RAMPAGE_KILL
//
// and then, unconditionally and whether or not a frenzy is running:
//
//   0x00421013  inc [008F1B7Ch]           CStats::PeopleKilledByPlayer
//   0x00421019  inc word [eax*2+6EDBE0h]  RegisteredKills[model]
//   0x00421021  [ebp+15Bh] >> 7           bChrisCriminal, picking
//   0x0042103C  inc [eax*4+00880DBCh]     CStats::PedsKilledOfThisType[..]
//   0x00421047  inc [008F647Ch]           CStats::HeadsPopped, if headshot
//   0x0042104D  inc [008F2C8Ch]           CStats::KillsSinceLastCheckpoint
//
// **Six callers**, which is why CoopIII hooks the function rather than any
// one of them: 0x004EAD39 (CPed::InflictDamage), 0x0053BDB6 and 0x0053BE17,
// 0x00541D5A, 0x00552776 and 0x005527A0. A rampage kill made with a car, a
// fire or a blast arrives through one of the five that are not InflictDamage.
constexpr uintptr_t CDarkel__RegisterKillByPlayer = 0x00420F60;

// __cdecl void CDarkel::RegisterKillNotByPlayer(CPed *victim, eWeaponType)
//
// Two instructions: `inc dword [008E2C50h] / ret`, CStats::PeopleKilledByOthers
// and nothing else. Recorded because it is the hole this whole feature exists
// to fill - see the note at CDarkel__KILL_CREDIT_TEST below.
constexpr uintptr_t CDarkel__RegisterKillNotByPlayer = 0x00421060;

// __cdecl void CDarkel::RegisterCarBlownUpByPlayer(CVehicle *vehicle)
//
// Re-read from the file 2026-09-23 (dumpbin loses sync on the padding at
// 0x00421067, so 0x00421070..0x0042107F was decoded from the bytes):
//
//   0x00421070  66 83 3D B4CC9500 01   cmp word [0095CCB4],1   Status
//   0x00421078  53 / 8B 5C 24 08       push ebx / mov ebx,[esp+8]  the car
//   0x0042107D  75 41                  jne 004210C0            to the stats
//   0x0042107F  cmp [008F2C78],-2      ModelToKill == FRENZY_ANY_CAR
//   0x00421088  movsx eax,word [ebx+5Ch], then the four model slots
//   0x004210B1  dec [008F1AB8]         KillsNeeded--
//   0x004210BB  call 0057CC20          PlayFrontEndSound(5Dh, 0)
//   0x004210C4  inc word [eax*2+006EDBE0]   RegisteredKills[model], always
//   0x004210CC  inc [00941288]         CStats::CarsExploded, always
//   0x004210D3  ret                    one argument, caller-cleaned
//
// The kill register's shape with no weapon and no headshot test, and a -2
// sentinel instead of -1. **The difference that matters is the callers.** A
// scan for E8 rel32 finds two: 0x0053BF04, inside CAutomobile::BlowUpCar,
// reached unconditionally with no culprit test anywhere in that function,
// and 0x0054A04F in CHeli::UpdateHelis. CBoat::BlowUpCar doesn't call it.
// So every machine counts a car its own copy of which blows up, whoever blew
// it up, which is why game/darkel.cpp keeps CoopIII's replays off the counter
// rather than copying the kill's report-and-relay.
constexpr uintptr_t CDarkel__RegisterCarBlownUpByPlayer = 0x00421070;

// int32 CStats::CarsExploded. `mov eax,[00941288]` at 0x00482AF9, pushed
// with the "CAR_EXP" string (0x005F3A24) on the stats screen. Moved by the
// car register above and by nothing CoopIII does.
constexpr uintptr_t CStats__CarsExploded = 0x00941288;

// __thiscall void CPlayerInfo::AwardMoneyForExplosion(CVehicle *wreck). ret 4.
//
// The only money the engine pays for destroying a car:
//
//   0x004A15F0  mov eax,[00885B48]            CTimer ms, for the 6 s chain
//   0x004A162A  mov edx,[edx+128h] / [edx+0D0h]  pHandling->nMonetaryValue
//   0x004A1646  fmul [005F6AB4]               0.002f (3B03126Fh)
//   0x004A168A  add [eax*4+0094139Ch],ebp     Players[PlayerInFocus].m_nMoney
//
// Two callers, and they disagree about who earned it. The fire timer in
// CAutomobile::ProcessControl calls it at 0x0053479B with no test at all:
// whoever's engine watches a car burn out is paid for it, whoever lit it.
// CVehicle::ProcessDelayedExplosion calls it at 0x00551D6C only when
// `this != FindPlayerVehicle()` and m_pBlowUpEntity (+0x218) is
// FindPlayerPed() (0x00551D42..0x00551D56). docs/rampage.md §9 says why
// money didn't travel; game/money.cpp hooks it for the server's money rule
// and passes every call straight through with the rule off.
constexpr uintptr_t CPlayerInfo__AwardMoneyForExplosion = 0x004A15F0;

// The rest of it, re-read for the money rule. `this` is the CPlayerInfo both
// callers pass (Players + PlayerInFocus * 13Ch at 0x00534787 and 0x00551D58):
//
//   0x004A15FC  sub eax,[ebx+104h] / cmp eax,1770h   now - last award, 6000
//   0x004A160B  jae -> mov dword [ebx+108h],1        a chain of one
//   0x004A160D  inc dword [ebx+108h]                 or one more in it
//   0x004A1624  mov [ebx+104h],eax                   last award = now
//   0x004A1654  or byte [esp+5],0Ch / fistp          the 0.002f product, truncated
//   0x004A166E  push 5F6AB8h ("$%d") / call 0059E5B0 sprintf into gString
//   0x004A16A0  call 005A41D0 / add ...,ebp          (chain - 1) more times, one
//                                                    discarded rand() each
//   0x004A16C1  ret 4
//
// So a burnout pays unit * chain, the chain counted on the payer's own
// CPlayerInfo. The two return addresses are what tell the callers apart from
// inside a detour.
constexpr uintptr_t AWARD_RETURN_FIRE_TIMER = 0x005347A0;   // after 0x0053479B
constexpr uintptr_t AWARD_RETURN_BOMB_TIMER = 0x00551D71;   // after 0x00551D6C
constexpr uint32_t  EXPLOSION_CHAIN_MS      = 6000;         // 1770h
constexpr float     EXPLOSION_REWARD_FACTOR = 0.002f;       // [005F6AB4h]

// CVehicle::ProcessDelayedExplosion (0x00551C90, recorded with BlowUpCar's
// callers) is the bomb timer's caller. The timer is the uint16 at +0x216
// (`mov cx,[ebp+216h]` at 0x00551C97, `sub [ebp+216h],bx` at 0x00551CF0); at
// zero it pays through the gate above and calls BlowUpCar(m_pBlowUpEntity)
// through vtable +74h at 0x00551D7C, with m_pBlowUpEntity read at 0x00551D50
// and 0x00551D73. tHandlingData::nMonetaryValue is the dword the award reads
// at +0xD0 through the car's pHandling (+0x128), 0x004A162A..0x004A1630.
namespace offs {
constexpr size_t VEH_BOMB_TIMER          = 0x216;   // uint16, ms
constexpr size_t VEH_BLOW_UP_ENTITY      = 0x218;   // CEntity*
constexpr size_t HANDLING_MONETARY_VALUE = 0xD0;    // uint32 in tHandlingData
} // namespace offs

// __cdecl uint16 CDarkel::ReadStatus()
//
// `mov ax,word [0095CCB4] / ret`, and the reason it is worth a detour rather
// than a read: **it has exactly one caller in the whole image**, 0x00442BE8,
// which is the handler for script opcode 01FA. So this function is not "the
// status" - it is *the script's* view of the status, and nothing else in the
// game goes through it. CHud::Draw reads the global directly (0x005062CE
// calls FrenzyOnGoing), CanBePickedUp reads it through FrenzyOnGoing, and
// CDarkel::Update reads the global itself at 0x00420660.
//
// That is what lets CoopIII hold every machine's `rampage.sc` at the same
// point in the script without touching the HUD, the timer or the sounds.
constexpr uintptr_t CDarkel__ReadStatus = 0x00420E50;

// __cdecl void CDarkel::Update() - one caller, 0x0048C90E in CGame::Process.
// A switch on Status through the table at 0x005ECD30. The ONGOING arm is
// 0x0042067B and it is the whole of the ending:
//
//   0x0042067B  FrameTime = TimeLimit - (CTimer::ms - TimeOfFrenzyStart)
//   0x0042068E  jg ongoing; 0x00420696 TimeLimit < 0 -> ongoing
//               else Status = 3 (FAILED) and the weapon is put back
//   0x004207FE  cmp [008F1AB8h],0 / jg out    KillsNeeded <= 0
//   0x00420819  Status = 2 (PASSED)
//
// Recorded but not detoured: CoopIII lets this run on every machine exactly
// as retail does, so the HUD, the clock, the tick sound and the weapon
// restore are all the engine's. Only the script's *view* of the outcome is
// held back until the session has agreed one.
constexpr uintptr_t CDarkel__Update = 0x00420660;

// __cdecl void CDarkel::ResetOnPlayerDeath() - 0x00420E70, three callers at
// 0x004A12FB / 0x004A134B / 0x004A139B. Status = FAILED when the local
// player dies. Left alone for the same reason as Update: it is a real local
// ending, and it is reported to the session as one candidate verdict.
constexpr uintptr_t CDarkel__ResetOnPlayerDeath = 0x00420E70;

// **The site this whole feature exists for.**
//
// CPed::InflictDamage decides which of the two registers a death goes into,
// and it decides it on the identity of the damaging entity:
//
//   004EAD15  call 004D37D0        CPed::SetDie - the ped is dead here
//   004EAD1A  call 004A1150        FindPlayerPed()
//   004EAD1F  cmp esi,eax          damagedBy == our player?
//   004EAD21  je  004EAD30
//   004EAD23  test esi,esi         null damager -> not by player
//   004EAD25  je  004EAD50
//   004EAD27  call 004A10C0        FindPlayerVehicle()
//   004EAD2C  cmp esi,eax          damagedBy == our car?
//   004EAD2E  jne 004EAD50
//   004EAD39  call 00420F60        RegisterKillByPlayer(this, method, headshot)
//   004EAD55  call 00421060        RegisterKillNotByPlayer(this, method)
//
// In a CoopIII session the ped is hosted by one machine and shot by another,
// so on the host `esi` is the *replica* of the shooter's ped - neither
// FindPlayerPed() nor FindPlayerVehicle() - and the kill goes to
// RegisterKillNotByPlayer, which only bumps a statistic. On the shooter's own
// machine CPed::InflictDamage returned long before this line, because
// game/combat.cpp turns the hit into a packet instead. **So today a co-op NPC
// kill counts for nobody at all.** docs/rampage.md §2.
constexpr uintptr_t CDarkel__KILL_CREDIT_TEST = 0x004EAD1A;

// The headshot argument, and where it comes from, because CoopIII has to
// reconstruct it on a machine that did not fire the shot.
//
// `headShot` is a stack local of CPed::InflictDamage: cleared at 0x004EA448
// (`mov byte [esp+8],0`) and set in exactly one place, 0x004EA7F7
// (`mov byte [esp+8],1`), inside the bullet-weapon arm's PEDPIECE_HEAD case -
// the same case that calls RemoveBodyPart(PED_HEAD). It is then pushed at
// 0x004EAD30. So `headshot` is "the fatal bullet took the head off", and the
// piece is the only part of it that travels.
//
// The limb only comes off when re3's `dontRemoveLimb` is false, and for the
// pistol, the uzi and the shotgun that is a CGeneral::GetRandomNumber() roll,
// which two machines will not agree on. **It does not matter here**: all three
// headshot rampages in rampage.sc (07, 19, 20) use SNIPERRIFLE, SNIPERRIFLE
// and M16, and those two weapons take the arm where `dontRemoveLimb` is
// unconditionally false. For them, and for them alone, headshot is exactly
// `pedPiece == PEDPIECE_HEAD`.
constexpr uintptr_t CDarkel__HEADSHOT_SET_SITE = 0x004EA7F7;

// DMAudio, and the two sounds a rampage kill makes. `mov ecx,95CDBEh` before
// every call in Darkel.cpp's neighbourhood, and the function is
// __thiscall void CAudioEngine::PlayFrontEndSound(uint16 sound, uint32 frame),
// `ret 8`.
constexpr uintptr_t DMAudio_Object                 = 0x0095CDBE;
constexpr uintptr_t CAudioEngine__PlayFrontEndSound = 0x0057CC20;
constexpr uint16_t  SOUND_RAMPAGE_KILL             = 0x5C;
constexpr uint16_t  SOUND_RAMPAGE_CAR_BLOWN        = 0x5D;

// CPlayerInfo, from the award switch. CWorld::Players is 0x009412F0 with a
// 0x13C stride (`imul eax,eax,4Fh` then `[eax*4 + 9412F0h]`, so 4Fh*4 = 13Ch),
// and CWorld::PlayerInFocus is the byte at 0x0095CD61. Both are cross-
// confirmed by FindPlayerPed (0x004A1150) and FindPlayerVehicle (0x004A10C0),
// which are already in this file and open with exactly those two instructions.
//
//   m_nMoney             +0xAC   `sub [ebx+94139Ch],eax` at 0x00430F20 for a
//                                shop purchase, `add [ebp*4+94139Ch],eax` at
//                                0x0043132C for PICKUP_MONEY
//   m_nCollectedPackages +0xB4   `inc dword [edi+9413A4h]` at 0x0043126A
//   m_nTotalPackages     +0xB8   compared against it at 0x0043127A; set to
//                                100 by opcode 749 out of packages.sc
// FindPlayerCoors(CVector *out) -> out. Called from CPickup::Update's respawn
// branch at 0x004313ED to measure how far the player has walked away. Same
// PlayerInFocus / imul 4Fh prologue as FindPlayerPed, then `cmp byte
// [eax+314h],0` (CPed::bInVehicle) picking the *vehicle's* matrix position
// over the ped's - so it answers "where is the player", not "where is the
// ped", which is what a proximity test wants.
//
// **It does not null-check the ped.** With no player it dereferences zero at
// +314h. Every caller here guards with FindPlayerPed first.
constexpr uintptr_t FindPlayerCoors = 0x004A1030;

constexpr uintptr_t CWorld__Players       = 0x009412F0;
constexpr uintptr_t CWorld__PlayerInFocus = 0x0095CD61;

namespace offs {
constexpr size_t PLAYERINFO_STRIDE             = 0x13C;
constexpr size_t PLAYERINFO_MONEY              = 0xAC;
constexpr size_t PLAYERINFO_COLLECTED_PACKAGES = 0xB4;
constexpr size_t PLAYERINFO_TOTAL_PACKAGES     = 0xB8;
// The rest of what the money rule touches (game/money.h):
//
//   m_nVisibleMoney      +0xB0   what the HUD prints: `mov eax,[edx*4+9413A0h]`
//                                into "$%08d" at 0x00505E7B. CPlayerInfo::Process
//                                walks it toward m_nMoney by 12345 / 1234 / 123 /
//                                42 / 1 a frame, for a gap over 100000 / 10000 /
//                                1000 / 50 / under (0x0049FDC4..0x0049FE31). So
//                                writing m_nMoney alone rolls the counter the
//                                way earning does, and never snaps it.
//   last explosion award +0x104  AwardMoneyForExplosion's chain clock and
//   explosion chain      +0x108  count (addresses above)
//   bGetOutOfJailFree    +0x116  byte, tested before the arrest fine
//   bGetOutOfHospitalFree +0x117 byte, tested before the hospital fee
constexpr size_t PLAYERINFO_VISIBLE_MONEY      = 0xB0;
constexpr size_t PLAYERINFO_LAST_EXPLOSION_MS  = 0x104;
constexpr size_t PLAYERINFO_EXPLOSION_CHAIN    = 0x108;
constexpr size_t PLAYERINFO_JAIL_FREE          = 0x116;
constexpr size_t PLAYERINFO_HOSPITAL_FREE      = 0x117;
} // namespace offs

// What busted and wasted cost, both inside CGameLogic::Update once the
// WBState (+0xD8) has been set for 0x1000 ms. Under `shared` this is money
// leaving the session's wallet like any other change, so it is recorded to
// say that the engine clamps it and nothing here needs to.
//
//   busted  0x004216C7  cmp eax,6 / ja 004216D3   wanted level, 0..6
//           0x004216CC  jmp [eax*4+005ECD88h]     table: 004216D3 x2 (64h),
//                                                 004216DA (0C8h), 004216E1
//                                                 (190h), 004216E8 (258h),
//                                                 004216EF (384h), 004216F6 (5DCh)
//           0x004216FB  cmp byte [ebx+116h],0     jail free: no fine, flag cleared
//           0x00421716  sub ebp,eax / jge / xor   money = max(0, money - fine)
//   wasted  0x004214D9  cmp byte [ebx+117h],0     hospital free: no fee
//           0x004214F6  add eax,0FFFFFC18h / jge  money = max(0, money - 1000)
constexpr uintptr_t BUSTED_FINE_TABLE = 0x005ECD88;
constexpr int32_t   BUSTED_FINES[7]   = {100, 100, 200, 400, 600, 900, 1500};
constexpr int32_t   HOSPITAL_FEE      = 1000;

// ---- the wanted level (docs/wanted.md) ------------------------------------
//
// It hangs off the *ped*, not off CPlayerInfo. docs/roadmap.md §2.3 and the
// original brief for this work both say `CPlayerInfo::m_pWanted` and both are
// wrong about the name, though right about the consequence: CPlayerPed is the
// only class in the process that has one, its first member sits at the end of
// CPed, and a CCivilianPed - which every remote player and every replica is -
// is exactly SIZEOF_PED bytes with nowhere to put a second.
//
// Four witnesses for the offset, three of them reached the usual way. Opcodes
// 269..272 are ALTER_WANTED_LEVEL / _NO_DROP / IS_WANTED_LEVEL_GREATER /
// CLEAR_WANTED_LEVEL, and they live in the 200..299 range: the dispatcher at
// 0x00439500 tests `cmp dx,0C8h` / `cmp dx,12Ch` and calls 0x0043D530, which
// is `lea ebp,[eax-0D6h] / cmp ebp,54h / jmp [ebp*4 + 5EEC40h]` - base opcode
// **214**, not 200, and 85 entries.
//
//   0043E0EC  mov ecx,[eax+53Ch]      opcode 271, then
//   0043E0F7  mov edx,[ecx+18h]       m_nWantedLevel, compared against arg 1
//   004F3194  mov ecx,[ecx+53Ch]      CPlayerPed::SetWantedLevel
//   004F31B4  mov ecx,[ecx+53Ch]      CPlayerPed::SetWantedLevelNoDrop
//   004F4A76  mov eax,[eax+53Ch]      CPopulation::AddToPopulation's cop gate
//
// The rest of CWanted comes from CWanted::UpdateWantedLevel (0x004AD900),
// which writes four members in one pass and is re3 Wanted.cpp:47-77 bracket
// for bracket - chaos 3200/1600/800/400/200/40 mapping to 6..1 stars, with
// m_MaxCops 10/8/6/4/3/1, m_MaximumLawEnforcerVehicles 3/3/2/2/2/1 and
// m_RoadblockDensity 12/10/8/4/0/0.
namespace offs {
constexpr size_t PLAYER_PED_WANTED   = 0x53C;  // CPlayerPed::m_pWanted, CWanted*
constexpr size_t WANTED_LEVEL        = 0x18;   // CWanted::m_nWantedLevel, int32
constexpr size_t WANTED_CHAOS        = 0x00;   // CWanted::m_nChaos, int32
constexpr size_t WANTED_CURRENT_COPS = 0x10;   // uint8
constexpr size_t WANTED_MAX_COPS     = 0x11;   // uint8
constexpr size_t WANTED_MAX_LAW_CARS = 0x12;   // uint8
} // namespace offs

// __thiscall CPlayerPed::SetWantedLevel(int32). Two script opcodes call it:
// jump-table entry 55 of 0x005EEC40 is ALTER_WANTED_LEVEL (269), which passes
// ScriptParams[1], and entry 58 is CLEAR_WANTED_LEVEL (272), which passes a
// literal 0. The function itself is five instructions - load the argument,
// `mov ecx,[ecx+53Ch]`, tail into CWanted::SetWantedLevel (0x004ADA50),
// `ret 4`.
//
// **It is not idempotent, and that is why CoopIII only calls it on a
// disagreement.** CWanted::SetWantedLevel calls ClearQdCrimes (0x004ADF20)
// and then sets m_nChaos to the *bottom* of the requested bracket - 820 for
// four stars, from `mov dword [ebx],334h`. Calling it every tick with the
// level the player already has would throw away their accumulated chaos 25
// times a second and stop them ever reaching the next star.
constexpr uintptr_t CPlayerPed__SetWantedLevel = 0x004F3190;

// Recorded, not used. Established while proving the four above; here so the
// next person starts from a list instead of a search.
//
//   CWanted::SetWantedLevel            0x004ADA50   thiscall(int32)
//   CWanted::SetWantedLevelNoDrop      0x004ADAC0   `cmp arg,[ecx+18h] / jg`
//   CWanted::UpdateWantedLevel         0x004AD900   chaos -> stars
//   CWanted::RegisterCrime             0x004AD9F0
//   CWanted::RegisterCrime_Immediately 0x004ADA10
//   CWanted::SetMaximumWantedLevel     0x004ADAE0   table 0x005F7800
//   CWanted::WorkOutPolicePresence     0x004ADD00   cdecl(CVector, float)
//   CWanted::ClearQdCrimes             0x004ADF20
//   CWanted::MaximumWantedLevel        0x005F7714   the level clamp
//   CWanted::nMaximumWantedLevel       0x005F7718   the chaos clamp
//   CEventList::ReportCrimeForEvent    0x00476070
//   CRunningScript::ProcessCommands200To299  0x0043D530, base 214, 85 entries
//
// And the one fact that made this feature small, from the tail of
// CEventList::RegisterEvent:
//
//   00475DD1  call 004A1150         FindPlayerPed()
//   00475DD6  cmp  [esp+28h],eax    the `criminal` argument
//   00475DDA  jne  00475DF0         not the player: report nothing
//   00475DE8  call 00476070         ReportCrimeForEvent
//
// A crime is only ever reported against the local player's own ped, so M3
// replaying a remote player's shot through CWeapon::Fire on this machine
// cannot move this machine's wanted level. Nobody had to write that.

// The engine's own ceiling, from CWanted::SetMaximumWantedLevel's case 6
// (`mov dword [5F7714h],6`) and from SetWantedLevel's `cmp ebp,[5F7714h] /
// jle` clamp against it. Three bits on the wire hold 0..7, so the eighth
// value is not a state the game has.
//
// Pinned to the wire's copy rather than declared a second time. Two constants
// for one fact is how the ped pool's slot size and the animation node array
// both went wrong on this project: they only ever agreed with themselves.
static_assert(WANTED_LEVEL_CEILING == 6,
              "CWanted::MaximumWantedLevel is 6 in retail 1.0; protocol.h's "
              "clamp has to be the same number");

// The respawn window each type gets, in milliseconds, or 0 for "never comes
// back". A pure function so the server can be given the same number without
// the game, and so a test can walk the whole type range.
//
// Read off the award switch's tails, not off re3:
//   IN_SHOP         0x00430FBE  add eax,1388h    =   5 000
//   ON_STREET       0x0043111C  add eax,7530h    =  30 000
//   ON_STREET_SLOW  0x0043113A  add eax,493E0h   = 300 000  (bribe model)
//                   0x00431146  add eax,0AFC80h  = 720 000  (anything else)
// and ONCE / ONCE_TIMEOUT / COLLECTABLE1 / MONEY all end in the inline
// Remove(), which sets m_eType = PICKUP_NONE and frees the slot for good.
//
// Every one of those is `CTimer::m_snTimeInMilliseconds + k`, i.e. an absolute
// stamp on a *local* clock. The constant travels; the deadline must not.
constexpr uint32_t PickupRespawnMs(uint8_t type, bool isBribeModel) {
	switch (type) {
	case PICKUP_IN_SHOP:        return 5000;
	case PICKUP_ON_STREET:      return 30000;
	case PICKUP_ON_STREET_SLOW: return isBribeModel ? 300000u : 720000u;
	default:                    return 0;
	}
}

// ---- what a dead pedestrian leaves behind ---------------------------------
//
// docs/pickups.md 10. Found from CPed::SetDead (0x004D3970, already in this
// file under the ProcessControl call sequence), whose tail is:
//
//   004D39F2  call 004A1150          FindPlayerPed
//   004D39F7  cmp  ebx,eax
//   004D39F9  je   004D3A09          the local player drops nothing
//   004D39FB  mov  ecx,ebx
//   004D39FD  call 00433660          CreateDeadPedWeaponPickups
//   004D3A02  mov  ecx,ebx
//   004D3A04  call 00433490          CreateDeadPedMoney
//
// which is re3 Ped.cpp:6383-6384 in the engine's own order, weapons first.
// A whole-image scan for E8/E9 rel32 targets equal to either address finds
// **exactly one reference each**, and both are those two call sites. So
// these two functions are reached from nowhere else in this build and
// detouring them covers every ped drop there is.
//
// Both are __thiscall and take nothing but `this`.

// CPed::CreateDeadPedMoney(). re3 Pickups.cpp:1445-1473, statement for
// statement, and **the RNG is all of it**:
//
//   00433497  cmp  byte [005F4DD4],0 / jne      CGame::nastyGame, else return
//   004334B0  movsx edi,word [ebx+5Ch]          m_modelIndex
//             cmp edi,1 / je ret                MI_COP
//             lea eax,[edi-2] / cmp eax,3 / jbe ret   MI_..2..5
//             cmp edi,6 / je ret                MI_FIREMAN
//   004334D0  cmp  byte [ebx+160h],2 / je ret   CharCreatedBy == MISSION_CHAR
//   004334E1  cmp  byte [ebx+314h],0 / jne ret  bInVehicle
//   004334F2  call 005A41D0                     CGeneral::GetRandomNumber
//             movzx ebp,ax, then the 88888889h/sar 5/imul 3Ch pair = %60
//   00433514  cmp  ebp,0Ah / jl ret             money < 10 -> nothing at all
//   00433521  cmp  ebp,2Bh / jne / mov ebp,2BCh money == 43 -> 700
//   0043352B  the 66666667h/sar 4 pair = /40, inc esi   pickupCount
//   00433544  idiv esi                          moneyPerPickup
//   00433567  call 005A41D0 / and eax,0FFh / fmul [005EE138] / fsin
//             fmul [005EDE5C] / fadd pos.x      1.5 * Sin(rnd%256 * PI/128)
//   004335AE  the same again with fcos for y
//   00433606  call 004B3AE0                     FindGroundZFor3DCoord, +0.5f
//   0043361F  call 005A41D0 / and eax,7 / add ebp   quantity + (rnd & 7)
//   00433643  call 004304B0                     GenerateNewOne, type 7, model
//                                               from the int16 at 005F5A08
//
// **Four RNG draws per pickup and one for the amount.** Nothing about this
// function is reproducible on a second machine, which is why the money half
// of a drop can only ever be decided by one of them.
constexpr uintptr_t CPed__CreateDeadPedMoney = 0x00433490;

// CPed::CreateDeadPedWeaponPickups(). re3 Pickups.cpp:1478-1523.
//
// **It never calls the RNG once.** The whole function was disassembled and
// scanned for rel32 calls to CGeneral::GetRandomNumber (0x005A41D0) and there
// are none; the only calls it makes are 005A0CB0 (the FPU range-reduction
// helper behind fsin/fcos), 004B3AE0 (FindGroundZFor3DCoord), 004AEAA0
// (GetIsLineOfSightClear), 00430660 (GenerateNewOne_WeaponType) and 004CFB70
// (ClearWeapons). The scatter is `angleToPed = i * 1.75f` - an index, not a
// draw - and the line-of-sight retry is
// `GetIsLineOfSightClear(edge, pedPos, true, 0,0,0,0,0,0)` at 0x004338C6:
// **buildings only**, every dynamic flag zero, so it reads nothing but static
// collision and answers the same on every machine.
//
//   00433669  cmp  byte [ebx+314h],0 / jne ret   bInVehicle
//   00433684  mov  eax,[ebx+edi+35Ch]            m_weapons[i].m_eWeaponType
//             test eax,eax / je next             WEAPONTYPE_UNARMED
//             cmp  eax,0Ch / je next             WEAPONTYPE_DETONATOR
//   0043369E  cmp  dword [ebx+edi+368h],0 / je next   m_nAmmoTotal == 0
//   00433980  movzx edx,word [eax*2+005ED8FC]    AmmoForWeapon_OnStreet[w]
//             cmp eax,edx / jl / mov eax,edx     Min(m_nAmmoTotal, that)
//   004339A4  push 4                             PICKUP_ONCE_TIMEOUT
//   004339B3  call 00430660                      GenerateNewOne_WeaponType
//   004339BF  cmp  bp,0Dh / jl                   13 inventory slots
//   004339CB  call 004CFB70                      ClearWeapons
//
// So a weapon drop is a pure function of the ped's position and its
// inventory. Two machines holding the same ped in the same place with the
// same guns would produce byte-identical pickups - which is worth knowing
// and is *not* what makes the design work, because an observer has neither:
// AmbientPedState carries no inventory and no health.
constexpr uintptr_t CPed__CreateDeadPedWeaponPickups = 0x00433660;

// CPickups::GenerateNewOne_WeaponType(CVector pos, eWeaponType, uint8 type,
//                                     uint32 quantity). cdecl.
//
// Eleven instructions: it maps the weapon to a model through 0x00430690 (a
// jump table at 0x005EE144 returning 0AAh..0B5h) and then tail-calls
// GenerateNewOne with the same pos, type and quantity. Recorded because it
// is what the weapon half of a drop goes through - an observer replaying a
// drop uses the model-index form above and needs no weapon type at all,
// since the model is what the engine wrote into the slot.
constexpr uintptr_t CPickups__GenerateNewOne_WeaponType = 0x00430660;
constexpr uintptr_t CPickups__ModelForWeapon            = 0x00430690;

// uint16[], indexed by eWeaponType: how much ammo a weapon left on the street
// carries at most. `movzx edx,word [eax*2+005ED8FCh]` at 0x00433987, compared
// against m_nAmmoTotal and used as the cap. A third table beside AmmoForWeapon
// (0x005ED8D4) and CostOfWeapon (0x005ED924), and 0x28 bytes below the first
// of them.
constexpr uintptr_t AmmoForWeapon_OnStreet = 0x005ED8FC;

// CPed::ClearWeapons(). The tail of CreateDeadPedWeaponPickups, and the one
// thing a suppressed drop still has to do - skipping it would leave a corpse
// holding an inventory the engine meant to empty.
//
//   004CFB73  RemoveWeaponModel(GetWeaponInfo(m_weapons[m_currentWeapon])->
//                               m_nModelId)      via 00564FD0 / 004CF980
//   004CFB96  mov byte [ebx+499h],1              m_maxWeaponTypeAllowed =
//                                                WEAPONTYPE_BASEBALLBAT
//   004CFB9D  mov byte [ebx+498h],0              m_currentWeapon = UNARMED
//   004CFBC2  AddWeaponModel(...)                via 004CF8F0
//             then the 13-slot clear loop
//
// re3 Ped.cpp:4770-4790, statement for statement.
constexpr uintptr_t CPed__ClearWeapons = 0x004CFB70;

// CGame::nastyGame - the byte CreateDeadPedMoney opens on. Not read by
// CoopIII; recorded because a machine with it clear makes no money drops at
// all, which would look exactly like the seam having failed to install.
constexpr uintptr_t CGame__nastyGame = 0x005F4DD4;

// MI_MONEY. int16, runtime-filled like the eight pickup models above.
// `movzx eax,word [005F5A08]` at 0x0043362D, pushed to GenerateNewOne as the
// model index for every money pickup.
constexpr uintptr_t MI_MONEY = 0x005F5A08;

// The expiry GenerateNewOne stamps on the two drop types, in milliseconds.
// Absolute, on CTimer::m_snTimeInMilliseconds, so like every other pickup
// deadline it is local and must never travel - but the *duration* is a
// constant, and it is why a drop needs no server table and no backfill:
// every copy of it is gone within half a minute however it was made.
//
//   004305A5  cmp al,4 / add edx,4E20h   PICKUP_ONCE_TIMEOUT  20 000 ms
//   004305BB  cmp al,7 / add ecx,7530h   PICKUP_MONEY         30 000 ms
constexpr uint32_t PICKUP_ONCE_TIMEOUT_MS = 20000;
constexpr uint32_t PICKUP_MONEY_MS        = 30000;

// The server needs the same numbers and cannot include this file, so
// protocol.h carries its own copy. Two hardcoded copies of a table that only
// agree with themselves are worth nothing; this is what makes them agree with
// each other, at compile time, in both builds.
static_assert(PickupRespawnMs(PICKUP_IN_SHOP, false) ==
                  ::coopiii::PickupRespawnMs(PICKUP_IN_SHOP, false),
              "shop window agrees with protocol.h");
static_assert(PickupRespawnMs(PICKUP_ON_STREET, false) ==
                  ::coopiii::PickupRespawnMs(PICKUP_ON_STREET, false),
              "street window agrees with protocol.h");
static_assert(PickupRespawnMs(PICKUP_ON_STREET_SLOW, true) ==
                  ::coopiii::PickupRespawnMs(PICKUP_ON_STREET_SLOW, true),
              "bribe window agrees with protocol.h");
static_assert(PickupRespawnMs(PICKUP_ON_STREET_SLOW, false) ==
                  ::coopiii::PickupRespawnMs(PICKUP_ON_STREET_SLOW, false),
              "slow window agrees with protocol.h");
static_assert(PICKUP_COLLECTABLE1 == ::coopiii::PICKUP_TYPE_PACKAGE,
              "the hidden package agrees with protocol.h");
static_assert(PICKUP_TYPE_ONCE_TIMEOUT == PICKUP_ONCE_TIMEOUT &&
                  PICKUP_TYPE_MONEY == PICKUP_MONEY &&
                  PICKUP_DROP_FORGET_MS >= 2 * PICKUP_MONEY_MS,
              "a collected drop is remembered past the longest it lies on the ground");
static_assert(PickupRespawnMs(PICKUP_ONCE, false) == 0 &&
                  PickupRespawnMs(PICKUP_COLLECTABLE1, false) == 0 &&
                  PickupRespawnMs(PICKUP_MONEY, false) == 0,
              "the types that end in the engine's inline Remove() never return");

// ---- garages, doors and the Pay'n'Spray ------------------------------------
//
// Found the usual way: opcode 864 OPEN_GARAGE and 865 CLOSE_GARAGE, through
// the dispatcher at CRunningScript__ProcessCommands, the 800-range link
// (`cmp dx,384h`) to ProcessCommands800To899 (0x00448240) and the table at
// 0x005EF77C (base opcode 800), entries 64 and 65. Both handlers are four
// instructions long and between them they name the array, its stride and both
// mutators. Opcodes 944/945 IS_GARAGE_OPEN / IS_GARAGE_CLOSED - table
// 0x005EFA14, base 900, entries 44/45 - name the two predicates and pin the
// state enum's values from the other end.
//
// NOTHING BELOW CAME OFF re3's DECLARATIONS. re3 happens to agree on 140 and
// on the whole field layout; that was checked afterwards. Five re3 constants
// have been refuted on this project.

// CGarage aGarages[32], stride 0x8C. Three independent witnesses:
//
//   1. COMMAND_OPEN_GARAGE's handler (0x0044B230)
//        0044B243  mov ecx,[006ED460h]        ScriptParams[0], the index
//        0044B249  imul ecx,ecx,8Ch           stride 140
//        0044B24F  add ecx,72BCD0h            the array
//        0044B255  call 00426F20              CGarage::OpenThisGarage
//      and 0x0044B275 is the same four instructions with 0x00426F40.
//   2. CGarages::IsGarageOpen (0x00426CF0), which builds 140*idx out of three
//      `lea`s and then reads the byte at +1:
//        0F BF 4C 24 04         movsx ecx,word [esp+4]
//        8D 04 CD 00 00 00 00   lea eax,[ecx*8]
//        8D 04 81               lea eax,[ecx+eax*4]     33*idx
//        8D 04 48               lea eax,[eax+ecx*2]     35*idx
//        8A 14 85 D1 BC 72 00   mov dl,[eax*4 + 0072BCD1h]
//      35*4 = 140, and the +1 in the displacement is m_eGarageState.
//   3. CGarages::Update (0x00421E40), whose loop is the bound:
//        00421E74  mov esi,72BCD0h
//        00421E94  cmp byte [ebp+0072BCD0h],0    IsUsed(): type != GARAGE_NONE
//        00421E9F  call 004222D0                 CGarage::Update, this in ecx
//        00421EA5  add ebp,8Ch / add esi,8Ch
//        00421EB1  cmp ebx,20h / jl              32 garages, not NumGarages
//
// The loop bound is the literal 0x20, not CGarages::NumGarages, so 32 is the
// number to walk and a garage the script never created is skipped by its own
// type byte being zero.
constexpr uintptr_t CGarages__aGarages = 0x0072BCD0;
constexpr size_t    NUM_GARAGES        = 32;
constexpr size_t    SIZEOF_GARAGE      = 0x8C;

// CGarage::Update, __thiscall, one garage. This is the whole state machine and
// it is the function CoopIII detours.
//
//   004222D0  53 56 57 55        push ebx/esi/edi/ebp
//   004222D4  89 CD              mov ebp,ecx            this
//   004222D6  81 EC 08 02 00 00  sub esp,208h
//   004222DC  80 7D 00 0D        cmp byte [ebp+0],0Dh   type != GARAGE_CRUSHER
//
// That last compare is the first line of re3's Garages.cpp:1220 and it pins
// both m_eGarageType at +0 and GARAGE_CRUSHER == 13, i.e. the whole eGarageType
// enum, since it is dense from GARAGE_NONE == 0.
constexpr uintptr_t CGarage__Update            = 0x004222D0;
constexpr uintptr_t CGarages__Update           = 0x00421E40;
constexpr uintptr_t CGarage__OpenThisGarage    = 0x00426F20;
constexpr uintptr_t CGarage__CloseThisGarage   = 0x00426F40;
constexpr uintptr_t CGarage__UpdateDoorsHeight = 0x00426730;
constexpr uintptr_t CGarages__IsGarageOpen     = 0x00426CF0;
constexpr uintptr_t CGarages__IsGarageClosed   = 0x00426D20;

// The two mutators in full, because they are twenty bytes between them and
// they are the *only* legal transitions into a moving state:
//
//   OpenThisGarage  0x00426F20
//     8A 41 01        mov al,[ecx+1]
//     84 C0 74 08     test al,al / je set          GS_FULLYCLOSED
//     3C 02 74 04     cmp al,2   / je set          GS_CLOSING
//     3C 05 75 04     cmp al,5   / jne ret         GS_CLOSEDCONTAINSCAR
//     C6 41 01 03     mov byte [ecx+1],3           GS_OPENING
//     C3
//
//   CloseThisGarage 0x00426F40
//     8A 41 01        mov al,[ecx+1]
//     3C 01 74 04     cmp al,1 / je set            GS_OPENED
//     3C 03 75 04     cmp al,3 / jne ret           GS_OPENING
//     C6 41 01 02     mov byte [ecx+1],2           GS_CLOSING
//     C3
//
// Neither touches m_fDoorPos. A garage that is already open is left alone by
// OpenThisGarage - which is exactly why HoldGarageOpen below cannot use it for
// the common case and has to write GS_OPENED itself. See garage.cpp.

namespace offs {

// CGarage, 0x8C bytes. Every offset below is either written or read by
// CGarage::Update at the address in the comment; the Pay'n'Spray arm alone
// witnesses eleven of them.
constexpr size_t GARAGE_TYPE       = 0x00; // uint8  0x004222DC cmp byte [ebp+0],0Dh
constexpr size_t GARAGE_STATE      = 0x01; // uint8  0x00426F2F mov byte [ecx+1],3
constexpr size_t GARAGE_CLOSING_NO_CAR = 0x03; // bool
constexpr size_t GARAGE_DEACTIVATED    = 0x04; // bool
constexpr size_t GARAGE_RESPRAY_HAPPENED = 0x05; // bool 0x00422AEA mov [ebp+5],1
constexpr size_t GARAGE_TARGET_MODEL = 0x08; // int32
constexpr size_t GARAGE_DOOR1      = 0x0C; // CEntity*
constexpr size_t GARAGE_DOOR2      = 0x10; // CEntity*
constexpr size_t GARAGE_ROTATED_DOOR = 0x19; // bool 0x00422B2F mov dl,[ebp+19h]
constexpr size_t GARAGE_X1         = 0x1C; // float 0x00422949 fld [ebp+1Ch]
constexpr size_t GARAGE_X2         = 0x20; // float 0x0042294E fld [ebp+20h]
constexpr size_t GARAGE_Y1         = 0x24; // float 0x00422991 fld [ebp+24h]
constexpr size_t GARAGE_Y2         = 0x28; // float 0x00422996 fld [ebp+28h]
constexpr size_t GARAGE_Z1         = 0x2C; // float
constexpr size_t GARAGE_Z2         = 0x30; // float
constexpr size_t GARAGE_DOOR_POS   = 0x34; // float 0x00422B96 fstp [ebp+34h]
constexpr size_t GARAGE_DOOR_HEIGHT = 0x38; // float 0x00422B9C fcomp [ebp+38h]
constexpr size_t GARAGE_DOOR1_Z    = 0x4C; // float 0x00422A13 fld [ebp+4Ch]
constexpr size_t GARAGE_TIME_TO_ACT = 0x54; // uint32 0x004226B0 mov eax,[ebp+54h]
constexpr size_t GARAGE_TARGET     = 0x5C; // CVehicle*

} // namespace offs

static_assert(offs::GARAGE_TARGET + 4 + 4 + 0x28 == SIZEOF_GARAGE,
              "m_pTarget, the unused pointer after it and the 0x28-byte "
              "CStoredCar are the tail of the 140 the stride proves");

// eGarageState. The values are not taken from re3's enum: 0, 2 and 5 are the
// three OpenThisGarage accepts, 1 and 3 are the two CloseThisGarage accepts,
// 4 is written by the Pay'n'Spray's GS_OPENING arm at 0x00422BAF
// (`mov byte [ebp+1],4`), and IsGarageOpen's `cmp dl,1 / cmp dl,4` plus
// IsGarageClosed's `cmp byte [..],0` say which of them count as open and as
// shut. Six of the seven are therefore witnessed by an instruction; only
// GS_AFTERDROPOFF is inferred from the gap, and CoopIII never writes it.
enum eGarageState : uint8_t {
	GS_FULLYCLOSED       = 0,
	GS_OPENED            = 1,
	GS_CLOSING           = 2,
	GS_OPENING           = 3,
	GS_OPENEDCONTAINSCAR = 4,
	GS_CLOSEDCONTAINSCAR = 5,
	GS_AFTERDROPOFF      = 6,
};

// eGarageType, dense from 0, with GARAGE_CRUSHER == 13 nailed by Update's own
// first instruction and GARAGE_RESPRAY == 5 by the jump table at 0x005ED168
// that the respray arm dispatches its five states through.
enum eGarageType : uint8_t {
	GARAGE_NONE                          = 0,
	GARAGE_MISSION                       = 1,
	GARAGE_BOMBSHOP1                     = 2,
	GARAGE_BOMBSHOP2                     = 3,
	GARAGE_BOMBSHOP3                     = 4,
	GARAGE_RESPRAY                       = 5,
	GARAGE_COLLECTORSITEMS               = 6,
	GARAGE_COLLECTSPECIFICCARS           = 7,
	GARAGE_COLLECTCARS_1                 = 8,
	GARAGE_COLLECTCARS_2                 = 9,
	GARAGE_COLLECTCARS_3                 = 10,
	GARAGE_FORCARTOCOMEOUTOF             = 11,
	GARAGE_60SECONDS                     = 12,
	GARAGE_CRUSHER                       = 13,
	GARAGE_MISSION_KEEPCAR               = 14,
	GARAGE_FOR_SCRIPT_TO_OPEN            = 15,
	GARAGE_HIDEOUT_ONE                   = 16,
	GARAGE_HIDEOUT_TWO                   = 17,
	GARAGE_HIDEOUT_THREE                 = 18,
	GARAGE_FOR_SCRIPT_TO_OPEN_AND_CLOSE  = 19,
	GARAGE_KEEPS_OPENING_FOR_SPECIFIC_CAR = 20,
	GARAGE_MISSION_KEEPCAR_REMAINCLOSED  = 21,
};

// ---- the Pay'n'Spray, which is three things and not one --------------------
//
// The whole of it is one arm of CGarage::Update: GS_FULLYCLOSED, once
// m_nTimeToStartAction has passed. Transcribed from 0x004226B0, in order,
// because every one of these lines is a decision about what has to travel:
//
//   004226B0  mov eax,[ebp+54h]            m_nTimeToStartAction
//   004226B3  mov ebx,[00885B48h]          CTimer::m_snTimeInMilliseconds
//   004226B9  cmp ebx,eax / jbe out
//   004226C8  mov byte [ebp+1],3           GS_OPENING   <- the edge CoopIII watches
//   004226D0  call 0057CC20                PlayFrontEndSound(SOUND_GARAGE_OPENING)
//   004226DA  call 004A1150                FindPlayerPed
//   004226DF  mov eax,[eax+53Ch]           CPed::m_pWanted  (sizeof(CPed) == 0x53C)
//   004226E5  cmp dword [eax+18h],0        m_nWantedLevel  -> bTakeMoney
//   004226FB  call 004AD790                CWanted::Reset()        <- 1. WANTED
//   00422702  call 00492F60 / and [eax+DFh],0FBh   SetEnablePlayerControls
//   0042271A  and byte [wanted+16h],0FEh   m_bIgnoredByCops = false
//   00422722  call 004A10C0 / test / je    if (!FindPlayerVehicle()) skip
//   00422734  mov eax,[eax+284h]           IsCar()
//   00422747  fld [eax+200h]               m_fHealth < threshold -> bTakeMoney
//   00422767  mov dword [eax+200h],447A0000h   m_fHealth = 1000.0f   <- 2. REPAIR
//   00422776  mov dword [eax+530h],0       m_fFireBlowUpTimer = 0
//   00422787  call 0053C240 (ecx = car)    CAutomobile::Fix()        <- 2. REPAIR
//   00422791  fld [eax+2Ch] / fcomp 0      GetUp().z < 0 -> flip up and right
//   0042284E  mov al,[eax+4D9h] / shr 5    bFixedColour -> skip the repaint
//   0042287F  call 00520FD0                ChooseVehicleColour       <- 3. PAINT
//   00422901  mov [eax+19Ch],dl            m_currentColour1
//   00422913  mov [eax+19Dh],bl            m_currentColour2
//   00422926  call 005A41D0 x 600          the spray particles, GetRandomNumber
//   00422A63  call 00428000                CenterCarInGarage
//   00422AEA  mov byte [ebp+5],1           m_bResprayHappened = true
//
// Three effects, three different answers about what travels. See
// client/src/game/garage.h.
//
// CAutomobile::Fix itself is declared with the rest of the damage model above,
// at the "it is an event, not a number" block. The damage work and the garage
// work found it independently and agreed on 0x0053C240, which is worth saying
// once rather than declaring twice.

// Recorded, and deliberately NOT called by this feature. CWanted::Reset is the
// wanted level's own seam and wanted.cpp owns it; garage.cpp marks the
// place and clears nothing. See "the wanted seam" in garage.h.
constexpr uintptr_t CWanted__Reset = 0x004AD790;

// ---- the respray colour is not random, and that is worse -------------------
//
// docs/roadmap.md 5.9 says the model info "picks those at random", which is
// true of the extra components and NOT of the colours. The retail
// CVehicleModelInfo::ChooseVehicleColour contains no call to
// CGeneral::GetRandomNumber at all:
//
//   00520FD0  53                   push ebx
//   00520FD1  89 CB                mov ebx,ecx                 this
//   00520FD3  8A 83 D4 01 00 00    mov al,[ebx+1D4h]           m_numColours
//   00520FDB  84 C0 ... 75 0D      if (numColours == 0) { *col1 = *col2 = 0; ret 8 }
//   00520FF1  0F B6 C8             movzx ecx,al
//   00520FF4  0F B6 83 D5 01 00 00 movzx eax,[ebx+1D5h]        m_lastColorVariation
//   00520FFB  40 99 F7 F9          inc eax / cdq / idiv ecx    (last + 1) % numColours
//   00520FFF  88 93 D5 01 00 00    mov [ebx+1D5h],dl
//   0052100B  8A 8C 03 C4 01 00 00 mov cl,[ebx+eax+1C4h]       m_colours1[]
//   0052101C  8A 94 03 CC 01 00 00 mov dl,[ebx+eax+1CCh]       m_colours2[]
//   00521030  76 68                if (numColours <= 1) ret
//   00521032  E8 89 00 F8 FF       call 004A10C0               FindPlayerVehicle
//   0052103E  0F BF 48 5C          movsx ecx,[eax+5Ch]         its model index
//   00521042  39 1C 8D 08 D4 83 00 cmp [0083D408h + ecx*4],ebx same model info?
//   0052104E  8A 90 9C 01 00 00    ... compare both colours, and if they match
//                                  advance m_lastColorVariation once more
//
// So it is a **round robin over the model's own colour table**, plus a
// tiebreak against whatever the local player happens to be driving. Both
// halves are machine-local state: m_lastColorVariation counts every car of
// that model this engine has ever created, and FindPlayerVehicle is a
// different car on every machine.
//
// The conclusion is the same as if it had been an RNG roll - the colour has to
// travel - but the reason is not, and it is a stronger reason. An RNG could in
// principle be seeded to agree. A counter over "how many Kurumas has this
// process made" cannot be, and neither can "what is the local player sitting
// in right now". This is the third time this class has bitten the project
// (vehicle extras and vehicle colours are roadmap 5.9); it is the same class,
// and re3's source would have given the wrong reason for it.
constexpr uintptr_t CVehicleModelInfo__ChooseVehicleColour = 0x00520FD0;

namespace offs {
constexpr size_t MODELINFO_COLOURS1    = 0x1C4; // uint8[8]
constexpr size_t MODELINFO_COLOURS2    = 0x1CC; // uint8[8]
constexpr size_t MODELINFO_NUM_COLOURS = 0x1D4; // uint8
constexpr size_t MODELINFO_LAST_COLOUR = 0x1D5; // uint8, the round-robin cursor
} // namespace offs

static_assert(offs::MODELINFO_COLOURS2 == offs::MODELINFO_COLOURS1 + 8 &&
                  offs::MODELINFO_NUM_COLOURS == offs::MODELINFO_COLOURS2 + 8,
              "ChooseVehicleColour's two `mov cl,[ebx+eax+X]` displacements "
              "and the count it divides by are consecutive");

// ---- which way a garage rests, which is the only classification we need ----
//
// Every type's GS_OPENING and GS_CLOSING arms are the same four statements:
// ramp m_fDoorPos by the door speed times CTimer::ms_fTimeStep, call
// UpdateDoorsHeight(), and on reaching the limit take the resting state and
// play a sound. Every type's *resting* arms are the ones that look at
// FindPlayerPed / FindPlayerVehicle / FindPlayerCoors and decide something.
//
// So a garage has exactly two resting positions and each type sits at one of
// them when nobody is using it:
//
//   - the shops stand open and close over a car being worked on: RESPRAY,
//     the three BOMBSHOPs and the CRUSHER. Their GS_OPENED arm is the one
//     that starts a visit.
//   - everything else stands shut and opens for somebody: the three HIDEOUTs
//     (which are the safehouse garages - init.sc calls them GARAGE_SAVEONE,
//     SAVETWO and SAVETHREE), the mission lockups, the collection garages and
//     the script-driven ones.
//
// That is the whole classification this feature needs, and it is why the wire
// carries one bit per garage rather than a state: "my engine has this garage
// away from where it rests".
constexpr bool GarageRestsOpen(uint8_t type) {
	return type == GARAGE_RESPRAY || type == GARAGE_BOMBSHOP1 ||
	       type == GARAGE_BOMBSHOP2 || type == GARAGE_BOMBSHOP3 ||
	       type == GARAGE_CRUSHER;
}

// IsGarageOpen's own test, byte for byte: `cmp dl,1 / je true / cmp dl,4 /
// jne false`. GS_OPENING and GS_CLOSING are deliberately *not* open - a door
// on its way up is not up yet, and IsGarageOpen agrees.
constexpr bool GarageStateIsOpen(uint8_t state) {
	return state == GS_OPENED || state == GS_OPENEDCONTAINSCAR;
}

// Which side of its resting position the door is heading for. A moving state
// is classified by its destination, not by where the door has got to: that is
// the whole "the transition travels, the door position does not" argument in
// one function.
constexpr bool GarageHeadingOpen(uint8_t state) {
	return state == GS_OPENED || state == GS_OPENEDCONTAINSCAR ||
	       state == GS_OPENING;
}

// The one bit that goes on the wire, per garage: is this machine's own state
// machine away from where this type of garage rests?
constexpr bool GarageDeviates(uint8_t type, uint8_t state) {
	if (type == GARAGE_NONE)
		return false;
	return GarageHeadingOpen(state) != GarageRestsOpen(type);
}
// ---- breakable street objects ---------------------------------------------
//
// docs/objects.md is the investigation and the design. This is the first time
// this project has touched `CObject`, so every one of these was walked out of
// the retail image rather than read off re3, and where re3 was checked and
// turned out to be right that is said too - four re3 constants have been
// refuted here and one was very nearly a fifth (the pool stride below).
//
// The anchor for the whole layout is CObjectData::SetObjectData, which copies
// ten fields out of the object.dat table into a CObject in one straight run.
// One function pins five CObject offsets and the two flag bytes at once,
// which is why it is the thing to find first.
//
//   004BC270  mov eax,[esp+4] / mov ecx,[esp+8]     modelId, CObject&
//   004BC278  mov eax,[eax*4+0083D408h]             ms_modelInfoPtrs
//   004BC281  cmp word [edx+24h],0FFFFh / je ret    GetObjectID() == -1
//   004BC290  shl edx,5 / add edx,6F4708h           ms_aObjectInfo[id], 0x20
//   004BC299  fld [edx]     / fstp [ecx+0C0h]       m_fMass
//   004BC2A1  fld [edx+4]   / fstp [ecx+0C4h]       m_fTurnMass
//   004BC2AA  fld [edx+8]   / fstp [ecx+0CCh]       m_fAirResistance
//   004BC2B3  fld [edx+0Ch] / fstp [ecx+0D0h]       m_fElasticity
//   004BC2BC  fld [edx+10h] / fstp [ecx+0D4h]       m_fBuoyancy
//   004BC2C5  fld [edx+14h] / fstp [ecx+170h]       m_fUprootLimit
//   004BC2CE  fld [edx+18h] / fstp [ecx+178h]       m_fCollisionDamageMultiplier
//   004BC2D7  mov al,[edx+1Ch] / mov [ecx+17Ch],al  m_nCollisionDamageEffect
//   004BC2E0  mov al,[edx+1Dh] / mov [ecx+17Dh],al  m_nSpecialCollisionResponseCases
//   004BC2E9  mov al,[edx+1Eh] / mov [ecx+17Eh],al  m_bCameraToAvoidThisObject
//   004BC2F2  fld [edx] / fcomp [005F7E78h]         m_fMass >= 99998.0f
//   004BC301  [ecx+122h] &= 0FBh, |= 4              bInfiniteMass  = true
//   004BC311  [ecx+122h] &= 0FDh                    bAffectedByGravity = false
//   004BC31F  [ecx+52h]  &= 0FDh, |= 2              bExplosionProof = true
//
// re3 ObjectData.cpp:72-104 statement for statement, and it independently
// re-confirms two bits this file already had from elsewhere: bExplosionProof
// is byte B bit 1, and the CPhysical flag byte lives at +0x122 in re3's
// declared order (bIsHeavy 0, bAffectedByGravity 1, bInfiniteMass 2).
constexpr uintptr_t CObjectData__SetObjectData   = 0x004BC270;   // __cdecl
constexpr uintptr_t CObjectData__ms_aObjectInfo  = 0x006F4708;
constexpr size_t    SIZEOF_OBJECTINFO            = 0x20;
constexpr size_t    MODELINFO_OBJECT_ID          = 0x24;   // int16, -1 == not an object

namespace object {

// CObject, on top of CPhysical. Every offset below is witnessed by an
// instruction quoted in this block; none of them is arithmetic off re3.
constexpr size_t OBJECT_MATRIX     = 0x128;   // CMatrix, the ORIGINAL placement
constexpr size_t OBJECT_MATRIX_POS = 0x158;   // OBJECT_MATRIX + CMatrix's 0x30
constexpr size_t UPROOT_LIMIT      = 0x170;   // float
constexpr size_t CREATED_BY        = 0x174;   // int8, eObjectCreatedBy
constexpr size_t OBJECT_FLAGS      = 0x175;   // the seven-bit bitfield byte
constexpr size_t BONUS_VALUE       = 0x176;   // int8
constexpr size_t DAMAGE_MULTIPLIER = 0x178;   // float
constexpr size_t DAMAGE_EFFECT     = 0x17C;   // uint8, eCollisionDamageEffect
constexpr size_t SPECIAL_RESPONSE  = 0x17D;   // uint8, COLLRESPONSE_*
constexpr size_t CAMERA_AVOID      = 0x17E;   // bool
constexpr size_t END_OF_LIFE_TIME  = 0x184;   // uint32, TEMP_OBJECT expiry
constexpr size_t REF_MODEL_INDEX   = 0x188;   // int16
constexpr size_t COLOUR1           = 0x194;   // int8
constexpr size_t COLOUR2           = 0x195;   // int8

// CPhysical::m_fDamageImpulse and the entity that caused it, straight out of
// CObject::ProcessControl's first two instructions - `push [ebx+10Ch]` is the
// argument to ObjectDamage and re3 Object.cpp:85 says that argument is
// m_fDamageImpulse. m_pDamageEntity is its declared neighbour; nothing below
// trusts it for anything a wrong answer could break (docs/objects.md).
constexpr size_t DAMAGE_IMPULSE    = 0x10C;   // float,     CPhysical
constexpr size_t DAMAGE_ENTITY     = 0x110;   // CEntity*,  CPhysical

// eObjectCreatedBy, +0x174. The value is what decides whether an object is
// the map's or somebody's, and it is the whole of CObject::CanBeDeleted
// (0x004BB010, which returns false for 2 and 4 and true for everything else).
constexpr uint8_t UNKNOWN_OBJECT  = 0;
constexpr uint8_t GAME_OBJECT     = 1;   // placed by the map. what we sync.
constexpr uint8_t MISSION_OBJECT  = 2;   // the script's
constexpr uint8_t TEMP_OBJECT     = 3;   // debris, self-expiring
constexpr uint8_t CUTSCENE_OBJECT = 4;

// The bitfield byte at +0x175, in re3 Object.h's declaration order.
// bHasBeenDamaged is the one that is witnessed directly, twice: the explosion
// arm writes it (`[ebp+175h] &= 0DFh / |= 20h` at 0x004B15CE) and
// CObject::ProcessControl reads it (`[ebx+175h] >> 5 & 1` at 0x004BB177) in
// the exploding-barrel branch re3 Object.cpp:99 describes. bUseVehicleColours
// is witnessed by CObject::Render reading bit 6 at 0x004BB20C beside the
// TEMP_OBJECT and m_nRefModelIndex tests re3 Object.cpp:120 has in the same
// order, which pins the two ends of the byte and so the order between them.
constexpr uint8_t OBJ_IS_PICKUP             = 1 << 0;
constexpr uint8_t OBJ_PICKUP_WITH_MESSAGE   = 1 << 1;
constexpr uint8_t OBJ_OUT_OF_STOCK          = 1 << 2;
constexpr uint8_t OBJ_GLASS_CRACKED         = 1 << 3;
constexpr uint8_t OBJ_GLASS_BROKEN          = 1 << 4;
constexpr uint8_t OBJ_HAS_BEEN_DAMAGED      = 1 << 5;
constexpr uint8_t OBJ_USE_VEHICLE_COLOURS   = 1 << 6;

// bRenderDamaged is CEntity flags byte B bit 7 - the top bit of the byte that
// already holds bExplosionProof (bit 1), bIsVisible (bit 2) and
// bRenderScorched (bit 4). Witnessed by ObjectDamage's own case 1, which is
// the whole of DAMAGE_EFFECT_CHANGE_MODEL:
//
//   004BB390  mov al,[ecx+52h] / and al,7Fh / or al,80h / mov [ecx+52h],al
//
// re3 Entity.h:57 is the last flag in flagsB, so that bit lands exactly where
// re3's declaration order puts it. It is read again by case 4, which tests
// the same bit (`shr al,7` at 0x004BB403) to decide whether this is the first
// hit or the second.
constexpr uint8_t ENTITY_RENDER_DAMAGED = 0x80;   // byte B, +0x52

// eCollisionDamageEffect. Read off ObjectDamage's own compare chain, which
// walks the effect down in the engine's order and so states the values
// exactly: 1, 2, 3, 4, then 50, 60, 70, 80.
//
//   004BB336  movzx eax,byte [ecx+17Ch] / test eax,eax / je end      0
//   004BB345  sub eax,1 / je 004BB390                                1
//   004BB34A  sub eax,1 / je end                                     2  (no-op)
//   004BB353  sub eax,1 / je 004BB3A0                                3
//   004BB358  sub eax,1 / je 004BB400                                4
//   004BB361  sub eax,2Eh / je 004BB480                             50
//   004BB36A  sub eax,0Ah / je 004BB6D5                             60
//   004BB373  sub eax,0Ah / je 004BB961                             70
//   004BB37C  sub eax,0Ah / je 004BBB42                             80
//
// The game's own data/object.dat documents the same list in its header
// comment, which is a second, independent witness that costs nothing to read.
constexpr uint8_t DAMAGE_EFFECT_NONE              = 0;
constexpr uint8_t DAMAGE_EFFECT_CHANGE_MODEL      = 1;
constexpr uint8_t DAMAGE_EFFECT_SPLIT_MODEL       = 2;   // does nothing
constexpr uint8_t DAMAGE_EFFECT_SMASH_COMPLETELY  = 3;
constexpr uint8_t DAMAGE_EFFECT_CHANGE_THEN_SMASH = 4;
constexpr uint8_t DAMAGE_EFFECT_SMASH_CARDBOARD   = 50;
constexpr uint8_t DAMAGE_EFFECT_SMASH_WOODENBOX   = 60;
constexpr uint8_t DAMAGE_EFFECT_SMASH_TRAFFICCONE = 70;
constexpr uint8_t DAMAGE_EFFECT_SMASH_BARPOST     = 80;

// The threshold every break in the game goes through, and the one number an
// applier has to beat. 0x005F7D88 reads 43160000.
constexpr float OBJECT_DAMAGE_THRESHOLD = 150.0f;

// __thiscall void CObject::ObjectDamage(float amount).  `ret 4`.
//
// **Breaking is a state, not a destroy-and-replace.** Nothing in this
// function frees the object, removes it from the world or allocates
// anything - every arm writes flags on the CObject that is already there.
// That is what makes the whole feature a latch rather than a lifecycle, and
// it is worth saying because the obvious guess (the engine swaps in a broken
// object) is wrong for every case.
//
// Two gates, in this order, both of which an applier has to satisfy:
//
//   004BB24A  cmp byte [ecx+17Ch],0 / je ret          m_nCollisionDamageEffect == 0
//   004BB270  mov al,[ecx+51h] / and al,1 / je ret    !bUsesCollision
//
// then MI_BODYCAST's special case, then the one threshold the whole thing
// turns on:
//
//   004BB319  fld st(0) / fmul [ecx+178h] / fcomp [005F7D88h]
//                                           amount * mult > 150.0f
//
// The other constants in the bodycast arm are 50.0f (0x005F7D7C), 0.5f
// (0x005F7D80) and 0.0f (0x005F7D84), and nBodyCastHealth is an int16 at
// 0x005F7D4C, so re3 Object.cpp:158-167 is right down to the field type.
//
// The two outcomes, as the engine writes them:
//
//   case 1   bRenderDamaged = true                                  (004BB390)
//   case 2   nothing at all
//   case 3   bIsVisible=0, bUsesCollision=0, bIsStatic=1,
//            bExplosionProof=1, m_vecMoveSpeed=0, m_vecTurnSpeed=0  (004BB3A0)
//   case 4   bRenderDamaged ? the case-3 block : bRenderDamaged=true (004BB400)
//   50/60/70/80  the case-3 block, then particles and a sound
//
// which is why one byte of "how broken is it" is enough on the wire: there
// are two visible states, and case 4 is the only thing that needs both.
constexpr uintptr_t CObject__ObjectDamage   = 0x004BB240;

// The rest of the CObject block, for orientation and because two of them are
// how the offsets above were confirmed a second time.
constexpr uintptr_t CObject__CObject        = 0x004BABD0;   // CObject::CObject(void)
constexpr uintptr_t CObject__CanBeDeleted   = 0x004BB010;
constexpr uintptr_t CObject__ProcessControl = 0x004BB040;
constexpr uintptr_t CObject__Render         = 0x004BB1E0;
constexpr uintptr_t CObject__operator_new   = 0x004BAE70;

// Every call site of ObjectDamage in the image, found by scanning .text for
// E8/E9 rel32 targets equal to 0x004BB240. **Six, and that is all of them**:
//
//   00497AA5   CPhysical, the ped-or-vehicle against a static object arm
//   0049D84B   CPhysical, the two-moving-entities arm
//   004B1626   CWorld::TriggerExplosionSectorList, the static arm
//   004B1A24   CWorld::TriggerExplosionSectorList, the moving arm
//   004BB055   CObject::ProcessControl, with m_fDamageImpulse
//
// (five sites; 0x00497AA5 and 0x0049D84B are the two CPhysical arms and the
// compiler merged re3 Physical.cpp:1761 and :1764 into the second.)
//
// **Nothing in CWeapon is on that list**, which is the finding that shrank
// this whole job: a bullet that hits an object runs the ENTITY_TYPE_OBJECT
// arm of CWeapon::FireInstantHit, and that arm adds eight spark particles,
// clears bIsStatic when m_fUprootLimit <= 0 and applies `normal * -4.0f` -
// and then stops. It never calls ObjectDamage. Shooting street furniture in
// retail III nudges it; it does not break it. The one exception is
// CWeapon::BlowUpExplosiveThings, which turns a barrel or a petrol pump into
// a CExplosion - and an explosion is already agreed on every machine.
constexpr uintptr_t OBJECT_DAMAGE_CALLSITES[] = {
    0x00497AA5, 0x0049D84B, 0x004B1626, 0x004B1A24, 0x004BB055,
};

// ---- the object pool ------------------------------------------------------
//
// CPools::Initialise (0x004A1770) constructs the pools in one run and stores
// each one immediately after starting the next allocation, so the size that
// belongs to a pointer is the `push` *before* the store:
//
//   004A182E  push 1C2h   then 004A1835  mov [00880E28],eax
//   004A184B  push 0AF2h  then 004A1852  mov [008F2C18],eax
//
// 450 objects and 2802 dummies, which is re3's NUMOBJECTS and NUMDUMMIES
// exactly - as are the building (5500) and treadable (1214) pools in the same
// run. The object pool pointer also falls out of CPools::CheckPoolsEmpty's
// "Objects left %d" at 0x004A191D, independently.
constexpr uintptr_t CPools__ms_pDummyPool = 0x008F2C18;
constexpr int32_t   OBJECT_POOL_SIZE      = 450;
constexpr int32_t   DUMMY_POOL_SIZE       = 2802;

// **The slot stride is 0x19C and sizeof(CObject) is 0x198.** Both numbers are
// true and using the wrong one walks off the end of an entity. Three
// independently compiled `GetSlot` sites agree on the stride:
//
//   00408529  imul eax,eax,19Ch      (beside `mov esi,[00880E28]`)
//   00434000  imul ebp,ebp,19Ch      (beside `mov ebx,[00880E28]`)
//   004F3BF6  imul eax,eax,19Ch      CPopulation::ManagePopulation
//
// This looked like a fifth refuted re3 constant and is not one. re3 declares
// the pool as `CPool<CObject, CCutsceneHead>` and CPool allocates
// `sizeof(U)`, and `VALIDATE_SIZE(CCutsceneHead, 0x19C)` - the derived type
// is one RwFrame* longer and that is what the array is made of. re3 was
// right; what would have been wrong is reading VALIDATE_SIZE(CObject, 0x198)
// as the stride, which is the mistake this comment exists to stop.
constexpr size_t OBJECT_POOL_STRIDE = 0x19C;

// CPool's own three fields, from ManagePopulation's inlined walk:
// `mov esi,[ebx+8]` is the size, `mov esi,[ebx+4]` the flag array,
// `mov esi,[ebx]` the entry array, and `and eax,80h` on a flag byte is
// POOLFLAG_ISFREE. Same shape for every pool in the game.
constexpr size_t  POOL_ENTRIES    = 0x00;
constexpr size_t  POOL_FLAGS      = 0x04;
constexpr size_t  POOL_SIZE       = 0x08;
constexpr uint8_t POOLFLAG_ISFREE = 0x80;

// ---- the 80 m horizon, which is the reason none of this needs a backfill ---
//
// CPopulation::ManagePopulation (0x004F3B90) walks 1/32 of the object pool
// and 1/32 of the dummy pool per frame, keyed off CTimer::m_FrameCounter & 31,
// and moves objects across the line in both directions:
//
//   004F3B99  mov bp,[009412ECh] / and ebp,1Fh     the 1/32 slice
//   004F3BF6  imul eax,eax,19Ch                    object pool walk
//   004F3C50  call 004BB010                        CanBeDeleted
//   004F3C65  lea eax,[esi+34h]                    GetPosition()
//   004F3C9C  lea eax,[esi+158h]                   m_objectMatrix.GetPosition()
//   004F3CDA  fcom [005FA890h]                     both against 80.0f
//
// 0x005FA890 reads 42A00000 = 80.0f. Past that line a GAME_OBJECT is handed
// to CPopulation::ConvertToDummyObject, which builds a CDummyObject out of
// nothing but the model index, the RwObject and m_level (re3
// DummyObject.cpp:6-13) and places it at m_objectMatrix - so **every flag
// ObjectDamage wrote is thrown away**, and walking back in builds a brand
// new, pristine CObject. That is retail single-player behaviour, not a bug:
// street furniture stands back up once you have left the block.
//
// Two consequences the design leans on directly. A broken object is state
// with an 80 m horizon and a lifetime of one visit, so there is nothing for
// the server to remember and nothing to tell a joiner. And it is the reason
// roadmap.md 5.8's literal answer - an ownerless world entity is the host's -
// cannot transplant: an object 80 m from the host is not a CObject on the
// host at all, so the host has nothing to observe and nothing to report.
constexpr uintptr_t CPopulation__ManagePopulation = 0x004F3B90;
constexpr float     OBJECT_DUMMY_RANGE            = 80.0f;

// The other half of that horizon, and the reason the key below is
// m_objectMatrix rather than the live position: an IPL instance whose model
// has an object.dat entry becomes a CDummyObject and nothing else, and the
// matrix it is placed at survives every round trip through the two
// conversions untouched. CFileLoader::LoadObjectInstance's whole decision is
// `mi->GetObjectID() == -1`, i.e. the same +0x24 word SetObjectData tests
// above: -1 makes a CBuilding or a CTreadable, anything else makes a dummy.
// (ConvertToRealObject and ConvertToDummyObject themselves are deliberately
// not listed: nothing here calls them, and an address nothing uses is an
// address nobody re-checks.)

// ---- an explosion breaks objects identically on every machine --------------
//
// The object arm of CWorld::TriggerExplosionSectorList (0x004B1340), which is
// the companion to the vehicle arm this file already records above:
//
//   004B15C8  [ebp+175h] &= 0DFh / |= 20h        bHasBeenDamaged = true
//   004B15D8  [ebp+51h] >> 2 & 1                 GetIsStatic()
//   004B15E4  fld [esp+0CCh] / fsub [esp+10h]    fRadius - fMagnitude
//   004B15EF  fmul [005F799Ch]                   * 2.0f
//   004B15F5  fdiv [esp+0CCh]                    / fRadius
//   004B15FE  fcomp [005F7994h]                  Min(.., 1.0f)
//   004B1616  fld [005F79A0h] / fmul             * 300.0f
//   004B1626  call 004BB240                      CObject::ObjectDamage
//
// and the not-static arm at 0x004B1A24 is the same 300.0f multiply. The three
// constants read 40000000, 3F800000 and 43960000 - 2.0f, 1.0f, 300.0f - so
// re3 World.cpp:2050-2078 and :2137 are right.
//
// **The amount is a pure function of the two positions and the radius.** No
// RNG, no impulse, no timestep, nothing carried over from a previous frame.
// ObjectDamage then compares it against a multiplier that came out of
// object.dat, which is the same file on every install. So an explosion
// CoopIII already replays at an agreed world position (docs/protocol.md
// 1.9.3) breaks exactly the same objects on every machine, for free, and
// sending anything about them would be a duplicate.
//
// CWorld::TriggerExplosion (0x004B1140, __cdecl) has **exactly two callers in
// the whole image**, 0x00559FD3 and 0x0055A185, both inside CExplosion - so
// it is also the one place a guard has to sit to recognise "this break was an
// explosion's" and stay quiet about it.
constexpr uintptr_t CWorld__TriggerExplosion           = 0x004B1140;
constexpr uintptr_t CWorld__TriggerExplosionSectorList = 0x004B1340;

// ---- uprooting: what it is, where it happens, and how it ends -------------
//
// **Uprooting is one bit and one list, and nothing else.** An object goes
// from standing to lying down when `bIsStatic` (CEntity byte A, +0x51 bit 2 -
// offs::ENTITY_IS_STATIC) is cleared and the object is handed to
// CPhysical::AddToMovingList. From that instant CWorld::Process calls its
// ProcessControl every frame and ordinary physics takes it. There is no
// second model, no flag on CObject, and no call that means "fall over".
//
// It is therefore **orthogonal to breaking**, and CObject::ObjectDamage says
// so itself: none of its nine arms clears bIsStatic, and the smash arm
// *sets* it (`[ecx+51h] &= 0FBh / |= 4` at 0x004BB3B0). A lamp post can be
// bent without coming loose and can come loose without being bent, which is
// exactly the pair of screens docs/objects.md 8 described.
//
// Three places in the image decide it, and all three read m_fUprootLimit
// (+0x170, the float CObjectData::SetObjectData copies out of object.dat
// column G at 0x004BC2C8):
//
//   1. **A collision.** CPhysical, two arms:
//        00497559  fld st(1) / fcomp [esi+170h]        impulseA > uprootLimit
//        00497B6B  fld [eax] / fst st(1) / fcomp [esi+170h]
//      each followed by the same eight-model `IsFence` chain (the model ids
//      at 0x005F5ADC..0x005F5AF8) as the `||` arm, and then, at 0x00497E94:
//        mov al,[ebp+51h] / shr al,2 / and al,1        GetIsStatic()
//        mov ecx,ebp / call 004958F0                   AddToMovingList()
//
//   2. **A blast.** CWorld::TriggerExplosionSectorList:
//        004B1473  fcomp [ebp+170h]                    fPower > uprootLimit
//      then AddToMovingList at 0x004B154D and 0x004B163A. The power is the
//      same pure function of the two positions and the radius that the
//      damage is, so an explosion uproots the same objects on every machine
//      for the same reason it breaks them - see the block above.
//
//   3. **A bullet, a shotgun pellet or a bat.** Three arms, identical
//      instruction for instruction, in CWeapon::DoBulletImpact
//      (0x00560481), CWeapon::FireShotgun (0x005616A2) and CWeapon::FireMelee
//      (0x00558A64):
//        mov al,[X+50h] / and al,7 / cmp al,4          ENTITY_TYPE_OBJECT
//        mov al,[X+122h] / shr al,2 / and al,1         bInfiniteMass
//        mov al,[X+51h]  / shr al,2 / and al,1         GetIsStatic()
//        fld [X+170h] / fcomp [const] / fnstsw ax
//        test ah,4 / jne .. / and ah,45h / test ah,41h / je ..
//                                                      m_fUprootLimit <= 0.0f
//        [X+51h] &= 0FBh / call 004958F0               SetIsStatic(false),
//                                                      AddToMovingList()
//        then, only if it is already loose, ApplyMoveForce(normal * -k)
//      The three constants compared against are 0x00603060, 0x00603060 and
//      0x00602C88, and all three read 00000000 - so the test really is
//      `<= 0.0f` and not a threshold. The three force factors are 0x0060311C
//      (C0800000, -4.0f), 0x006030B8 (C0A00000, -5.0f) and 0x00602C98
//      (C0F00000, -7.5f).
//
// **A bullet cannot uproot a lamp post, and object.dat is what says so.**
// The shipped table gives lamppost1/2/3 and doublestreetlght1 an uproot
// limit of 400.0, trafficlight1 500.0, parkingmeter/bin1/postbox1/
// fire_hydrant 100.0, bar_barrier10/12 and the lhouse barriers 350.0,
// parkbench1 5.0, trafficcone 10.0 and smashbar 1000.0. Every one of them is
// strictly greater than zero, so arm 3's gate fails - and because the post is
// still static, the `!GetIsStatic()` that guards the move force fails too.
// Shooting street furniture in retail 1.0 produces eight sparks and a sound
// and moves nothing. The breakable models a bullet *can* knock loose are the
// ones whose uproot limit is 0.0: woodenbox, cardboardbox, cardboardbox2,
// cardboardbox4, wastebin, dump1, palette, parktable1, papermachn01 and the
// two fishstalls.
//
// **The engine decides when it has stopped.** CPhysical::ProcessControl
// (0x00495F10, called by CObject::ProcessControl at 0x004BB05C) counts quiet
// frames and puts the object back to sleep itself:
//
//   004960D7  inc byte [ebx+0EDh]                      m_nStaticFrames++
//   004960DD  cmp byte [ebx+0EDh],0Ah / jbe            > 10
//   004960F1  [ebx+51h] &= 0FBh / |= 4                 SetIsStatic(true)
//   004960FB  m_vecMoveSpeed, m_vecTurnSpeed and both frictions zeroed
//   00496172  m_nStaticFrames = 0                      (the else arm)
//
// and CWorld::Process then unlinks it, in its own moving-entity loop:
//
//   004B1B99  call [edi+20h]                           ProcessControl
//   004B1B9C  [ebp+51h] >> 2 & 1                       GetIsStatic()
//   004B1BA8  call 00495940                            RemoveFromMovingList
//
// (again at 0x004B1BF3 for the postponed pass.) That is the reason nothing in
// CoopIII ever has to touch the moving list to put an object back: setting
// bIsStatic is enough, and the engine does the unlink on its own next pass.
// Writing into that list is what client/src/game/movinglist.h exists to clean
// up after, and this feature deliberately never does it.
//
// **m_pDamageEntity is good for exactly one frame, and that is the frame
// ObjectDamage runs in.** The same CPhysical::ProcessControl clears both
// halves of the collision record at its top:
//
//   00495F78  mov dword [ebx+10Ch],0                   m_fDamageImpulse = 0
//   00495F82  mov dword [ebx+110h],0                   m_pDamageEntity = nil
//
// and CObject::ProcessControl calls ObjectDamage with m_fDamageImpulse
// immediately *before* that (0x004BB04F/0x004BB055). So the pointer a break
// detour reads is this frame's, not an arbitrarily old one - which settles
// docs/objects.md 9's second open question in the safe direction. It also
// means a bullet leaves it nil, because a bullet never writes it.
constexpr size_t STATIC_FRAMES = 0x0ED;   // uint8, CPhysical::m_nStaticFrames
constexpr uint8_t STATIC_FRAMES_ASLEEP = 10;

// eEntityType's fourth value, witnessed by all three weapon object arms
// (`and al,7 / cmp al,4`) and by CWorld::Add's `cmp al,1 / cmp al,5` pair of
// exclusions for buildings and dummies. ENTITY_TYPE_VEHICLE and
// ENTITY_TYPE_PED are already in this file; this is the one that was never
// needed until something had to recognise a CObject by its type byte.
constexpr uint8_t ENTITY_TYPE_OBJECT = 4;

// __thiscall void CPhysical::AddToMovingList(void) is CPhysical__AddToMovingList
// above (0x004958F0). Recorded here as well because it is the one door every
// uproot in the image goes through - eleven call sites, and the ones that can
// reach a breakable map object are the CPhysical collision arm (0x00497EB1),
// the two explosion arms (0x004B154D, 0x004B163A) and the three weapon arms
// (0x00558AAF, 0x005604BD, 0x005616ED). The other five cannot:
//
//   0044D784   a script opcode, so a MISSION_OBJECT
//   004AE9C0   CWorld::Add, for any entity added already non-static
//   004F458B   CPopulation::ConvertToRealObject's buoy arm - it is guarded by
//              `model == [005F5B5Ch]` and the buoy has no damage effect
//   0053B585   a garage/crusher shove
//   00564B6D   a weapon nudge with no uproot-limit test at all
//
// The three weapon arms and the four *other* call sites of ObjectDamage are
// the whole reason a break and an uproot need separate treatment.
constexpr uintptr_t CWeapon__DoBulletImpactObjectArm = 0x00560481;   // for the record
constexpr uintptr_t CWeapon__FireShotgunObjectArm    = 0x005616A2;
// Misnamed: the arm is in CBulletInfo::Update, the sniper round, and FireMelee
// has none ("fists and the bat" above). Kept under this name for the record.
constexpr uintptr_t CWeapon__FireMeleeObjectArm      = 0x00558A64;

} // namespace object


// ---- cheats ---------------------------------------------------------------
//
// Verified 2026-09-23 against the retail image. docs/cheats.md is the design
// and the per-cheat argument; this is the transcription it rests on.
//
// **There is one door, and it is the keyboard.** CPad::DoCheats(int16), the
// pad-button path, is `sub esp,8 / mov [esp+4],ecx / add esp,8 / ret 4` at
// 0x00492F20 - an empty stub on PC, reached from CGame::Process at 0x0048C8E6
// through CPad::DoCheats() (0x00492F00). So every cheat in this build comes
// through CPad::AddToPCCheatString, and a byte scan finds exactly one call to
// that: 0x005841C7, the default arm of the key-down handler (0x00583F10),
// which is re3 events.cpp:314 - `if (c < 255) { VK_KEYS[c] = 255;
// AddToPCCheatString(c); }`. It runs off the window procedure, on the thread
// that also runs CGame::Process.
//
// __thiscall void CPad::AddToPCCheatString(char c).  ret 4.
//
//   0x00492450  sub esp,8 / mov [esp+4],ecx
//   0x00492457  mov edx,12h                          i = 18
//   0x00492460  mov al,[edx+885B90h]
//   0x00492466  mov [edx+885B91h],al / dec edx / jge   buf[i+1] = buf[i]
//   0x00492481  mov [00885B90h],al                   buf[0] = c
//   then, 23 times:
//               push <len> / push 885B90h / push <reversed string>
//               call 0x005A0A10 (strncmp) / add esp,0Ch
//               test eax,eax / jne <next> / call <handler>
//   0x00492718  ret 4
//
// Twenty-three independent `if`s rather than an else-if chain, re3
// Pad.cpp:896-1027's shape, so one keystroke could in principle fire two. No
// retail string is a suffix of another, which is what stops it happening;
// tools/clienttest types every one of them and checks.
//
// The buffer is newest-first, which is why every string in the table is
// stored backwards. A byte scan finds 0x00885B90 used by this function and by
// nothing else in the image, so CoopIII keeping it up to date itself (game/
// cheats.cpp does, in a session) cannot confuse any other reader of it.
constexpr uintptr_t CPad__AddToPCCheatString  = 0x00492450;
constexpr uintptr_t CPad__KeyBoardCheatString = 0x00885B90;   // char[20]
constexpr size_t    KEYBOARD_CHEAT_STRING_LEN = 20;
constexpr uintptr_t CPad__DoCheatsPad         = 0x00492F20;   // recorded: empty

// Where the rows begin and end, and the strncmp every row calls. The first
// row is the one with the `mov [00885B90h],al` folded into it, between its
// last push and its call; the last one's `jne` lands on the epilogue.
constexpr uintptr_t CPad__CheatRowsBegin = 0x00492475;   // `push 0Ch`, row 0
constexpr uintptr_t CPad__CheatRowsEnd   = 0x00492715;   // `add esp,8 / ret 4`
constexpr uintptr_t crt_strncmp          = 0x005A0A10;
// The function's first seven bytes, `sub esp,8 / mov [esp+4],ecx`. If they
// are anything else when CoopIII goes to hook it, another mod got there
// first, and the cheats are left to it.
constexpr uint8_t CPAD_ADD_TO_PC_CHEAT_STRING_PROLOGUE[] = {
    0x83, 0xEC, 0x08, 0x89, 0x4C, 0x24, 0x04};

// One row per `if`, in the order the function tests them, which is CheatId's
// order (protocol.h). `string` is the reversed literal it passes to strncmp,
// read out of .rdata, and `length` is the `push` in front of it. `handler` is
// the call inside the `if`; a byte scan finds each one called from that one
// place and nowhere else, so detouring the dispatch, not the handlers, is
// enough to see every cheat. tools/clienttest decodes all 23 rows out of the
// function's bytes and compares them with this table when it is handed the
// exe (COOPIII_GTA3_EXE), which is how the row below was caught.
//
// **`length` is strlen of the literal in every row but one.** BOOOOORING's
// is `push 10h` (0x004925D6) for a ten-letter string, so strncmp goes on past
// the string's own NUL and compares it with KeyBoardCheatString[10] - which
// only matches while that byte is still the zero the buffer starts with in
// .bss. Nothing else writes the buffer. So in 1.0 the slow-motion cheat works
// only as the very first thing typed after the game starts, and never again
// once eleven keys have been pressed. CoopIII compares the same way, so a
// session keeps it exactly as broken as single player has it.
struct CheatSite {
	const char *reversed;
	uintptr_t   string;
	uint8_t     length;
	uintptr_t   handler;
};

constexpr CheatSite CHEAT_SITES[] = {
    {"SNUGSNUGSNUG",     0x005F6548, 0x0C, 0x00490D90},   // WeaponCheat
    {"NAMHCIRAEREWIFI",  0x005F6558, 0x0F, 0x00491430},   // MoneyCheat
    {"TIEHDNUSEG",       0x005F6568, 0x0A, 0x00490E70},   // HealthCheat
    {"ESAELPECILOPEROM", 0x005F6574, 0x10, 0x00491490},   // WantedLevelUpCheat
    {"ESAELPECILOPON",   0x005F6588, 0x0E, 0x004914F0},   // WantedLevelDownCheat
    {"KNATASUEVIG",      0x005F6598, 0x0B, 0x00490EE0},   // TankCheat
    {"GNABGNABGNAB",     0x005F65A4, 0x0C, 0x00491040},   // BlowUpCarsCheat
    {"PUGNISSERDEKILI",  0x005F65B4, 0x0F, 0x004910B0},   // ChangePlayerCheat
    {"DAAAMGNIOGLLASTI", 0x005F65C4, 0x10, 0x004911C0},   // MayhemCheat
    {"EMSEKILYDOBON",    0x005F65D8, 0x0D, 0x00491270},   // EverybodyAttacksPlayerCheat
    {"LLAROFSNOPAEW",    0x005F65E8, 0x0D, 0x00491370},   // WeaponsForAllCheat
    {"UOYNEHWSEILFEMIT", 0x005F65F8, 0x10, 0x004913A0},   // FastTimeCheat
    {"GNIROOOOOB",       0x005F660C, 0x10, 0x004913F0},   // SlowTimeCheat - 16, see above
    {"ESIOTRUT",         0x005F6618, 0x08, 0x00491460},   // ArmourCheat
    {"EMROFRECNACNIKS",  0x005F6624, 0x0F, 0x00491520},   // SunnyWeatherCheat
    {"DNALTOCSEKILI",    0x005F6634, 0x0D, 0x00491550},   // CloudyWeatherCheat
    {"DNALTOCSEVOLI",    0x005F6644, 0x0D, 0x00491580},   // RainyWeatherCheat
    {"PUOSAEP",          0x005F6654, 0x07, 0x004915B0},   // FoggyWeatherCheat
    {"REHTAEWDAM",       0x005F665C, 0x0A, 0x004915E0},   // FastWeatherCheat
    {"SLEEHWFOTESECINA", 0x005F6668, 0x10, 0x00491610},   // OnlyRenderWheelsCheat
    {"BBYTTIHCYTTIHC",   0x005F667C, 0x0E, 0x00491640},   // ChittyChittyBangBangCheat
    {"DAMEKILSRENROC",   0x005F668C, 0x0E, 0x00491670},   // StrongGripCheat
    {"TAEHCSBMILYTSAN",  0x005F669C, 0x0F, 0x004916A0},   // NastyLimbsCheat
};
static_assert(sizeof(CHEAT_SITES) / sizeof(CHEAT_SITES[0]) == CHEAT_COUNT,
              "one row per CheatId, in the order AddToPCCheatString tests them");

// What each handler writes, read off its body. Every one but the last opens
// on CHud::SetHelpMessage(TheText.Get(key), true) - `mov ecx,941520h /
// push 1 / push <key> / call 0x0052C5A0 / push eax / call 0x005051E0` - which
// is the "Cheat activated" line and nothing else. The rest:
//
//   0x00490D90  GiveWeapon (0x004CF9B0) on FindPlayerPed eleven times:
//               bat 0, colt 100, uzi 100, shotgun 20, AK 200, M16 200,
//               sniper 5, rocket 5, molotov 5, grenade 5, flamethrower 200
//   0x00491430  add [PlayerInFocus * 13Ch + 0094139Ch], 3D090h   money +250000
//   0x00490E70  [FindPlayerPed + 2C0h] = 100.0f; then, if FindPlayerVehicle
//               (0x004A10C0): [veh + 200h] = 1000.0f, and for m_vehType 0
//               CDamageManager::SetEngineStatus(0) on [veh + 288h] - which is
//               `mov [ecx+4],al` clamped to 250, i.e. DMG_ENGINE_STATUS
//   0x00491490  CPlayerPed::SetWantedLevel (0x004F3190) with
//               min([[ped + 53Ch] + 18h] + 2, 6)          two stars, max six
//   0x004914F0  CPlayerPed::SetWantedLevel(0)
//   0x00490EE0  streams model 7Ah (Rhino), finds the car path node nearest the
//               player within 100 (ThePaths.FindNodeClosestToCoors,
//               0x0042CC30), `new CAutomobile(7Ah, 2)` - MISSION_VEHICLE, the
//               bug re3 fixes under FIX_BUGS - at node + 4.0 z, heading
//               3.490659 (200 degrees), `and al,7 / or al,20h` STATUS_ABANDONED,
//               m_nDoorLock [+224h] = 1 (unlocked), CWorld::Add. No bIsLocked,
//               no ClearSpaceForMissionEntity.
//   0x00491040  walks the vehicle pool from the top slot down and calls
//               `call [edi+74h]`, vtable slot 29, BlowUpCar(nil), on every
//               slot whose flag byte is not free
//   0x004910B0  random model 0..82 (GetRandomNumber * 1/32768 * 83), skipping
//               unloaded ones, 1Ah-1Dh (the four SPECIALs) and 8 (MI_TAXI_D),
//               then DeleteRwObject / RequestModel / SetModelIndex, keeping
//               m_animGroup. Only when IsPedInControl.
//   0x004911C0  CPedType::ms_apPedType[4..20]->m_threats = 0FFFFFh
//   0x00491270  CPedType::ms_apPedType[4..20]->m_threats |= 1   (PLAYER1)
//   0x00491370  toggles CPopulation::ms_bGivePedsWeapons
//   0x004913A0  ms_fTimeScale *= 2.0 while < 4.0
//   0x004913F0  ms_fTimeScale *= 0.5 while > 0.25
//   0x00491460  [FindPlayerPed + 2C4h] = 100.0f                  armour
//   0x00491520..0x004915B0  CWeather::ForceWeatherNow(0, 1, 2, 3)
//   0x004915E0  toggles gbFastTime
//   0x00491610  toggles CVehicle::bWheelsOnlyCheat
//   0x00491640  toggles CVehicle::bAllDodosCheat
//   0x00491670  toggles CVehicle::bCheat3
//   0x004916A0  toggles CPed::bNastyLimbsCheat - and no help message
//
// The four float constants the time handlers compare against were read out
// of the file: 4.0 at 0x005F64CC, 2.0 at 0x005F64E8, 0.25 at 0x005F64EC and
// 0.5 at 0x005F64F0.
constexpr uintptr_t CPopulation__ms_bGivePedsWeapons = 0x0095CCF6;   // bool
constexpr uintptr_t gbFastTime                       = 0x0095CDBB;   // bool
constexpr uintptr_t CVehicle__bWheelsOnlyCheat       = 0x0095CD78;   // bool
constexpr uintptr_t CVehicle__bAllDodosCheat         = 0x0095CD75;   // bool
constexpr uintptr_t CVehicle__bCheat3                = 0x0095CD66;   // bool
constexpr uintptr_t CPed__bNastyLimbsCheat           = 0x0095CD44;   // bool

// Who reads the toggles, from a byte scan of the image for each address:
//
//   gbFastTime             CClock::Update, 0x004734C5 and 0x004734DF - the
//                          minute ticks every frame instead of every
//                          ms_nMillisecondsPerGameMinute
//   ms_bGivePedsWeapons    CPopulation::AddPed, 0x004F532B - a new pedestrian
//                          only, and only on the machine whose generator
//                          made it
//   bWheelsOnlyCheat       thirteen reads in CAutomobile / CBoat rendering
//   bAllDodosCheat         0x005341DE (ProcessControl's flight arm) and
//                          0x00589AFF (a script opcode's cheat test)
//   bCheat3                eight reads in CAutomobile::ProcessControl, and
//                          0x00589B08 beside the dodo one
//   bNastyLimbsCheat       **nothing.** Written by its handler and by
//                          ResetCheats, read by no instruction in the image.
//                          NASTYLIMBSCHEAT does nothing in 1.0.

// CPad::ResetCheats, called on a new game and a load (0x00582F74,
// 0x00590AE9). The listing runs the padding before it into its first
// instruction; decoded from 0x00494450 it is `call 0x005231A0`
// (CWeather::ReleaseWeather), then zeroes every toggle above and writes
// 3F800000h - 1.0 - into ms_fTimeScale. It does NOT restore the CPedType
// threat table, so ITSALLGOINGMAAAD and NOBODYLIKESME outlive a load in
// single player too.
constexpr uintptr_t CPad__ResetCheats = 0x00494450;   // recorded, not called

// CPedType's table: 0x00941594 is `ms_apPedType`, one CPedType* per ePedType.
// Pinned by the two threat handlers above (`mov edx,[eax*4+00941594h] / mov
// dword [edx+18h],0FFFFFh`, eax running 4..20) and by CPed's constructor
// below. `[esi]` off an entry is m_flag, the type's own PED_FLAG bit - it is
// what ScanForThreats ANDs a neighbour against.
constexpr uintptr_t CPedType__ms_apPedType = 0x00941594;
namespace offs {
constexpr size_t PEDTYPE_FLAG    = 0x00;   // uint32, this type's PED_FLAG bit
constexpr size_t PEDTYPE_THREATS = 0x18;   // uint32
} // namespace offs
constexpr int PEDTYPE_CHEAT_FIRST = 4;    // PEDTYPE_CIVMALE
constexpr int PEDTYPE_CHEAT_LAST  = 20;   // PEDTYPE_PROSTITUTE; SPECIAL (21) is spared

// **Why a riot reaches replicas.** Nothing reads the table when a pedestrian
// decides; it reads its own copy. CPed's constructor (0x004C41C0) takes it
// once, at 0x004C4CD4:
//
//   mov eax,[ecx+32Ch]            m_nPedType
//   mov edx,[eax*4+00941594h]     ms_apPedType[type]
//   mov eax,[edx+18h]             ->m_threats
//   mov [ebx+188h],eax            m_fearFlags
//
// and CPed::ScanForThreats (0x004C5FE0, __thiscall, no arguments, plain
// `ret`, uint32 in eax) opens on `mov eax,[ebx+188h] / mov [esp+8],eax` and
// tests everything against that. So a threat cheat changes nobody who
// already exists and everybody constructed afterwards - CoopIII's replicas
// and remote players included, since every one of them is built through the
// same CCivilianPed constructor.
//
// And a replica does act on it. CCivilianPed::CivilianAI (0x004C07A0, called
// once, from CCivilianPed::ProcessControl at 0x004C06D2) opens:
//
//   0x004C07AF  cmp esi,[ebx+340h] / jbe out       now <= m_fleeTimer
//   0x004C07BD  cmp dword [ebx+164h],0 / je 004C07D6    objective NONE: go on
//   0x004C07C6  test bRespondsToThreats ([ebx+156h] bit 1) / je out
//   0x004C07D8  call IsPedInControl / je out
//   0x004C07E7  call ScanForThreats                 the full reaction
//
// A replica holds m_objective at NONE (docs/protocol.md 1.13.3), so the
// bRespondsToThreats test that looks like its off switch is skipped, and the
// reaction runs: flee, or SetObjective(KILL_CHAR_ON_FOOT) at whatever it
// found. CPed::RegisterThreatWithGangPeds ORs an attacker's flag into its
// neighbours' m_fearFlags as well, so this was reachable without a cheat;
// the riot cheats just make it certain. game/cheats.cpp detours
// ScanForThreats and answers "nothing" for a ped CoopIII built.
//
// ScanForThreats' five callers, each resolved by a byte scan: CivilianAI
// twice (0x004C07E7, 0x004C1002), 0x004C30C5, and CPed::ProcessObjective
// (0x004D94E0) at 0x004DA45B plus one more at 0x004D9236 - the last two only
// with an objective set, which a replica never has.
constexpr uintptr_t CPed__ScanForThreats       = 0x004C5FE0;
constexpr uintptr_t CCivilianPed__CivilianAI   = 0x004C07A0;   // recorded
namespace offs {
constexpr size_t PED_FEAR_FLAGS = 0x188;   // uint32 m_fearFlags
constexpr size_t PED_FLEE_TIMER = 0x340;   // uint32, recorded
} // namespace offs

// **Every class in the vehicle pool, and what slot 29 is for it.** BANGBANGBANG
// calls BlowUpCar through the vtable on everything in the pool, so the
// question for co-op is whether that can reach a body the two BlowUpCar
// detours in game/vehicle.cpp do not cover. A byte scan of the image for the
// three possible slot-29 targets finds six vtables:
//
//   0x00600C1C  CAutomobile   0x00600C90 -> 0x0053BC60  detoured
//   0x00600EA4  CBoat         0x00600F18 -> 0x00541CB0  detoured
//   0x00601EB0  CHeli         0x00601F24 -> 0x00444B10  empty
//   0x006021DC  CPlane        0x00602250 -> 0x00444B10  empty - stamped by
//                             the ctor at 0x0054B18D, InitPlanes' `push 8Ch`
//   0x0060241C  CTrain        0x00602490 -> 0x00444B10  empty
//   0x006028A8  CVehicle      0x0060291C -> 0x00444B10  empty
//
// and nothing else holds any of the three. "Empty" is the whole body:
// `sub esp,8 / mov [esp+4],ecx / add esp,8 / ret 4`, thirteen bytes that
// store `this` in a local and throw it away. So the cheat can only ever reach
// the two detoured bodies or the empty base, and the detours decide on the
// car, not on the caller - vehicle.cpp's refusals hold for it unchanged. The
// police helicopter, the planes and the trains are untouched by it in single
// player too.
constexpr uintptr_t CPlane__vtable         = 0x006021DC;
constexpr uintptr_t CVehicle__BlowUpCarBase = 0x00444B10;   // empty, see above

// The vehicle pool's size. CPools::Initialise (0x004A1770) pushes the size
// before each store: `push 6Eh` at 0x004A17D5, then `mov [009430DCh],eax` at
// 0x004A17DE. 110, re3's NUMVEHICLES. It is the most cars one frame can
// wreck, which is what the unowned-wreck queue has to hold.
constexpr int32_t VEHICLE_POOL_SIZE = 110;

// The tank's other half. CAutomobile::ProcessControl's model switch sends a
// Rhino (`cmp eax,7Ah` at 0x00532001) to TankControl (0x0053D530) and then
// BlowUpCarsInPath (0x0053E000) whatever its status, so an observer's
// replica runs both:
//
//   TankControl        `call FindPlayerVehicle / cmp ebx,eax / jne out` at
//                      0x0053D5E5 - fires only for the car the local player
//                      is in, i.e. on the driver's machine. Its shell is
//                      AddExplosion(nil, FindPlayerPed(), 8, ...) at
//                      0x0053DA3C, which game/combat.cpp relays like any
//                      explosion the local player caused.
//   BlowUpCarsInPath   over m_aCollisionRecords, `call [esi+74h]` with the
//                      tank as culprit at 0x0053E06E - slot 29 again, so
//                      again the detour. A replica crushing a car somebody
//                      else owns is refused like any other observer.
constexpr uintptr_t CAutomobile__TankControl      = 0x0053D530;   // recorded
constexpr uintptr_t CAutomobile__BlowUpCarsInPath = 0x0053E000;   // recorded


// ---- helpers --------------------------------------------------------------

template <class T>
inline T *Ptr(uintptr_t address) {
	return reinterpret_cast<T *>(address);
}

template <class T>
inline T &Global(uintptr_t address) {
	return *reinterpret_cast<T *>(address);
}

template <class Fn>
inline Fn Func(uintptr_t address) {
	return reinterpret_cast<Fn>(address);
}

// A member of a game object, by the offsets above. Kept tiny on purpose and
// named after the re3 members at the call sites, so any one use can be
// checked against this file without following pointer arithmetic.
template <class T>
inline T &Field(void *object, size_t offset) {
	return *reinterpret_cast<T *>(reinterpret_cast<uint8_t *>(object) + offset);
}

} // namespace coopiii::game
