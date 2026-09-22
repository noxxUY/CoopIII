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
// agentpad/ uses this as its readiness signal, and it's worth knowing why
// instead of just copying it: it's the only "the game is up" indicator that
// doesn't go through CTimer, and CTimer is disputed (see the long note in
// game/frame.cpp). WinMain sets gGameState itself, so reaching GS_FRONTEND
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
// agentpad/ writes into these structures every frame and a wrong offset here
// means a write into a neighbouring global.
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
constexpr size_t VEH_EXTRAS             = 0x19E;   // int8[2]
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
//        ... the CBoat branch, `push 484h` then the same shape ...
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
constexpr uintptr_t CBoat__ctor       = 0x0053E3E0;   // same signature
constexpr uintptr_t CVehicle__ctor    = 0x00550A60;   // (uint8 createdBy)

// CVehicle::SetModelIndex(uint32). Calls CEntity::SetModelIndex, copies
// CVehicleModelInfo::ms_compsUsed into m_aExtras, then sets
// m_nNumMaxPassengers from the model's door count, matching re3 Vehicle.cpp
// exactly. The subclass constructors call it, so a spawn doesn't need it
// directly - it's here as the fifth independent confirmation of +0x1CC.
constexpr uintptr_t CVehicle__SetModelIndex = 0x00551170;

constexpr uintptr_t CVehicle__CanBeDeleted = 0x005511B0;

// __thiscall CEntity::GetDistanceFromCentreOfMassToBaseOfModel() -> float.
// CREATE_CAR adds it to the spawn z so the car sits on its wheels rather than
// with its centre of mass on the road surface.
constexpr uintptr_t CVehicle__GetDistanceFromCentreOfMassToBaseOfModel = 0x004755C0;

// CCarCtrl::JoinCarWithRoadSystem(CVehicle*). Opens by zeroing
// AutoPilot.m_nNextRouteNode (+0x130) and m_nCurrentRouteNode (+0x12C), which
// is both re3's first statement and another confirmation of VEH_AUTOPILOT.
constexpr uintptr_t CCarCtrl__JoinCarWithRoadSystem = 0x0041F820;

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
// room for twelve. What follows it is the function's own saved state:
//
//   [esp+40h] saved ebx   <- nodes[12]
//   [esp+44h] saved esi   <- nodes[13]
//   [esp+48h] saved edi   <- nodes[14]
//   [esp+4Ch] saved ebp   <- nodes[15]
//   [esp+50h] return address                      <- nodes[16]
//   [esp+54h] the clump argument                  <- nodes[17]
//   [esp+58h] timeDelta                           <- nodes[18]
//
// A thirteenth animation corrupts the caller's registers, a seventeenth the
// return address, and an eighteenth the clump. The last of those is what a
// live crash dump caught: ESI was 0x11, the terminator wrote null over the
// clump argument, and two hundred bytes later the function's own tail read it
// back and faulted at 0x004025D2 on `mov eax,[eax+4]`, address 4.
//
// Worth knowing that saved edi and ebp are in that window, because
// CWorld::Process's walk over the moving list keeps the node cursor in edi
// and the entity in ebp across this very call. Fifteen animations on one
// clump and the list walk resumes on registers this function restored out of
// the overflow.
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
constexpr uintptr_t CPed__SetDie = 0x004D37D0;

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
// target already has a fire (`cmp [ebx+4B4h],0`), when a ped is not
// IsPedInControl (call 0x004CE6C0), or when a car's engine is already past
// 225. Six callers: ProcessFire's own spread rule, CWorld::SetCarsOnFire and
// its ped equivalent, CExplosion, and CShotInfo::Update - which is the
// flamethrower.
constexpr uintptr_t CFireManager__StartFireEntity = 0x00479590;

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
// The one path that does *not* check bFireProof is CShotInfo::Update
// (0x0055C1A8 gates on IsPedInControl and a distance and nothing else), so a
// replayed flamethrower can still light a remote ped locally, with its own
// SetFlee before the call. ped.cpp treats that as a fire that is not ours
// and puts it out; see PlanRemoteFire.
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

// Recorded because the next person to want a ray will find it first and it is
// the wrong one for a yes-or-no question. 11 arguments, `add esp,2Ch` at all
// 45 of its call sites, and it contains the same inlined AdvanceCurrentScanCode
// at 0x004B0DE6.
constexpr uintptr_t CWorld__ProcessLineOfSight = 0x004B0DE0;

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
