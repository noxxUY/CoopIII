#include "passengeraim.h"

#include "combat.h"
#include "leadcheck.h"
#include "pedanim.h"
#include "../log.h"

#include <windows.h>

#include <cmath>
#include <cstring>

namespace coopiii::game {

namespace {

using ThisFn        = void(__thiscall *)(void *);
using LosFn         = bool(__cdecl *)(const float *, const float *, void *, void **, uint32_t,
                                      uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);
using SphereFn      = void *(__cdecl *)(float, float, float, float, void *, uint32_t, uint32_t,
                                        uint32_t, uint32_t, uint32_t, uint32_t);
using TakeControlFn = void(__thiscall *)(void *, void *, int32_t, int32_t, int32_t);
using TargetVecFn   = void(__thiscall *)(void *, float, float, float, float, float *, float *);
using UpdateFn      = void(__thiscall *)(void *, int32_t);
using CompFn        = void(__thiscall *)(void *, float *, uint32_t);
using SetWeaponFn   = void(__thiscall *)(void *, uint32_t);
using RemoveModelFn = void(__thiscall *)(void *, int32_t);
using GetAssocFn    = void *(__cdecl *)(void *, uint32_t);
using AddAnimFn     = void *(__cdecl *)(void *, int, int);
using InfoFn        = void *(__cdecl *)(int);
using RequestFn     = void(__cdecl *)(int32_t, int32_t);

struct Site {
	uintptr_t   at;
	uintptr_t   calls;
	const char *what;
	bool        taken;
};


// The four calls in CCam::Process_FollowPedWithMouse (addresses.h).
bool __cdecl CamLineOfSight(const float *, const float *, void *, void **, uint32_t, uint32_t,
                            uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);
void *__cdecl CamSphere(float, float, float, float, void *, uint32_t, uint32_t, uint32_t,
                        uint32_t, uint32_t, uint32_t);

Site g_sites[] = {
    {FollowPedMouse_LineOfSightCall1, CWorld__ProcessLineOfSight, "the clip's line", false},
    {FollowPedMouse_LineOfSightCall2, CWorld__ProcessLineOfSight, "the clip past a ped", false},
    {FollowPedMouse_SphereCall1, CWorld__TestSphereAgainstWorld, "the near-clip sphere", false},
    {FollowPedMouse_SphereCall2, CWorld__TestSphereAgainstWorld, "the near-clip loop", false},
};
bool g_installed = false;

// While the camera is ours: the passenger it follows and the car around him.
void *g_camPed = nullptr;
void *g_camCar = nullptr;

bool     g_aiming      = false;
uint32_t g_nextRoundMs = 0;
uint16_t g_pose        = ANIM_NONE;

bool g_saidStarted   = false;
bool g_saidFired     = false;
bool g_saidStolen    = false;
bool g_saidNoMouse   = false;
bool g_saidNoGun     = false;
bool g_saidStreaming = false;

// Points the `call` at `site` at `to`, only while it still calls `from`.
bool RedirectCall(uintptr_t site, uintptr_t from, uintptr_t to) {
	if (!RelCallAt(Ptr<uint8_t>(site), site, from))
		return false;
	DWORD old = 0;
	if (!VirtualProtect(reinterpret_cast<void *>(site), 5, PAGE_EXECUTE_READWRITE, &old))
		return false;
	const int32_t rel = static_cast<int32_t>(to - (site + 5));
	std::memcpy(reinterpret_cast<void *>(site + 1), &rel, sizeof rel);
	VirtualProtect(reinterpret_cast<void *>(site), 5, old, &old);
	FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void *>(site), 5);
	return true;
}

uintptr_t ThunkFor(const Site &s) {
	return s.calls == CWorld__ProcessLineOfSight ? reinterpret_cast<uintptr_t>(&CamLineOfSight)
	                                             : reinterpret_cast<uintptr_t>(&CamSphere);
}

// The camera names the ped it follows as the entity to ignore; aimed at a
// passenger, the one to ignore is his car, and the ped himself is out of every
// line already (bUsesCollision is clear in a seat).
bool __cdecl CamLineOfSight(const float *p1, const float *p2, void *point, void **entity,
                            uint32_t b, uint32_t v, uint32_t p, uint32_t o, uint32_t d,
                            uint32_t s, uint32_t some) {
	void *&ignored     = Global<void *>(CWorld__pIgnoreEntity);
	void *const before = ignored;
	if (g_camCar && before == g_camPed)
		ignored = g_camCar;
	const bool hit =
	    Func<LosFn>(CWorld__ProcessLineOfSight)(p1, p2, point, entity, b, v, p, o, d, s, some);
	ignored = before;
	return hit;
}

// Both of the camera's sphere tests pass no entity to ignore at all.
void *__cdecl CamSphere(float x, float y, float z, float radius, void *ignore, uint32_t b,
                        uint32_t v, uint32_t p, uint32_t o, uint32_t d, uint32_t some) {
	if (g_camCar && !ignore)
		ignore = g_camCar;
	return Func<SphereFn>(CWorld__TestSphereAgainstWorld)(x, y, z, radius, ignore, b, v, p, o, d,
	                                                      some);
}

void *PlayerPed() { return Func<void *(__cdecl *)()>(FindPlayerPed)(); }
void *Camera() { return Ptr<void>(TheCamera); }
void *ClumpOf(void *ped) { return Field<void *>(ped, offs::RW_OBJECT); }

void *Slot(void *ped, uint8_t weapon) {
	return reinterpret_cast<uint8_t *>(ped) + offs::PED_WEAPONS +
	       static_cast<size_t>(weapon) * offs::SIZEOF_WEAPON;
}

void *WeaponInfoOf(uint8_t weapon) { return Func<InfoFn>(CWeaponInfo__GetWeaponInfo)(weapon); }

bool MouseDown(size_t button) {
	return Global<uint8_t>(CPad__NewMouseControllerState + button) != 0;
}

// Bit w for each passenger gun the ped has in its slot with ammunition left.
uint16_t Carried(void *ped) {
	uint16_t carried = 0;
	for (const uint8_t w : {WEAPONTYPE_COLT45, WEAPONTYPE_UZI}) {
		void *const slot = Slot(ped, w);
		if (Field<uint32_t>(slot, offs::WEAPON_TYPE) == w &&
		    Field<int32_t>(slot, offs::WEAPON_AMMO_TOTAL) > 0)
			carried = static_cast<uint16_t>(carried | (1u << w));
	}
	return carried;
}

// The gun in the hand, model and all. Entering a car takes the model off
// anything that is not the uzi (RemoveWeaponWhenEnteringVehicle), and
// SetCurrentWeapon is what puts one back: it removes every weapon atomic and
// adds this one, so it is safe to call over the top of the uzi. The model has
// to be streamed first or AddWeaponModel instances nothing.
bool ArmPassenger(void *ped, uint8_t weapon) {
	const bool held  = Field<uint8_t>(ped, offs::PED_CURRENT_WEAPON) == weapon;
	const bool shown = Field<int32_t>(ped, offs::PED_WEP_MODEL_ID) != -1;
	if (held && shown)
		return true;
	void *const info = WeaponInfoOf(weapon);
	if (!info)
		return false;
	const int32_t model = Field<int32_t>(info, WEAPONINFO_MODEL_ID);
	if (model < 0)
		return false;
	if (!HasModelLoaded(static_cast<uint32_t>(model))) {
		Func<RequestFn>(CStreaming__RequestModel)(model, 0x02);
		if (!g_saidStreaming) {
			g_saidStreaming = true;
			Log("passengeraim: the gun's model %d is not in memory; asked for it, and the aim "
			    "starts once it is", model);
		}
		return false;
	}
	Func<SetWeaponFn>(CPed__SetCurrentWeapon)(ped, weapon);
	return true;
}

// Where the crosshair points, from the hand: CCamera::Find3rdPersonCamTargetVector,
// the call FireInstantHit's mouse branch makes, with the weapon's range and
// the hand as the point it projects onto the camera's line.
bool AimLine(void *ped, float range, Vec3 &hand, Vec3 &dir) {
	float h[3] = {};
	Func<CompFn>(CPedIK__GetComponentPosition)(reinterpret_cast<uint8_t *>(ped) + offs::PED_IK,
	                                           h, PED_NODE_HANDR);
	if (!std::isfinite(h[0]) || !std::isfinite(h[1]) || !std::isfinite(h[2]))
		return false;
	float src[3] = {}, trgt[3] = {};
	Func<TargetVecFn>(CCamera__Find3rdPersonCamTargetVector)(Camera(), range, h[0], h[1], h[2],
	                                                         src, trgt);
	hand = Vec3{h[0], h[1], h[2]};
	return UnitDirection(Vec3{trgt[0] - h[0], trgt[1] - h[1], trgt[2] - h[2]}, dir);
}

// DoDriveByShootings' three anim writes (addresses.h, "the drive-by"), for the
// window that faces the aim.
void HoldPose(void *ped, void *car, const Vec3 &dir) {
	void *const clump = ClumpOf(ped);
	if (!clump)
		return;
	const uint16_t want = DriveByAnimForShot(dir, ReadVec3(car, offs::MATRIX_RIGHT));
	if (void *const other = Func<GetAssocFn>(RpAnimBlendClumpGetAssociation)(
	        clump, DriveByOtherSide(want)))
		Field<float>(other, ANIM_BLEND_DELTA) = DRIVEBY_ANIM_DROP_DELTA;
	void *const assoc = Func<GetAssocFn>(RpAnimBlendClumpGetAssociation)(clump, want);
	if (assoc && DriveByHeld(true, Field<float>(assoc, ANIM_BLEND_DELTA)))
		Field<int32_t>(assoc, ANIM_FLAGS) |= ASSOC_RUNNING;
	else
		Func<AddAnimFn>(CAnimManager__AddAnimation)(clump, ASSOCGRP_STD, static_cast<int>(want));
	g_pose = want;
}

void DropPose(void *ped) {
	void *const clump = ped ? ClumpOf(ped) : nullptr;
	if (clump)
		for (const uint16_t id : {ANIM_STD_CAR_DRIVEBY_LEFT, ANIM_STD_CAR_DRIVEBY_RIGHT})
			if (void *const assoc = Func<GetAssocFn>(RpAnimBlendClumpGetAssociation)(clump, id))
				Field<float>(assoc, ANIM_BLEND_DELTA) = DRIVEBY_ANIM_DROP_DELTA;
	g_pose = ANIM_NONE;
}

bool CameraIsOurs(void *ped) {
	void *const camera = Camera();
	return AimCameraStillOurs(Field<int32_t>(camera, offs::CAMERA_WHO_CONTROLS),
	                          Field<void *>(camera, offs::CAMERA_TARGET), ped);
}

// Everything the aim changed, put back. The camera only while it is still the
// one we took, and the gun put away the way a seat keeps it (only the uzi
// shows), only while he is still in one.
void EndAim(void *ped) {
	if (!g_aiming)
		return;
	DropPose(ped);
	if (g_camPed && CameraIsOurs(g_camPed))
		Func<ThisFn>(CCamera__RestoreWithJumpCut)(Camera());
	if (ped && ped == g_camPed && Field<bool>(ped, offs::PED_IN_VEHICLE) &&
	    Field<uint8_t>(ped, offs::PED_CURRENT_WEAPON) != WEAPONTYPE_UZI &&
	    Field<int32_t>(ped, offs::PED_WEP_MODEL_ID) != -1)
		Func<RemoveModelFn>(CPed__RemoveWeaponModel)(ped, Field<int32_t>(ped, offs::PED_WEP_MODEL_ID));
	g_camPed = g_camCar = nullptr;
	g_aiming = false;
}

void SayGate(AimGate gate) {
	if (gate == AimGate::NoMouseCamera && !g_saidNoMouse) {
		g_saidNoMouse = true;
		Log("passengeraim: the aim key was held in a passenger seat, and the game is not on "
		    "the mouse camera (CCamera::m_bUseMouse3rdPerson is off), which has the only "
		    "crosshair there is; no aim");
	} else if (gate == AimGate::NoGun && !g_saidNoGun) {
		g_saidNoGun = true;
		Log("passengeraim: the aim key was held in a passenger seat with no pistol or uzi "
		    "that has ammunition; a passenger fires only those two");
	}
}

void FireIfDue(void *ped, void *car, uint8_t weapon, const Vec3 &hand, const Vec3 &dir) {
	const uint32_t now = Global<uint32_t>(CTimer__m_snTimeInMilliseconds);
	if (now < g_nextRoundMs)
		return;
	void *const slot  = Slot(ped, weapon);
	const uint32_t st = Field<uint32_t>(slot, offs::WEAPON_STATE);
	if ((st != WEAPONSTATE_READY && st != WEAPONSTATE_FIRING) ||
	    Field<int32_t>(slot, offs::WEAPON_AMMO_IN_CLIP) <= 0)
		return;
	void *const info = WeaponInfoOf(weapon);
	if (!info)
		return;

	const Vec3 muzzle    = PassengerMuzzle(hand, dir);
	float      source[3] = {muzzle.x, muzzle.y, muzzle.z};
	const bool fired     = FirePassengerRound(ped, slot, car, source, dir);
	g_nextRoundMs        = now + PassengerRoundIntervalMs(
	                          weapon, Field<float>(info, WEAPONINFO_ANIM_LOOP_START),
	                          Field<float>(info, WEAPONINFO_ANIM_LOOP_END));
	if (fired && !g_saidFired) {
		g_saidFired = true;
		Log("passengeraim: our first round from a passenger seat, weapon %u, from (%.1f %.1f "
		    "%.1f) along (%.2f %.2f %.2f), our car left out of its line",
		    weapon, muzzle.x, muzzle.y, muzzle.z, dir.x, dir.y, dir.z);
	}
}

} // namespace

bool InstallPassengerAim() {
	if (g_installed)
		return true;
	bool all = true;
	for (const Site &s : g_sites)
		all = all && RelCallAt(Ptr<uint8_t>(s.at), s.at, s.calls);
	if (!all) {
		Log("passengeraim: FAILED - the mouse camera's calls at 0x%08X..0x%08X are not what the "
		    "retail image has there, so a passenger's camera would clip into his own car; the "
		    "passenger's aim is off",
		    static_cast<unsigned>(FollowPedMouse_LineOfSightCall1),
		    static_cast<unsigned>(FollowPedMouse_SphereCall2));
		return false;
	}
	for (Site &s : g_sites)
		s.taken = RedirectCall(s.at, s.calls, ThunkFor(s));
	for (const Site &s : g_sites)
		all = all && s.taken;
	if (!all) {
		RemovePassengerAim();
		Log("passengeraim: FAILED to take the mouse camera's calls; the passenger's aim is off");
		return false;
	}
	g_installed = true;
	Log("passengeraim: the mouse camera's four collision calls come to us; a passenger holding "
	    "the right button aims out of any window with his pistol or uzi, and the left fires");
	return true;
}

void RemovePassengerAim() {
	if (g_aiming)
		EndAim(PlayerPed());
	for (Site &s : g_sites)
		if (s.taken && RedirectCall(s.at, ThunkFor(s), s.calls))
			s.taken = false;
	g_installed = false;
}

void PassengerAimBeforeCamera() {
	if (!g_installed)
		return;

	void *const ped = PlayerPed();
	void *const car = ped && Field<bool>(ped, offs::PED_IN_VEHICLE)
	                      ? Field<void *>(ped, offs::PED_MY_VEHICLE)
	                      : nullptr;
	const bool passenger = car && Field<void *>(car, offs::VEH_DRIVER) != ped &&
	                       Field<uint32_t>(ped, offs::PED_STATE) == PEDSTATE_DRIVING &&
	                       Field<int32_t>(car, offs::VEH_TYPE) != VEHICLE_TYPE_TRAIN;
	if (!passenger) {
		EndAim(ped);
		return;
	}

	// Nothing in the engine updates a seated ped's gun but DoDriveByShootings,
	// which is the driver's; without this a reload never ends.
	const uint8_t current = Field<uint8_t>(ped, offs::PED_CURRENT_WEAPON);
	if (current < INVENTORY_SLOTS)
		Func<UpdateFn>(CWeapon__Update)(Slot(ped, current),
		                                Field<int32_t>(ped, offs::PED_AUDIO_ENTITY_ID));

	if (!MouseDown(pad::MOUSE_RMB)) {
		EndAim(ped);
		return;
	}

	// A script or the unique jump's shot took the camera while we aimed. It
	// is theirs to give back.
	if (g_aiming && !CameraIsOurs(ped)) {
		if (!g_saidStolen) {
			g_saidStolen = true;
			Log("passengeraim: something else took the camera while we aimed from a passenger "
			    "seat; the aim ends and the camera stays theirs");
		}
		EndAim(ped);
		return;
	}

	const uint8_t weapon = PassengerWeaponToUse(current, Carried(ped));
	void *const   camera = Camera();
	const AimGate gate   = PassengerAimGate(
        true, weapon != WEAPONTYPE_UNARMED, Global<uint8_t>(CCamera__m_bUseMouse3rdPerson) != 0,
        Field<int32_t>(camera, offs::CAMERA_WHO_CONTROLS), g_aiming,
        Field<uint8_t>(camera, CAMERA_WIDESCREEN_ON) != 0,
        Field<uint8_t>(Ptr<void>(CPad__Pads), pad::DISABLE_PLAYER_CONTROLS) != 0);
	if (gate != AimGate::Go) {
		SayGate(gate);
		EndAim(ped);
		return;
	}

	if (!ArmPassenger(ped, weapon))
		return;   // streaming; next frame

	if (!g_aiming) {
		Func<TakeControlFn>(CCamera__TakeControl)(camera, ped, CAM_MODE_FOLLOWPED,
		                                          CAM_SWITCH_JUMP_CUT, CAMCONTROL_SCRIPT);
		g_camPed      = ped;
		g_camCar      = car;
		g_aiming      = true;
		g_nextRoundMs = 0;
		if (!g_saidStarted) {
			g_saidStarted = true;
			Log("passengeraim: aiming from a passenger seat with weapon %u; the camera is the "
			    "on-foot mouse camera around us, and gives the car camera back on release",
			    weapon);
		}
	}
	g_camCar = car;

	void *const info  = WeaponInfoOf(weapon);
	const float range = info ? Field<float>(info, WEAPONINFO_RANGE) : 0.0f;
	Vec3        hand, dir;
	if (!(range > 0.0f) || !AimLine(ped, range, hand, dir))
		return;
	HoldPose(ped, car, dir);
	if (MouseDown(pad::MOUSE_LMB))
		FireIfDue(ped, car, weapon, hand, dir);
}

} // namespace coopiii::game
