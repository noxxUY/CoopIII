// Every place CoopIII rewrites the game's code, in one table, checked against
// each other and against the retail gta3.exe.
//
// The per-feature tests each pin their own sites. What none of them can see
// is the others: two modules taking the same call, a redirect landing inside
// the bytes a detour overwrites, or a detour on a function whose own code
// jumps back into its first instructions (the reason CHeli's
// SpecialHeliPreRender is taken at its call site instead). Those are checked
// here, over all of them at once.

#include "game/addresses.h"
#include "game/animcb.h"
#include "game/cargen.h"
#include "game/carletgo.h"
#include "game/crowdaddr.h"
#include "game/emergencyaddr.h"
#include "game/garage.h"
#include "game/heli.h"
#include "game/missionaddr.h"
#include "game/pause.h"
#include "game/replicacalm.h"
#include "game/social.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using namespace coopiii;
using namespace coopiii::game;

namespace {

int g_patchFailures = 0;

void Check(bool cond, const char *what) {
	std::printf("  [%s] %s\n", cond ? "ok" : "FAIL", what);
	if (!cond)
		++g_patchFailures;
}

// A `call rel32` CoopIII rewrites, and the function it must still reach
// when CoopIII finds it.
struct Site {
	uintptr_t   at;
	uintptr_t   calls;
	const char *name;
};

// An inline detour: MinHook overwrites the first instructions that cover
// five bytes, `prologue` bytes in all.
struct Detoured {
	uintptr_t   at;
	uint32_t    prologue;
	const char *name;
};

const Site kSites[] = {
    {CCutsceneMgr__Update_FinishCall, CCutsceneMgr__FinishCutscene, "CCutsceneMgr__Update_FinishCall"},
    {GENERATE_ONE_RANDOM_CAR_CALLS[0], CCarCtrl__GenerateOneRandomCar, "GENERATE_ONE_RANDOM_CAR_CALLS[0]"},
    {GENERATE_ONE_RANDOM_CAR_CALLS[1], CCarCtrl__GenerateOneRandomCar, "GENERATE_ONE_RANDOM_CAR_CALLS[1]"},
    {POSSIBLY_REMOVE_VEHICLE_CALLS[0], CCarCtrl__PossiblyRemoveVehicle, "POSSIBLY_REMOVE_VEHICLE_CALLS[0]"},
    {GameLogic_WastedClearCall, CWorld__ClearExcitingStuffFromArea, "GameLogic_WastedClearCall"},
    {GameLogic_BustedClearCall, CWorld__ClearExcitingStuffFromArea, "GameLogic_BustedClearCall"},
    {GameLogic_FailedClearCall, CWorld__ClearExcitingStuffFromArea, "GameLogic_FailedClearCall"},
    {GARAGE_DESTROY_MISSION_CALL, DestroyVehicleAndDriverAndPassengers, "GARAGE_DESTROY_MISSION_CALL"},
    {GARAGE_DESTROY_COLLECTED_CALL, DestroyVehicleAndDriverAndPassengers, "GARAGE_DESTROY_COLLECTED_CALL"},
    {GARAGE_DESTROY_CRAIG_CALL, DestroyVehicleAndDriverAndPassengers, "GARAGE_DESTROY_CRAIG_CALL"},
    {GARAGE_DESTROY_CRUSHER_CALL, DestroyVehicleAndDriverAndPassengers, "GARAGE_DESTROY_CRUSHER_CALL"},
    {HIDEOUT_STORE_UPDATE_CALL, CGarage__StoreAndRemoveCarsForThisHideout, "HIDEOUT_STORE_UPDATE_CALL"},
    {HIDEOUT_STORE_CLOSE_CALL, CGarage__StoreAndRemoveCarsForThisHideout, "HIDEOUT_STORE_CLOSE_CALL"},
    {CPickup__Update_MineExplosion, CExplosion__AddExplosion, "CPickup__Update_MineExplosion"},
    {IS_BUTTON_PRESSED_PadStateCall, CRunningScript__GetPadState, "IS_BUTTON_PRESSED_PadStateCall"},
    {IS_BUTTON_PRESSED_CompareCall, CRunningScript__UpdateCompareFlag, "IS_BUTTON_PRESSED_CompareCall"},
    {POINT_CAMERA_AT_CAR_TakeControlCall, CCamera__TakeControl, "POINT_CAMERA_AT_CAR_TakeControlCall"},
    {IS_CAR_IN_AIR_PROPER_ANSWER, CRunningScript__UpdateCompareFlag, "IS_CAR_IN_AIR_PROPER_ANSWER"},
    {RESTORE_CAMERA_JUMPCUT_Call, CCamera__RestoreWithJumpCut, "RESTORE_CAMERA_JUMPCUT_Call"},
    {CCam__Process_CamOnAStringCall, CCam__Process_Cam_On_A_String, "CCam__Process_CamOnAStringCall"},
    {FollowPedMouse_LineOfSightCall1, CWorld__ProcessLineOfSight, "FollowPedMouse_LineOfSightCall1"},
    {FollowPedMouse_LineOfSightCall2, CWorld__ProcessLineOfSight, "FollowPedMouse_LineOfSightCall2"},
    {FollowPedMouse_SphereCall1, CWorld__TestSphereAgainstWorld, "FollowPedMouse_SphereCall1"},
    {FollowPedMouse_SphereCall2, CWorld__TestSphereAgainstWorld, "FollowPedMouse_SphereCall2"},
    {CGame__Process_CameraCall, CCamera__Process, "CGame__Process_CameraCall"},
    {CRenderer__PreRender_SpecialHeliCall, CHeli__SpecialHeliPreRender, "CRenderer__PreRender_SpecialHeliCall"},
    {ClearExciting_FireCall, CFireManager__ExtinguishPoint, "ClearExciting_FireCall"},
    {ClearExciting_CarFireCall, CWorld__ExtinguishAllCarFiresInArea, "ClearExciting_CarFireCall"},
    {ClearExciting_ExplosionCall, CExplosion__RemoveAllExplosionsInArea, "ClearExciting_ExplosionCall"},
    {ClearExciting_ProjectileCall, CProjectileInfo__RemoveAllProjectiles, "ClearExciting_ProjectileCall"},
    {HEAD_ADD_ANIM_TIME_CALL, RpHAnimHierarchyAddAnimTime, "HEAD_ADD_ANIM_TIME_CALL"},
    {HEAD_UPDATE_MATRICES_CALL, RpHAnimUpdateHierarchyMatrices, "HEAD_UPDATE_MATRICES_CALL"},
    {HEAD_OBJECT_RENDER_CALL, object::CObject__Render, "HEAD_OBJECT_RENDER_CALL"},
    {MEDIC_FIND_ACCIDENT_CALLS[0], CAccidentManager__FindNearestAccident, "MEDIC_FIND_ACCIDENT_CALLS[0]"},
    {MEDIC_FIND_ACCIDENT_CALLS[1], CAccidentManager__FindNearestAccident, "MEDIC_FIND_ACCIDENT_CALLS[1]"},
    {MEDIC_FIND_ACCIDENT_CALLS[2], CAccidentManager__FindNearestAccident, "MEDIC_FIND_ACCIDENT_CALLS[2]"},
    {MEDIC_REVIVE_GETUP_CALL, CPed__SetGetUp, "MEDIC_REVIVE_GETUP_CALL"},
    {EVASIVE_STEP_SITES[0], CPed__SetEvasiveStep, "EVASIVE_STEP_SITES[0]"},
    {EVASIVE_STEP_SITES[1], CPed__SetEvasiveStep, "EVASIVE_STEP_SITES[1]"},
    {CPed__ProcessControl_CarDriverIsPlayer, CPed__IsPlayer, "CPed__ProcessControl_CarDriverIsPlayer"},
    {CPed__ProcessControl_CarHitDamage, CPed__InflictDamage, "CPed__ProcessControl_CarHitDamage"},
    {LOOK_PEDS_SITE, CPed__LookForSexyPeds, "LOOK_PEDS_SITE"},
    {LOOK_CARS_SITE, CPed__LookForSexyCars, "LOOK_CARS_SITE"},
    {EVASIVE_STEP_SITES[2], CPed__SetEvasiveStep, "EVASIVE_STEP_SITES[2]"},
    {EVASIVE_STEP_SITES[3], CPed__SetEvasiveStep, "EVASIVE_STEP_SITES[3]"},
    {EVASIVE_STEP_SITES[4], CPed__SetEvasiveStep, "EVASIVE_STEP_SITES[4]"},
    {EVASIVE_STEP_SITES[5], CPed__SetEvasiveStep, "EVASIVE_STEP_SITES[5]"},
    {CPed__KillPedWithCar_KillDamage, CPed__InflictDamage, "CPed__KillPedWithCar_KillDamage"},
    {CPed__KillPedWithCar_KnockDamage, CPed__InflictDamage, "CPed__KillPedWithCar_KnockDamage"},
    {CPhysical__Collision_KillPedWithCar, CPed__KillPedWithCar, "CPhysical__Collision_KillPedWithCar"},
    {CPed__ProcessControl_KillPedWithCar, CPed__KillPedWithCar, "CPed__ProcessControl_KillPedWithCar"},
    {LockOn_CanSeeCall, CPed__OurPedCanSeeThisOne, "LockOn_CanSeeCall"},
    {NextLockOn_CanSeeCall, CPed__OurPedCanSeeThisOne, "NextLockOn_CanSeeCall"},
    {POSSIBLY_REMOVE_VEHICLE_CALLS[1], CCarCtrl__PossiblyRemoveVehicle, "POSSIBLY_REMOVE_VEHICLE_CALLS[1]"},
    {MANAGE_POPULATION_REMOVE_PED_CALL, CPopulation__RemovePed, "MANAGE_POPULATION_REMOVE_PED_CALL"},
    {SPAWN_SPOT_TESTS[0], CWorld__FindObjectsKindaColliding, "SPAWN_SPOT_TESTS[0]"},
    {SPAWN_SPOT_TESTS[1], CWorld__FindObjectsKindaColliding, "SPAWN_SPOT_TESTS[1]"},
    {SPAWN_SPOT_TESTS[2], CWorld__FindObjectsKindaColliding, "SPAWN_SPOT_TESTS[2]"},
    {SPAWN_SPOT_TESTS[3], CWorld__FindObjectsKindaColliding, "SPAWN_SPOT_TESTS[3]"},
    {object::ManagePopulation_ConvertToRealObjectCall, object::CPopulation__ConvertToRealObject, "obj::ManagePopulation_ConvertToRealObjectCall"},
    {object::GLASS_COLLISION_PED_CALL, object::CGlass__WindowRespondsToCollision, "obj::GLASS_COLLISION_PED_CALL"},
    {object::GLASS_COLLISION_CALL, object::CGlass__WindowRespondsToCollision, "obj::GLASS_COLLISION_CALL"},
    {object::GLASS_BULLET_BREAK_CALL, object::CGlass__WindowRespondsToCollision, "obj::GLASS_BULLET_BREAK_CALL"},
    {object::GLASS_BLAST_BREAK_CALL, object::CGlass__WindowRespondsToCollision, "obj::GLASS_BLAST_BREAK_CALL"},
    {WATER_CANNON_EXTINGUISH_CALL, CFireManager__ExtinguishPoint, "WATER_CANNON_EXTINGUISH_CALL"},
    {PUSH_PEDS_SIDE_CALL, CPed__GetLocalDirection, "PUSH_PEDS_SIDE_CALL"},
    {PUSH_PEDS_FORCE_CALL, CPhysical__ApplyMoveForce, "PUSH_PEDS_FORCE_CALL"},
    {PUSH_PEDS_FALL_CALL, CPed__SetFall, "PUSH_PEDS_FALL_CALL"},
    {PUSH_PEDS_EXTINGUISH_CALL, CFire__Extinguish, "PUSH_PEDS_EXTINGUISH_CALL"},
    {FIRE_TRUCK_PLAYER_CAR_CALL, FindPlayerVehicle, "FIRE_TRUCK_PLAYER_CAR_CALL"},
    {FIRE_TRUCK_CANNON_CALL, CWaterCannons__UpdateOne, "FIRE_TRUCK_CANNON_CALL"},
    {ROOF_DRAIN_PLAYER_CAR_CALL, FindPlayerVehicle, "ROOF_DRAIN_PLAYER_CAR_CALL"},
    {TANK_PLAYER_CAR_CALL, FindPlayerVehicle, "TANK_PLAYER_CAR_CALL"},
    {CRANE_FIND_VEHICLES_CALL, CCrane__FindCarInSectorList, "CRANE_FIND_VEHICLES_CALL"},
    {CRANE_FIND_OVERLAP_CALL, CCrane__FindCarInSectorList, "CRANE_FIND_OVERLAP_CALL"},
    {CRANE_MILITARY_REMOVE_CALL, CWorld__Remove, "CRANE_MILITARY_REMOVE_CALL"},
    {CRANE_UPDATE_CALL, CCrane__Update, "CRANE_UPDATE_CALL"},
    {BombTimer_FindPlayerVehicleCall, FindPlayerVehicle, "BombTimer_FindPlayerVehicleCall"},
    {BombTimer_FindPlayerPedCall, FindPlayerPed, "BombTimer_FindPlayerPedCall"},
    {ENGINE_ACCELERATE_CALLS[0], CPad__GetAccelerate, "ENGINE_ACCELERATE_CALLS[0]"},
    {ENGINE_ACCELERATE_CALLS[1], CPad__GetAccelerate, "ENGINE_ACCELERATE_CALLS[1]"},
    {ENGINE_BRAKE_CALLS[0], CPad__GetBrake, "ENGINE_BRAKE_CALLS[0]"},
    {ENGINE_ACCELERATE_CALLS[2], CPad__GetAccelerate, "ENGINE_ACCELERATE_CALLS[2]"},
    {ENGINE_ACCELERATE_CALLS[3], CPad__GetAccelerate, "ENGINE_ACCELERATE_CALLS[3]"},
    {ENGINE_BRAKE_CALLS[1], CPad__GetBrake, "ENGINE_BRAKE_CALLS[1]"},
    {ENGINE_BRAKE_CALLS[2], CPad__GetBrake, "ENGINE_BRAKE_CALLS[2]"},
    {ENGINE_ACCELERATE_CALLS[4], CPad__GetAccelerate, "ENGINE_ACCELERATE_CALLS[4]"},
    {ENGINE_ACCELERATE_CALLS[5], CPad__GetAccelerate, "ENGINE_ACCELERATE_CALLS[5]"},
    {ENGINE_BRAKE_CALLS[3], CPad__GetBrake, "ENGINE_BRAKE_CALLS[3]"},
    {MUSIC_SERVICE_CALL, cMusicManager__Service, "MUSIC_SERVICE_CALL"},
    {SERVICE_TRACK_RESTART, cSampleManager__StartStreamedFile, "SERVICE_TRACK_RESTART"},
    {CHANGE_RADIO_CHANNEL_START_POS, cMusicManager__GetTrackStartPos, "CHANGE_RADIO_CHANNEL_START_POS"},
    {LEAVE_CAR_SET_EXIT_CAR_CALL, CPed__SetExitCar, "LEAVE_CAR_SET_EXIT_CAR_CALL"},
    {NEW_DRIVER_PASSENGER_LEAVE_CALL, CPed__SetObjective, "NEW_DRIVER_PASSENGER_LEAVE_CALL"},
    {CARGEN_INTERNAL_CALL, CCarGenerator__DoInternalProcessing, "CARGEN_INTERNAL_CALL"},
    {HIDEOUT_RESTORE_CALL, CGarage__RestoreCarsForThisHideout, "HIDEOUT_RESTORE_CALL"},
};

const Detoured kDetoured[] = {
    {CStreaming__RequestSpecialModel, 7, "CStreaming__RequestSpecialModel"},
    {CBridge__Update, 10, "CBridge__Update"},
    {CCarAI__UpdateCarAI, 8, "CCarAI__UpdateCarAI"},
    {CCarCtrl__SteerAICarWithPhysics, 8, "CCarCtrl__SteerAICarWithPhysics"},
    {CCarCtrl__SteerAIBoatWithPhysics, 5, "CCarCtrl__SteerAIBoatWithPhysics"},
    {CDarkel__ReadStatus, 6, "CDarkel__ReadStatus"},
    {CDarkel__RegisterKillByPlayer, 8, "CDarkel__RegisterKillByPlayer"},
    {CDarkel__RegisterCarBlownUpByPlayer, 8, "CDarkel__RegisterCarBlownUpByPlayer"},
    {CDarkel__StartFrenzy, 5, "CDarkel__StartFrenzy"},
    {CGarage__Update, 6, "CGarage__Update"},
    {CPickups__Update, 5, "CPickups__Update"},
    {CPed__CreateDeadPedMoney, 7, "CPed__CreateDeadPedMoney"},
    {CPed__CreateDeadPedWeaponPickups, 5, "CPed__CreateDeadPedWeaponPickups"},
    {CRunningScript__ProcessCommands0To99, 5, "CRunningScript__ProcessCommands0To99"},
    {CRunningScript__ProcessCommands100To199, 10, "CRunningScript__ProcessCommands100To199"},
    {CRunningScript__ProcessCommands200To299, 10, "CRunningScript__ProcessCommands200To299"},
    {CPool_CPed__GetAt, 5, "CPool_CPed__GetAt"},
    {CRunningScript__ProcessCommands300To399, 5, "CRunningScript__ProcessCommands300To399"},
    {CRunningScript__ProcessCommands400To499, 10, "CRunningScript__ProcessCommands400To499"},
    {CRunningScript__ProcessCommands500To599, 5, "CRunningScript__ProcessCommands500To599"},
    {CRunningScript__ProcessCommands600To699, 10, "CRunningScript__ProcessCommands600To699"},
    {CRunningScript__ProcessCommands700To799, 10, "CRunningScript__ProcessCommands700To799"},
    {CRunningScript__ProcessCommands800To899, 10, "CRunningScript__ProcessCommands800To899"},
    {CRunningScript__ProcessCommands900To999, 10, "CRunningScript__ProcessCommands900To999"},
    {CTheScripts__HighlightImportantArea, 7, "CTheScripts__HighlightImportantArea"},
    {CTrafficLights__LightForCars1, 5, "CTrafficLights__LightForCars1"},
    {CTrafficLights__LightForCars2, 5, "CTrafficLights__LightForCars2"},
    {CTrafficLights__LightForPeds, 5, "CTrafficLights__LightForPeds"},
    {CEventList__ReportCrimeForEvent, 7, "CEventList__ReportCrimeForEvent"},
    {CFireManager__StartFireEntity, 7, "CFireManager__StartFireEntity"},
    {CGame__Process, 5, "CGame__Process"},
    {FrontendIdle, 8, "FrontendIdle"},
    {CPad__AddToPCCheatString, 7, "CPad__AddToPCCheatString"},
    {CPhysical__AddToMovingList, 5, "CPhysical__AddToMovingList"},
    {CPlayerInfo__AwardMoneyForExplosion, 5, "CPlayerInfo__AwardMoneyForExplosion"},
    {CPools__SaveVehiclePool, 10, "CPools__SaveVehiclePool"},
    {GenericSave, 10, "GenericSave"},
    {CRadar__DrawBlips, 7, "CRadar__DrawBlips"},
    {CTimer__Update, 6, "CTimer__Update"},
    {CWanted__RegisterCrime_Immediately, 5, "CWanted__RegisterCrime_Immediately"},
    {CWorld__Add, 5, "CWorld__Add"},
    {CWorld__Remove, 5, "CWorld__Remove"},
    {object::CWorld__TriggerExplosion, 7, "CWorld__TriggerExplosion"},
    {CWorld__Process, 5, "CWorld__Process"},
    {object::CObject__ObjectDamage, 10, "CObject__ObjectDamage"},
    {CPed__ScanForThreats, 5, "CPed__ScanForThreats"},
    {CPed__SetFall, 7, "CPed__SetFall"},
    {CPed__SetDie, 7, "CPed__SetDie"},
    {CPed__ReactToAttack, 5, "CPed__ReactToAttack"},
    {CPed__StartFightDefend, 5, "CPed__StartFightDefend"},
    {CPed__FightStrike, 6, "CPed__FightStrike"},
    {CPed__InflictDamage, 6, "CPed__InflictDamage"},
    {CPed__RemoveBodyPart, 7, "CPed__RemoveBodyPart"},
    {CPedIK__PointGunInDirection, 7, "CPedIK__PointGunInDirection"},
    {CHud__Draw, 10, "CHud__Draw"},
    {CBulletTraces__AddTrace, 5, "CBulletTraces__AddTrace"},
    {CAutomobile__BlowUpCar, 5, "CAutomobile__BlowUpCar"},
    {CBoat__BlowUpCar, 6, "CBoat__BlowUpCar"},
    {CHeli__ProcessControl, 10, "CHeli__ProcessControl"},
    {CHeli__UpdateHelis, 5, "CHeli__UpdateHelis"},
    {CHeli__TestRocketCollision, 7, "CHeli__TestRocketCollision"},
    {CHeli__TestBulletCollision, 7, "CHeli__TestBulletCollision"},
    {CPlane__UpdatePlanes, 6, "CPlane__UpdatePlanes"},
    {PLANE_ROCKET_TEST_LEAD, 8, "PLANE_ROCKET_TEST_LEAD"},
    {CTrain__UpdateTrains, 7, "CTrain__UpdateTrains"},
    {CVehicle__InflictDamage, 5, "CVehicle__InflictDamage"},
    {CExplosion__AddExplosion, 5, "CExplosion__AddExplosion"},
    {CProjectileInfo__RemoveProjectile, 6, "CProjectileInfo__RemoveProjectile"},
    {CShotInfo__Update, 7, "CShotInfo__Update"},
    {CWeapon__Fire, 7, "CWeapon__Fire"},
    {CWeapon__FireFromCar, 8, "CWeapon__FireFromCar"},
    {CWeapon__FireMelee, 10, "CWeapon__FireMelee"},
    {CWeapon__DoBulletImpact, 10, "CWeapon__DoBulletImpact"},
    {CWeapon__DoDoomAiming, 10, "CWeapon__DoDoomAiming"},
    {FireOneInstantHitRound, 10, "FireOneInstantHitRound"},
    {CWeapon__ProcessLineOfSight, 8, "CWeapon__ProcessLineOfSight"},
    {cAudioManager__ProcessVehicleSirenOrAlarm, 5, "cAudioManager__ProcessVehicleSirenOrAlarm"},
    {CRunningScript__ProcessCommands1000To1099, 5, "CRunningScript__ProcessCommands1000To1099"},
    {CRunningScript__ProcessCommands1100To1199, 10, "CRunningScript__ProcessCommands1100To1199"},
};

// The byte patches that are neither: the honk's jump into the car scan, the
// font's y test and the adrenaline store.
struct Span {
	uintptr_t   at;
	uint32_t    len;
	const char *name;
};

const Span kBytes[] = {
    {CPlayerPed__ProcessControl_RollingDoorCBPush, 5, "CPlayerPed__ProcessControl_RollingDoorCBPush"},
    {CPed__SetExitTrain_OutTrainCBPush, 5, "CPed__SetExitTrain_OutTrainCBPush"},
    {SlowCarDown_StatusTest, sizeof SLOWCARDOWN_STATUS_TEST, "the honk's status test"},
    {CFont__PrintChar_CullY, sizeof PRINTCHAR_CULL_Y_HEIGHT, "CFont::PrintChar's y test"},
    {ADRENALINE_SLOWDOWN_STORE, sizeof ADRENALINE_SLOWDOWN_BYTES, "the adrenaline slowdown"},
    {SET_EXIT_CAR_ROOM_CALL, SET_EXIT_CAR_ROOM_CALL_LEN, "SetExitCar's room test"},
    {WINMAIN_WAITMESSAGE_CALL, sizeof WAITMESSAGE_CALL_BYTES, "WinMain's WaitMessage call"},
};

std::vector<Span> AllSpans() {
	std::vector<Span> out;
	for (const Site &s : kSites)
		out.push_back({s.at, 5, s.name});
	for (const Detoured &d : kDetoured)
		out.push_back({d.at, d.prologue, d.name});
	for (const Span &b : kBytes)
		out.push_back(b);
	std::sort(out.begin(), out.end(), [](const Span &a, const Span &b) { return a.at < b.at; });
	return out;
}

void TestNothingOverlaps() {
	std::printf("\npatch sites: no two of CoopIII's own patches share a byte\n");
	const std::vector<Span> spans = AllSpans();
	int                     clashes = 0;
	for (size_t i = 1; i < spans.size(); ++i)
		if (spans[i].at < spans[i - 1].at + spans[i - 1].len) {
			++clashes;
			std::printf("  %s (0x%08X) runs into %s (0x%08X)\n", spans[i - 1].name,
			            static_cast<unsigned>(spans[i - 1].at), spans[i].name,
			            static_cast<unsigned>(spans[i].at));
		}
	Check(clashes == 0, "every redirect, detour and byte patch has its bytes to itself");
	bool sane = true;
	for (const Detoured &d : kDetoured)
		sane &= d.prologue >= 5 && d.prologue < 16;
	Check(sane, "every detour's prologue covers the five bytes MinHook writes, and no more "
	            "than one more instruction");
}

// ---- against the real exe ---------------------------------------------------

bool LoadExe(std::vector<uint8_t> &image, std::string &from) {
	std::vector<std::string> candidates;
	if (const char *env = std::getenv("COOPIII_GTA3_EXE"))
		candidates.push_back(env);
	candidates.push_back("reference/bin/gta3.exe");
	candidates.push_back("../../../../reference/bin/gta3.exe");
	for (const std::string &path : candidates) {
		FILE *fh = std::fopen(path.c_str(), "rb");
		if (!fh)
			continue;
		std::fseek(fh, 0, SEEK_END);
		const long size = std::ftell(fh);
		std::fseek(fh, 0, SEEK_SET);
		image.resize(size > 0 ? size_t(size) : 0);
		const size_t got = image.empty() ? 0 : std::fread(image.data(), 1, image.size(), fh);
		std::fclose(fh);
		if (got == image.size() && image.size() == IMAGE_SIZE) {
			from = path;
			return true;
		}
	}
	return false;
}

int32_t Rel32(const std::vector<uint8_t> &img, uint32_t va) {
	const size_t o = va - IMAGE_BASE;
	return static_cast<int32_t>(uint32_t(img[o]) | uint32_t(img[o + 1]) << 8 |
	                            uint32_t(img[o + 2]) << 16 | uint32_t(img[o + 3]) << 24);
}

bool CallsAt(const std::vector<uint8_t> &img, uint32_t site, uint32_t target) {
	return img[site - IMAGE_BASE] == 0xE8 && site + 5 + Rel32(img, site + 1) == target;
}

// Byte patterns that read as a relative jump or call from somewhere in .text,
// wherever they sit. This finds more than there are - it cannot tell the
// start of an instruction from the middle of one - so it only ever errs on
// the side of refusing.
//
// Three hits are the middle of longer instructions, read by hand:
//   0x0043363C  inside `push dword [esp+1Ch]` at 0x0043363B
//   0x004429A1  inside `shr ebx,1Fh` at 0x004429A0
//   0x004A4291  inside `fmul dword [5F7100h]` at 0x004A428E
bool KnownMiddleOfAnInstruction(uint32_t from) {
	return from == 0x0043363C || from == 0x004429A1 || from == 0x004A4291;
}

void TestAgainstTheImage() {
	std::printf("\npatch sites, against gta3.exe\n");
	std::vector<uint8_t> img;
	std::string          from;
	if (!LoadExe(img, from)) {
		std::printf("  [skipped] no retail gta3.exe; set COOPIII_GTA3_EXE to check every patch "
		            "site against one\n");
		return;
	}
	std::printf("  reading %s\n", from.c_str());

	int wrong = 0;
	for (const Site &s : kSites)
		if (!CallsAt(img, static_cast<uint32_t>(s.at), static_cast<uint32_t>(s.calls))) {
			++wrong;
			std::printf("  %s at 0x%08X is not `call 0x%08X`\n", s.name,
			            static_cast<unsigned>(s.at), static_cast<unsigned>(s.calls));
		}
	Check(wrong == 0, "every call CoopIII redirects is, in the retail image, a call to the "
	                  "function it verifies before taking it");

	const uint32_t first = 0x00401000, last = 0x005E3000;
	int            into  = 0;
	for (uint32_t va = first; va + 6 <= last; ++va) {
		const uint8_t b = img[va - IMAGE_BASE];
		uint32_t      to;
		if (b == 0xE8 || b == 0xE9)
			to = va + 5 + Rel32(img, va + 1);
		else if (b == 0xEB || (b >= 0x70 && b <= 0x7F))
			to = va + 2 + static_cast<int8_t>(img[va + 1 - IMAGE_BASE]);
		else if (b == 0x0F && img[va + 1 - IMAGE_BASE] >= 0x80 && img[va + 1 - IMAGE_BASE] <= 0x8F)
			to = va + 6 + Rel32(img, va + 2);
		else
			continue;
		for (const Detoured &d : kDetoured)
			if (to > d.at && to < d.at + d.prologue && !KnownMiddleOfAnInstruction(va)) {
				++into;
				std::printf("  0x%08X jumps to 0x%08X, inside the bytes the %s detour "
				            "overwrites\n",
				            static_cast<unsigned>(va), static_cast<unsigned>(to), d.name);
			}
	}
	Check(into == 0, "nothing in the image jumps into the middle of a prologue CoopIII "
	                 "detours; the trampoline would run half an instruction");
}

} // namespace

int RunPatchSiteTests() {
	g_patchFailures = 0;
	TestNothingOverlaps();
	TestAgainstTheImage();
	return g_patchFailures;
}
