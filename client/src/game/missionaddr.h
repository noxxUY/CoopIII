// The script engine, as far as the session's mission needs it (game/mission.h).
//
// Every address, offset and type byte here was read out of the retail 1.0
// gta3.exe on 2026-09-24 and lives in addresses.h now, with the instruction
// that proves it; this file only gives them the short names mission.cpp uses.
// They were III.CLEO's and plugin-sdk's before that, and all of them held.
//
// The opcode numbers below are re3's ScriptCommands.h, and each one's handler
// in the exe takes the operands mission.cpp and replay.h give it. Two of them
// were wrong and are fixed: IS_CAR_IN_MISSION_GARAGE is 021C (03D4 is the
// import garage's question, with two operands), and OVERRIDE_NEXT_RESTART is
// 016E (016C adds a hospital restart point). tools/clienttest/missions.cpp
// checks the lot against the exe when it has one.
//
// Nothing here is touched unless CoopIII.ini says `missions = on`.
#pragma once

#include "addresses.h"

#include <cstddef>
#include <cstdint>

namespace coopiii::game::scripts {

// ---- CTheScripts ---------------------------------------------------------------

// The script memory: main.scm's first 128 KB, then the mission slot's 32 KB.
constexpr uintptr_t SCRIPT_SPACE      = CTheScripts__ScriptSpace;
constexpr uint32_t  SCRIPT_SPACE_SIZE = SIZE_SCRIPT_SPACE;
constexpr uint32_t  MAIN_SCRIPT_SIZE  = SIZE_MAIN_SCRIPT;

// The head of the running scripts' list; a new script goes in at the head.
constexpr uintptr_t ACTIVE_SCRIPTS = CTheScripts__pActiveScripts;

// uint32: where $ONMISSION lives in SCRIPT_SPACE, as main.scm's own
// DECLARE_MISSION_FLAG (0180) set it. mission.cpp still checks it against the
// 0180 in the script before trusting it: a main.scm that is not the stock one
// may never declare it.
constexpr uintptr_t ON_A_MISSION_FLAG = CTheScripts__OnAMissionFlag;

// bool: a mission is loaded in the mission slot. START_MISSION sets it and the
// mission's TERMINATE_THIS_SCRIPT clears it, just after its
// MISSION_HAS_FINISHED.
constexpr uintptr_t ALREADY_RUNNING_A_MISSION = CTheScripts__bAlreadyRunningAMissionScript;

// ---- the range handlers ------------------------------------------------------------
//
// char __thiscall CRunningScript::ProcessCommandsNToM(int32 command), `ret 4`.
// The dispatcher reads the opcode word, strips the NOT bit and calls one of
// these. III.CLEO keeps the same addresses in its own table and calls them
// itself, so a detour on one of them is reached with or without CLEO.
constexpr uintptr_t RANGE_0      = CRunningScript__ProcessCommands0To99;
constexpr uintptr_t RANGE_100    = CRunningScript__ProcessCommands100To199;
constexpr uintptr_t RANGE_200    = CRunningScript__ProcessCommands200To299;
constexpr uintptr_t RANGE_300    = CRunningScript__ProcessCommands300To399;
constexpr uintptr_t RANGE_400    = CRunningScript__ProcessCommands400To499;
constexpr uintptr_t RANGE_500    = CRunningScript__ProcessCommands500To599;
constexpr uintptr_t RANGE_600    = CRunningScript__ProcessCommands600To699;
constexpr uintptr_t RANGE_700    = CRunningScript__ProcessCommands700To799;
constexpr uintptr_t RANGE_800    = CRunningScript__ProcessCommands800To899;
constexpr uintptr_t RANGE_900    = CRunningScript__ProcessCommands900To999;
constexpr uintptr_t RANGE_1000   = CRunningScript__ProcessCommands1000To1099;
// The dispatcher's last link, 1100..1154: `cmp dx,4B0h / jge` past it, call
// at 0x0043963D. Not III.CLEO's: CLEO's own opcodes start at 0x0A8C.
constexpr uintptr_t RANGE_1100   = CRunningScript__ProcessCommands1100To1199;

// Every range handler is still checked, before it is hooked, to be one of the
// targets the dispatcher calls: its body is `cmp dx,<limit> / jge / movsx
// eax,dx / push eax / call <range>` twelve times over, all of it in its first
// 0x143 bytes. CLEO's jump over the dispatcher's first bytes leaves the calls
// where they are. That catches another plugin having moved a handler.
constexpr uint32_t DISPATCHER_BYTES = 0x200;

// ---- CRunningScript ---------------------------------------------------------------
//
// The engine's part is 0x88 bytes. CLEO's own scripts are longer (0xB0) and
// start the same.
namespace layout {
using offs::SCRIPT_NEXT;
using offs::SCRIPT_NAME;
using offs::SCRIPT_IP;
using offs::SCRIPT_STACK;
using offs::SCRIPT_SP;
using offs::SCRIPT_LOCALS;
using offs::SCRIPT_COND_RESULT;
using offs::SCRIPT_IS_MISSION;
// re3's m_bIsMissionScript: the death-arrest check (Process, 0x00439443) and
// MISSION_HAS_FINISHED's cleanup (0x0043D5F2) run for it. NOT the mission
// slot's script. START_MISSION's handler sets it (0x00588E56), and so does
// LAUNCH_MISSION's, 00D7 (range 200's table entry 1, 0x0043D5DF), on a plain
// main-script thread: main.scm's INIT_THREADS starts HJ, USJ, GENSTUF,
// RAMPAGE, IMPORT, CAMERA and GATES that way, and none of them ever ends.
constexpr size_t SCRIPT_MISSION_RULES = offs::SCRIPT_IS_MISSION;
using offs::SCRIPT_WAKE_TIME;
using offs::SCRIPT_AND_OR;
using offs::SCRIPT_NOT;
using offs::SCRIPT_DEATHARREST_ARMED;
using offs::SCRIPT_DEATHARREST_DONE;
// bool, re3's m_bMissionFlag: the script in the mission slot, the one
// START_MISSION launched, and nothing else. Only that handler sets it
// (0x00588E5A, beside ALREADY_RUNNING_A_MISSION); Init clears it (0x00438782);
// TERMINATE_THIS_SCRIPT reads it to free the slot (0x0043A533).
constexpr size_t SCRIPT_MISSION_SLOT = 0x85;
} // namespace layout

using game::SCRIPT_STACK_DEPTH;

// What an `if` (00D6) leaves in SCRIPT_AND_OR: 0 for a single condition, 1..8
// counting down through an `if and`, 21..28 through an `if or`.
constexpr uint16_t ANDOR_NONE  = SCRIPT_ANDOR_NONE;
constexpr uint16_t ANDOR_ORS_1 = SCRIPT_ANDOR_ORS_1;
constexpr uint16_t ANDOR_ORS_8 = SCRIPT_ANDOR_ORS_8;

// The type byte a SCRIPT_SPACE operand starts with. A float literal is 16
// bits of 1/16ths in GTA III.
constexpr uint8_t PARAM_INT32  = SCRIPT_PARAM_INT32;
constexpr uint8_t PARAM_GLOBAL = SCRIPT_PARAM_GLOBAL;
constexpr uint8_t PARAM_LOCAL  = SCRIPT_PARAM_LOCAL;
constexpr uint8_t PARAM_INT8   = SCRIPT_PARAM_INT8;
constexpr uint8_t PARAM_INT16  = SCRIPT_PARAM_INT16;
constexpr uint8_t PARAM_FLOAT  = SCRIPT_PARAM_FLOAT;

// ---- the opcodes (re3 ScriptCommands.h; plugin-sdk's names) -----------------------
namespace op {
constexpr int32_t IS_PLAYER_IN_AREA_2D                   = 0x0056;
constexpr int32_t IS_PLAYER_IN_AREA_3D                   = 0x0057;
constexpr int32_t MISSION_HAS_FINISHED                   = 0x00D8;
constexpr int32_t LOCATE_PLAYER_ANY_MEANS_2D             = 0x00E3;   // .. 0x00E8, six of them
constexpr int32_t LOCATE_STOPPED_PLAYER_IN_CAR_2D        = 0x00E8;
constexpr int32_t LOCATE_PLAYER_ANY_MEANS_3D             = 0x00F5;   // .. 0x00FA
constexpr int32_t LOCATE_STOPPED_PLAYER_IN_CAR_3D        = 0x00FA;
constexpr int32_t REMOVE_BLIP                            = 0x0164;
constexpr int32_t ADD_BLIP_FOR_COORD_OLD                 = 0x0167;
constexpr int32_t ADD_BLIP_FOR_COORD                     = 0x018A;
constexpr int32_t IS_PLAYER_IN_AREA_ON_FOOT_2D           = 0x0197;   // .. 0x019B, 2D
constexpr int32_t IS_PLAYER_STOPPED_IN_AREA_IN_CAR_2D    = 0x019B;
constexpr int32_t IS_PLAYER_IN_AREA_ON_FOOT_3D           = 0x019C;   // .. 0x01A0, 3D
constexpr int32_t IS_PLAYER_STOPPED_IN_AREA_IN_CAR_3D    = 0x01A0;
constexpr int32_t ADD_SPRITE_BLIP_FOR_COORD              = 0x02A8;
constexpr int32_t REGISTER_MISSION_PASSED                = 0x0318;
constexpr int32_t CAN_PLAYER_START_MISSION               = 0x03EE;
constexpr int32_t LOAD_AND_LAUNCH_MISSION_INTERNAL       = 0x0417;
// The 1100 range's (table 0x00610AB4, base 1100): the island loaded behind
// its loading screen (0x00589D2D, one operand into CGame::currLevel),
// Lips' car made to stand up to more (0x00589EE3, the car and a flag into
// +0x4DA bit 1), Bait's cartel car sent back after the player (0x00589F42,
// the car) and the end of the game's music loaded (0x00589FA5, none).
constexpr int32_t LOAD_COLLISION_WITH_SCREEN             = 0x044C;
constexpr int32_t MAKE_CRAIGS_CAR_A_BIT_STRONGER         = 0x044F;
constexpr int32_t SET_JAMES_CAR_ON_PATH_TO_PLAYER        = 0x0450;
constexpr int32_t LOAD_END_OF_GAME_TUNE                  = 0x0451;
// The HUD's widgets.
constexpr int32_t DISPLAY_ONSCREEN_TIMER                 = 0x014E;
constexpr int32_t CLEAR_ONSCREEN_TIMER                   = 0x014F;
constexpr int32_t DISPLAY_ONSCREEN_COUNTER               = 0x0150;
constexpr int32_t CLEAR_ONSCREEN_COUNTER                 = 0x0151;
constexpr int32_t FREEZE_ONSCREEN_TIMER                  = 0x0396;
constexpr int32_t DISPLAY_ONSCREEN_COUNTER_WITH_STRING   = 0x03C4;
// A cutscene, and the screen, camera and controls around it.
constexpr int32_t LOAD_SPECIAL_CHARACTER                 = 0x023C;
constexpr int32_t HAS_SPECIAL_CHARACTER_LOADED           = 0x023D;
constexpr int32_t HAS_MODEL_LOADED                       = 0x0248;
constexpr int32_t LOAD_ALL_MODELS_NOW                    = 0x038B;
constexpr int32_t LOAD_CUTSCENE                          = 0x02E4;
constexpr int32_t START_CUTSCENE                         = 0x02E7;
constexpr int32_t CLEAR_CUTSCENE                         = 0x02EA;
constexpr int32_t DO_FADE                                = 0x016A;
constexpr int32_t SWITCH_WIDESCREEN                      = 0x02A3;
constexpr int32_t SET_PLAYER_CONTROL                     = 0x01B4;
constexpr int32_t SET_FIXED_CAMERA_POSITION              = 0x015F;
constexpr int32_t POINT_CAMERA_AT_POINT                  = 0x0160;
constexpr int32_t RESTORE_CAMERA                         = 0x015A;
constexpr int32_t RESTORE_CAMERA_JUMPCUT                 = 0x02EB;
constexpr int32_t SET_CAMERA_BEHIND_PLAYER               = 0x0373;
constexpr int32_t SET_CAMERA_IN_FRONT_OF_PLAYER          = 0x03C8;
constexpr int32_t CAMERA_ON_PLAYER                       = 0x0157;
constexpr int32_t CAMERA_ON_VEHICLE                      = 0x0158;
constexpr int32_t CAMERA_ON_PED                          = 0x0159;
constexpr int32_t SET_PLAYER_COORDINATES                 = 0x0055;
// The 200 table (0x0043D530, base 214, jump table 0x005EEC40) entry 84,
// 0x0043E860: RemoveDriver (0x005520A0) or RemovePassenger (0x00551EB0),
// bInVehicle and m_pMyVehicle cleared, then the ped put down. The car is
// left where it is, which SET_PLAYER_COORDINATES (0x0043A995) never does.
constexpr int32_t WARP_PLAYER_FROM_CAR_TO_COORD          = 0x012A;
constexpr int32_t SET_PLAYER_VISIBLE                     = 0x0336;
constexpr int32_t SET_EVERYONE_IGNORE_PLAYER             = 0x03BF;
constexpr int32_t SET_POLICE_IGNORE_PLAYER               = 0x01F7;
constexpr int32_t SET_PLAYER_NEVER_GETS_TIRED            = 0x0330;
// A HUD item flashing, the way a tutorial points at it: Give Me Liberty's
// "This is the radar" flashes the radar (item 8) for four seconds, then -1.
constexpr int32_t FLASH_HUD_OBJECT                       = 0x03E7;
// The player's clothes (addresses.h, MI_PLAYER): the 800 table's entries 50
// and 51, 0x0044AAFC and 0x0044ABBA. UNDRESS_CHAR takes a char and an
// eight-byte label, DRESS_CHAR the char.
constexpr int32_t UNDRESS_CHAR                           = 0x0352;
constexpr int32_t DRESS_CHAR                             = 0x0353;
// Sets CPad::bApplyBrakes: its handler (0x0044383E) writes [GetPad(player) +
// 0E0h] and touches no car, so a player on foot runs it safely.
constexpr int32_t APPLY_BRAKES_TO_PLAYERS_CAR            = 0x0221;
// The bomb shops (mission-audit.md R6).
constexpr int32_t SET_FREE_BOMB_SHOP                     = 0x021D;
constexpr int32_t ARM_CAR_WITH_BOMB                      = 0x0242;
// The mines (addresses.h, COMMAND_DROP_MINE_Handler): three floats each.
constexpr int32_t DROP_MINE                              = 0x02F0;
constexpr int32_t DROP_NAUTICAL_MINE                     = 0x02F1;
// The garages' two questions (mission-audit.md R5), one operand each. The
// first reads the garage's state and nothing else (0x00426C20: `cmp byte
// [garage+1],5`, GS_CLOSEDCONTAINSCAR), the second takes the respray flag as
// it reads it (0x004274F0: reads [garage+5], then writes 0 there).
constexpr int32_t IS_CAR_IN_MISSION_GARAGE               = 0x021C;
constexpr int32_t HAS_RESPRAY_HAPPENED                   = 0x0329;
// A mission garage's car (mission-audit.md R5). Its handler (0x00443714)
// passes CPools::GetVehicle of the operand, or null for a negative one
// (`test eax,eax / jge`, else `push 0`), to
// CGarages::SetTargetCarForMissonGarage, which stores the raw pointer and
// registers no reference (game/teardown.h).
constexpr int32_t SET_TARGET_CAR_FOR_MISSION_GARAGE      = 0x021B;
// The mission Cessnas (mission-audit.md R7): their starts, and whether each
// has gone down. Both questions only compare the plane's mission status with
// 2 (0x0054E150, 0x0054E250) and write nothing.
constexpr int32_t START_DRUG_RUN                         = 0x033A;
constexpr int32_t HAS_DRUG_PLANE_BEEN_SHOT_DOWN          = 0x033C;
constexpr int32_t START_DRUG_DROP_OFF                    = 0x0358;
constexpr int32_t HAS_DROP_OFF_PLANE_BEEN_SHOT_DOWN      = 0x0359;
// CRestart::OverrideNextRestart sets bOverrideRestart (0x0095CD5D) to 1,
// CancelOverrideRestart sets it to 0.
constexpr int32_t CANCEL_OVERRIDE_RESTART                = 0x01F6;
constexpr int32_t OVERRIDE_NEXT_RESTART                  = 0x016E;
// Being spotted (mission-audit.md R13).
constexpr int32_t HAS_CHAR_SPOTTED_PLAYER                = 0x0123;
constexpr int32_t IS_PLAYER_SHOOTING_IN_AREA             = 0x02D5;
constexpr int32_t IS_PLAYER_IN_MODEL                     = 0x00DE;
constexpr int32_t LOCATE_PLAYER_ANY_MEANS_CHAR_2D        = 0x00E9;
constexpr int32_t LOCATE_PLAYER_ANY_MEANS_CHAR_3D        = 0x00FB;
// The mission's blips, changed.
constexpr int32_t CHANGE_BLIP_COLOUR                     = 0x0165;
constexpr int32_t DIM_BLIP                               = 0x0166;
constexpr int32_t CHANGE_BLIP_SCALE                      = 0x0168;
constexpr int32_t CHANGE_BLIP_DISPLAY                    = 0x018B;
// The mission's fires.
constexpr int32_t REMOVE_SCRIPT_FIRE                     = 0x02D1;
// The mission's objects.
constexpr int32_t CREATE_OBJECT                          = 0x0107;
constexpr int32_t DELETE_OBJECT                          = 0x0108;
constexpr int32_t SET_OBJECT_HEADING                     = 0x0177;
constexpr int32_t SET_OBJECT_COORDINATES                 = 0x01BC;
constexpr int32_t MARK_OBJECT_AS_NO_LONGER_NEEDED        = 0x01C4;
constexpr int32_t FLASH_OBJECT                           = 0x0240;
constexpr int32_t CREATE_OBJECT_NO_OFFSET                = 0x029B;
constexpr int32_t SET_OBJECT_COLLISION                   = 0x0382;
constexpr int32_t SET_OBJECT_DYNAMIC                     = 0x0392;
// The mission's pickups.
constexpr int32_t CREATE_PICKUP                          = 0x0213;
constexpr int32_t REMOVE_PICKUP                          = 0x0215;
constexpr int32_t CREATE_MONEY_PICKUP                    = 0x02E1;
constexpr int32_t CREATE_PICKUP_WITH_AMMO                = 0x032B;
constexpr int32_t CREATE_FLOATING_PACKAGE                = 0x035B;
// A car blown up by the script (0x00442DE4, one operand: GetAt, then
// BlowUpCar(null) through the vtable at 0x00442E08).
constexpr int32_t EXPLODE_CAR                            = 0x020B;
// An enemy made tougher (docs/missions.md 10.2).
constexpr int32_t SET_CHAR_HEALTH                        = 0x0223;
constexpr int32_t ADD_ARMOUR_TO_CHAR                     = 0x035F;
// An enemy's copies (docs/missions.md 10.5): made, armed and let go of.
constexpr int32_t CREATE_CHAR                            = 0x009A;
constexpr int32_t DELETE_CHAR                            = 0x009B;
constexpr int32_t GIVE_WEAPON_TO_CHAR                    = 0x01B2;
constexpr int32_t MARK_CHAR_AS_NO_LONGER_NEEDED          = 0x01C2;
// The end of a script. For the mission slot's, TERMINATE_THIS_SCRIPT is where
// the slot is let go of (0x0043A533, `cmp byte [ebx+85h],0`), and so the end
// of the mission for everybody: most missions run MISSION_HAS_FINISHED twice
// on the way out when they fail, once in the failure and once in the cleanup
// after it (Give Me Liberty's are at EIGHT_15406 and EIGHT_15689).
constexpr int32_t TERMINATE_THIS_SCRIPT                  = 0x004E;
// The mission's cars. CREATE_CAR (0x0043C476) leaves the new car's handle in
// ScriptParams[0] for StoreParameters (`mov [006ED460h],eax` at 0x0043C79C);
// DELETE_CAR (0x0043C7D5) and DONT_REMOVE_CAR (0x0044187F) take one.
constexpr int32_t CREATE_CAR                             = 0x00A5;
constexpr int32_t DELETE_CAR                             = 0x00A6;
constexpr int32_t DONT_REMOVE_CAR                        = 0x01C6;
// IS_PLAYER_IN_CAR player car (0x0043D959): the player's ped's bInVehicle
// (+0x314) and m_pMyVehicle (+0x310) against the vehicle pool's GetAt of the
// second operand.
constexpr int32_t IS_PLAYER_IN_CAR                       = 0x00DC;
// SET_CHAR_OBJ_LEAVE_CAR char car: the ped's objective to get out of the car.
// Deal Steal and Shima give it to the owner's own ped (GET_PLAYER_CHAR) at the
// casino and then wait on IS_PLAYER_IN_CAR (standin.h, the car the owner was
// walked out of).
constexpr int32_t SET_CHAR_OBJ_LEAVE_CAR                 = 0x01D3;
// Takes an object off the mission's cleanup list (0x004418BE: GetAt on the
// object pool, then CMissionCleanup::RemoveEntityFromList on 0x008F2A24 with
// type 3): the object stays in the world after the mission.
constexpr int32_t DONT_REMOVE_OBJECT                     = 0x01C7;
// Six floats, the two corners of a box (0x0044E267): every particle object in
// it is taken away.
constexpr int32_t REMOVE_PARTICLE_EFFECTS_IN_AREA        = 0x03AE;
// The words on the screen, a text label first.
constexpr int32_t PRINT                                  = 0x00BB;
constexpr int32_t PRINT_NOW                              = 0x00BC;
constexpr int32_t PRINT_SOON                             = 0x00BD;
constexpr int32_t CLEAR_THIS_PRINT                       = 0x03D5;
// A pedestrian told to kill the player, and the same objective at a pedestrian.
constexpr int32_t SET_CHAR_OBJ_KILL_CHAR_ON_FOOT         = 0x01C9;
constexpr int32_t SET_CHAR_OBJ_KILL_PLAYER_ON_FOOT       = 0x01CA;
constexpr int32_t SET_CHAR_OBJ_KILL_CHAR_ANY_MEANS       = 0x01CB;
constexpr int32_t SET_CHAR_OBJ_KILL_PLAYER_ANY_MEANS     = 0x01CC;
// A char into a car (seatplan.h, PlanScriptedWheel). All four collect a char,
// or 0369 a player, then a car, and end in CPed::SetObjective (0x004D83E0):
// 01D4 at 0x00441FD0 with `push 0Eh` (ENTER_CAR_AS_PASSENGER), 01D5 at
// 0x00442029 with `push 0Fh` (ENTER_CAR_AS_DRIVER), both clearing
// bScriptObjectiveCompleted (`and cl,7Fh` into +157h) first. 0369 at
// 0x0044BB17 takes the ped from CWorld::Players[ScriptParams[0]] (`imul
// ebx,ebx,13Ch / add ebx,9412F0h`, then `mov ecx,[ebx]`), 036A at 0x0044BB7C
// from the ped pool, and each follows `push 0Fh` with CPed::WarpPedIntoCar
// (0x004D7D20).
// How many ride in a car, and how many can (range 400's table 0x005EEFC8,
// base 400, entries 89 and 90). Each collects the car, GetAt on the vehicle
// pool, and stores one byte of it: 01E9 at 0x0044266E `movzx eax,byte
// [eax+1C8h]` (m_nNumPassengers), 01EA at 0x004426B4 `movzx eax,byte
// [eax+1CCh]` (m_nNumMaxPassengers), each then StoreParameters (0x004385A0).
// The Paramedic asks both of its ambulance and calls it full when they are
// equal (11_ambulance.sc:1122-1128), the only place main.scm asks either.
constexpr int32_t GET_NUMBER_OF_PASSENGERS               = 0x01E9;
constexpr int32_t SET_CHAR_OBJ_ENTER_CAR_AS_PASSENGER    = 0x01D4;
constexpr int32_t SET_CHAR_OBJ_ENTER_CAR_AS_DRIVER       = 0x01D5;
constexpr int32_t WARP_PLAYER_INTO_CAR                   = 0x0369;
constexpr int32_t WARP_CHAR_INTO_CAR                     = 0x036A;
// A char tied to a player and let go again (range 400's table 0x005EEFC8,
// entries 79 and 80). 01DF at 0x00442269 collects two, the char and the
// player, and calls CPed::SetObjective (0x004D83E0) with `push 1Ah`
// (OBJECTIVE_SET_LEADER) and CWorld::Players[n].m_pPed (`imul ecx,ecx,4Fh /
// mov esi,[ecx*4+9412F0h]`). 01E0 at 0x004422B0 collects one, the char, and
// calls CPed::ClearLeader (0x004D8E80) on it.
constexpr int32_t SET_PLAYER_AS_LEADER                   = 0x01DF;
constexpr int32_t CLEAR_LEADER                           = 0x01E0;
// What a mission sets for a while and a participant is given back at its end
// (replay.h has each handler). The launch's make-safe (0x005885F9) sets the
// pad's CUTSCENE bit (`or byte [eax+0DFh],80h`), makes the player safe
// (0x004A1400) and sets ms_cutsceneProcessing (`mov byte [95CD9Fh],1`); only
// a cutscene's teardown clears the first and the last.
constexpr int32_t MAKE_PLAYER_SAFE_FOR_CUTSCENE          = 0x03EF;
constexpr int32_t SET_FREE_RESPRAYS                      = 0x0335;
constexpr int32_t SET_WANTED_MULTIPLIER                  = 0x03C7;
constexpr int32_t OVERRIDE_HOSPITAL_LEVEL                = 0x041F;
constexpr int32_t OVERRIDE_POLICE_STATION_LEVEL          = 0x0420;
constexpr int32_t SWITCH_WORLD_PROCESSING                = 0x03B7;
constexpr int32_t SET_ALL_CARS_CAN_BE_DAMAGED            = 0x03F4;
constexpr int32_t SET_GENERATE_CARS_AROUND_CAMERA        = 0x03EA;
constexpr int32_t SET_NEAR_CLIP                          = 0x041D;
constexpr int32_t SET_MUSIC_DOES_FADE                    = 0x043C;
constexpr int32_t PLAY_END_OF_GAME_TUNE                  = 0x043F;
constexpr int32_t STOP_END_OF_GAME_TUNE                  = 0x0440;
constexpr int32_t ADD_CONTINUOUS_SOUND                   = 0x018D;
constexpr int32_t REMOVE_SOUND                           = 0x018E;
} // namespace op

// CPools' audio script object pool, the one REMOVE_SOUND looks its handle up
// in: its handler (0x00440BF2) loads `mov ecx,[008F1B6Ch]` and calls the
// pool's GetAt (0x00442950), which indexes the flag array with `handle >> 8`
// and checks nothing against the pool's size. So a handle a participant's
// own global holds is run only when it is inside the pool.
constexpr uintptr_t AUDIO_SCRIPT_OBJECT_POOL = 0x008F1B6C;
// The near clip Ray's missions put back as they end (54_ray1.sc:134).
constexpr float NEAR_CLIP_AFTER_MISSION = 0.875f;

// CPool<CPed, CPlayerPed>::GetAt(int32 handle), __thiscall on the ped pool:
// how the char instructions' handlers turn the script's handle into a ped.
// mission.cpp finds ADD_BLIP_FOR_CHAR's call to it again in the running image
// before it hooks it.
using game::CPool_CPed__GetAt;
using game::ADD_BLIP_FOR_CHAR_HANDLER;

// The screen's fade, on TheCamera (addresses.h). DO_FADE's handler (range 300's
// table entry 58, 0x0043FBC0) calls CCamera::Fade (0x0046B3A0) on it, which
// sets CAMERA_FADING first thing; CCamera::GetScreenFadeStatus (0x0046B9C0)
// reads CAMERA_FADE and says 2, faded out, at 255.0. A fade that has run its
// course leaves CAMERA_FADING clear, so "left faded out" is 255 and not fading.
constexpr size_t CAMERA_FADING = 0xE8F3;   // bool
constexpr size_t CAMERA_FADE   = 0xE998;   // float, 0 clear .. 255 faded out
constexpr float  CAMERA_FADED  = 255.0f;

// A z the coordinate handlers take as "on the ground here, wherever that is":
// they find it themselves.
using game::SCRIPT_Z_FIND_GROUND;

// The blue marker a location check draws with its sphere set, and the HUD
// item a tutorial flashes (addresses.h has the instructions for both).
constexpr uintptr_t HIGHLIGHT_AREA = CTheScripts__HighlightImportantArea;
using game::CHud__m_ItemToFlash;
using game::HUD_ITEM_NONE;
using game::HUD_ITEM_RADAR;

} // namespace coopiii::game::scripts
