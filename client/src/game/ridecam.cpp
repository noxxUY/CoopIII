#include "ridecam.h"

#include "addresses.h"
#include "leadcheck.h"
#include "passengeraim.h"
#include "teardown.h"
#include "../client.h"
#include "../log.h"

#include <cstring>

#include <windows.h>

namespace coopiii::game {

namespace {

using ThisFn        = void(__thiscall *)(void *);
using TakeControlFn = void(__thiscall *)(void *camera, void *target, int32_t mode, int32_t swap,
                                         int32_t controller);
using FixedModeFn   = void(__thiscall *)(void *camera, const Vec3 *from, const Vec3 *up);
using VehicleRefFn  = int32_t(__cdecl *)(void *);

Client *g_client       = nullptr;
bool    g_cameraCall   = false;   // 0x0048C9B5 comes to CameraProcess
bool    g_takeControl  = false;   // 0x0043F5A8 comes to TakeControlForScript
bool    g_restoreCall  = false;   // 0x00446EB2 comes to RestoreJumpCut
bool    g_saidThrew    = false;
bool    g_saidSeated   = false;
bool    g_saidAimThrew = false;

// The car our camera was pointed at for a driver's jump, while it is.
void *g_shotCar = nullptr;

void *Camera() { return Ptr<void>(TheCamera); }

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

// ---- the frame's camera, after the car it looks at -------------------------------

// 0x0048C9B5, `mov ecx,6FACF8h / call CCamera::Process`. __thiscall with no
// arguments is __fastcall with a spare edx.
void __fastcall CameraProcess(void *camera, void * /*edx*/) {
	if (g_client) {
		// A throw would unwind through the engine; a frame whose camera
		// follows the car one frame late is the lesser harm.
		try {
			g_client->BeforeCamera();
		} catch (...) {
			if (!g_saidThrew) {
				g_saidThrew = true;
				Log("ridecam: the correction before the camera threw; the camera runs as it "
				    "did (said once)");
			}
		}
	}
	// A passenger's aim, after his car and its occupants are where this frame
	// draws them and before the camera looks: the camera it hands him, the
	// pose and the rounds (passengeraim.h).
	try {
		PassengerAimBeforeCamera();
	} catch (...) {
		if (!g_saidAimThrew) {
			g_saidAimThrew = true;
			Log("ridecam: the passenger's aim threw; the camera runs as it did (said once)");
		}
	}
	Func<ThisFn>(CCamera__Process)(camera);
}

// CWorld::Process's arm for a seated ped (0x004B1EE2..0x004B1EF1), for the
// peds in one car: only those it would have sent there, PED_DRIVING and
// sitting in this car.
void SeatOccupants(RemoteVehicle &vehicle) {
	void *const car = VehicleAt(vehicle.poolHandle);
	if (!car || Field<int32_t>(car, offs::VEH_TYPE) == VEHICLE_TYPE_TRAIN)
		return;
	uint8_t seats = Field<uint8_t>(car, offs::VEH_NUM_MAX_PASSENGERS);
	if (seats > offs::VEH_MAX_PASSENGERS)
		seats = offs::VEH_MAX_PASSENGERS;
	for (size_t i = 0; i <= seats; ++i) {
		void *const ped = i == 0 ? Field<void *>(car, offs::VEH_DRIVER)
		                         : Field<void *>(car, offs::VEH_PASSENGERS + (i - 1) * sizeof(void *));
		if (!ped || !Field<bool>(ped, offs::PED_IN_VEHICLE) ||
		    Field<void *>(ped, offs::PED_MY_VEHICLE) != car ||
		    Field<uint32_t>(ped, offs::PED_STATE) != PEDSTATE_DRIVING)
			continue;
		Func<ThisFn>(CPed__SetPedPositionInCar)(ped);
		Func<ThisFn>(CMatrix__UpdateRW)(reinterpret_cast<uint8_t *>(ped) + offs::MATRIX);
		Func<ThisFn>(CEntity__UpdateRwFrame)(ped);
	}
	if (!g_saidSeated) {
		g_saidSeated = true;
		Log("ridecam: sat the people in vehicle %u back on it after moving it before the "
		    "camera (said once)",
		    vehicle.netId);
	}
}

// ---- the unique jump's shot ------------------------------------------------------

// The driver's half. POINT_CAMERA_AT_CAR hands TakeControl the car, the mode,
// the switch and CAMCONTROL_SCRIPT, and keeps its script in ebp
// (addresses.h). `camera` in ecx, the script in edx, the four arguments where
// the handler pushed them: __fastcall, callee-cleaned like TakeControl's
// `ret 10h`.
void __fastcall TakeControlFromScript(void *camera, void *script, void *target, int32_t mode,
                                      int32_t swap, int32_t controller) {
	Func<TakeControlFn>(CCamera__TakeControl)(camera, target, mode, swap, controller);
	if (!g_client || !target || !script)
		return;
	if (!IsStuntShotThread(reinterpret_cast<const char *>(script) + offs::SCRIPT_NAME))
		return;
	const Vec3 from = Field<Vec3>(camera, offs::CAMERA_FIXED_SOURCE);
	const Vec3 up   = Field<Vec3>(camera, offs::CAMERA_FIXED_UP);
	g_client->OwnStuntShot(Func<VehicleRefFn>(CPools__GetVehicleRef)(target), from, up,
	                       static_cast<uint8_t>(mode & 0xFF), static_cast<uint8_t>(swap & 0xFF));
}

// The call site's ebp is the script; this hands it on in edx and jumps, so
// the return address and the four arguments stay where the handler put them.
__declspec(naked) void TakeControlForScript() {
	__asm {
		mov edx, ebp
		jmp TakeControlFromScript
	}
}

// 0x00446EB2, RESTORE_CAMERA_JUMPCUT's only call. Whatever script restores
// the camera ends a shot we told the riders about: ours is the only one that
// could be up.
void __fastcall RestoreJumpCut(void *camera, void * /*edx*/) {
	Func<ThisFn>(CCamera__RestoreWithJumpCut)(camera);
	if (g_client)
		g_client->OwnStuntShotOver();
}

// The rider's half: the same two calls SET_FIXED_CAMERA_POSITION and
// POINT_CAMERA_AT_CAR make, on our copy of the car. Not while a script of
// ours holds the camera, whose shot this would take away.
bool ShowStuntShot(RemoteVehicle &vehicle, const Vec3 &from, const Vec3 &up, uint8_t mode,
                   uint8_t swap) {
	void *const camera = Camera();
	void *const car    = VehicleAt(vehicle.poolHandle);
	if (!car || Field<int32_t>(camera, offs::CAMERA_WHO_CONTROLS) != CAMCONTROL_NOBODY)
		return false;
	Func<FixedModeFn>(CCamera__SetCamPositionForFixedMode)(camera, &from, &up);
	Func<TakeControlFn>(CCamera__TakeControl)(camera, car, mode, swap, CAMCONTROL_SCRIPT);
	g_shotCar = car;
	return true;
}

// Given back only while it is still ours: pointed at that car by a script.
// RestoreWithJumpCut leaves the target on the player's car too, so the
// controller is what tells a camera somebody else has already restored.
void EndStuntShot() {
	void *const camera = Camera();
	const bool  ours   = g_shotCar != nullptr &&
	                  Field<int32_t>(camera, offs::CAMERA_WHO_CONTROLS) == CAMCONTROL_SCRIPT &&
	                  Field<void *>(camera, offs::CAMERA_TARGET) == g_shotCar;
	g_shotCar = nullptr;
	if (ours)
		Func<ThisFn>(CCamera__RestoreWithJumpCut)(camera);
}

} // namespace

bool InstallRideCamera(Client &client) {
	g_client = &client;
	if (!g_cameraCall)
		g_cameraCall = RedirectCall(CGame__Process_CameraCall, CCamera__Process,
		                            reinterpret_cast<uintptr_t>(&CameraProcess));
	if (g_cameraCall)
		Log("ridecam: CGame::Process's call to the camera at 0x%08X comes to us; the car we "
		    "ride in is corrected before the camera follows it",
		    static_cast<unsigned>(CGame__Process_CameraCall));
	else
		Log("ridecam: FAILED to take the camera call at 0x%08X; a rider's camera follows his "
		    "car a frame late",
		    static_cast<unsigned>(CGame__Process_CameraCall));

	if (!g_takeControl)
		g_takeControl = RedirectCall(POINT_CAMERA_AT_CAR_TakeControlCall, CCamera__TakeControl,
		                             reinterpret_cast<uintptr_t>(&TakeControlForScript));
	if (!g_restoreCall)
		g_restoreCall = RedirectCall(RESTORE_CAMERA_JUMPCUT_Call, CCamera__RestoreWithJumpCut,
		                             reinterpret_cast<uintptr_t>(&RestoreJumpCut));
	if (g_takeControl && g_restoreCall)
		Log("ridecam: POINT_CAMERA_AT_CAR and RESTORE_CAMERA_JUMPCUT come to us at 0x%08X and "
		    "0x%08X; a unique jump's shot reaches the players riding with us",
		    static_cast<unsigned>(POINT_CAMERA_AT_CAR_TakeControlCall),
		    static_cast<unsigned>(RESTORE_CAMERA_JUMPCUT_Call));
	else
		Log("ridecam: FAILED to take %s; the players riding with us see our unique jumps "
		    "from behind",
		    !g_takeControl ? "POINT_CAMERA_AT_CAR's TakeControl" : "RESTORE_CAMERA_JUMPCUT's call");
	return g_cameraCall;
}

void RemoveRideCamera() {
	if (g_restoreCall &&
	    RedirectCall(RESTORE_CAMERA_JUMPCUT_Call, reinterpret_cast<uintptr_t>(&RestoreJumpCut),
	                 CCamera__RestoreWithJumpCut))
		g_restoreCall = false;
	if (g_takeControl &&
	    RedirectCall(POINT_CAMERA_AT_CAR_TakeControlCall,
	                 reinterpret_cast<uintptr_t>(&TakeControlForScript), CCamera__TakeControl))
		g_takeControl = false;
	if (g_cameraCall &&
	    RedirectCall(CGame__Process_CameraCall, reinterpret_cast<uintptr_t>(&CameraProcess),
	                 CCamera__Process))
		g_cameraCall = false;
	g_client = nullptr;
}

bool RideCameraOrderTaken() { return g_cameraCall; }

void AddRideCameraToBridge(WorldBridge &bridge) {
	bridge.SeatOccupants = &SeatOccupants;
	bridge.ShowStuntShot = &ShowStuntShot;
	bridge.EndStuntShot  = &EndStuntShot;
}

} // namespace coopiii::game
