#include "carcam.h"

#include "addresses.h"
#include "leadcheck.h"
#include "../log.h"

#include <windows.h>

#include <cstring>

namespace coopiii::game {

namespace {

using CamFn = void(__thiscall *)(void *cam, const void *target, uint32_t orientation, uint32_t a,
                                 uint32_t b);
using PedFn = void *(__cdecl *)();

// What the car camera call led to before we took it: the mod's function.
uintptr_t g_next     = 0;
bool      g_taken    = false;
bool      g_modFound = false;
bool      g_saidKept = false;

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

uintptr_t CallTarget(uintptr_t site) {
	int32_t rel = 0;
	std::memcpy(&rel, reinterpret_cast<const void *>(site + 1), sizeof rel);
	return site + 5 + static_cast<uintptr_t>(static_cast<int64_t>(rel));
}

HMODULE ModuleOf(uintptr_t at) {
	HMODULE mod = nullptr;
	if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
	                            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
	                        reinterpret_cast<LPCSTR>(at), &mod))
		return nullptr;
	return mod;
}

CodeHome HomeOf(uintptr_t at) {
	const HMODULE mod = ModuleOf(at);
	if (!mod)
		return CodeHome::Nowhere;
	return mod == GetModuleHandleA(nullptr) ? CodeHome::Exe : CodeHome::OtherModule;
}

// The file name alone, for the log.
void NameOf(uintptr_t at, char *out, size_t size) {
	std::strncpy(out, "?", size);
	const HMODULE mod = ModuleOf(at);
	char          path[MAX_PATH] = {};
	if (!mod || !GetModuleFileNameA(mod, path, sizeof path))
		return;
	const char *name = std::strrchr(path, '\\');
	std::strncpy(out, name ? name + 1 : path, size - 1);
	out[size - 1] = '\0';
}

struct Reading {
	CarCamOwner owner  = CarCamOwner::Unknown;
	uintptr_t   target = 0;
};

Reading Read(uintptr_t site, uintptr_t retail) {
	Reading r;
	const uint8_t op = Global<uint8_t>(site);
	if (op == 0xE8)
		r.target = CallTarget(site);
	r.owner = WhoOwnsCarCamCall(op, r.target, retail, r.target ? HomeOf(r.target) : CodeHome::Nowhere);
	return r;
}

void Say(const char *what, uintptr_t site, const Reading &r) {
	if (r.owner == CarCamOwner::Retail)
		return;
	if (r.owner == CarCamOwner::Mod) {
		char name[64];
		NameOf(r.target, name, sizeof name);
		Log("carcam: the %s call at 0x%08X leads into %s (0x%08X), not gta3.exe: a car "
		    "camera mod is installed",
		    what, static_cast<unsigned>(site), name, static_cast<unsigned>(r.target));
		return;
	}
	Log("carcam: the %s call at 0x%08X is not the retail call and leads nowhere we can name "
	    "(byte %02X, 0x%08X); left alone",
	    what, static_cast<unsigned>(site), static_cast<unsigned>(Global<uint8_t>(site)),
	    static_cast<unsigned>(r.target));
}

// The car the camera follows, when its gun is not ours to turn.
void *GunToKeep(void *cam) {
	void *const car = Field<void *>(cam, CAM_TARGET_ENTITY);
	if (!car || (Field<uint8_t>(car, offs::ENTITY_FLAGS) & 0x07u) != ENTITY_TYPE_VEHICLE)
		return nullptr;
	const bool  automobile = Field<int32_t>(car, offs::VEH_TYPE) == VEHICLE_TYPE_CAR;
	void *const ped        = Func<PedFn>(FindPlayerPed)();
	const bool  drives     = ped != nullptr && Field<void *>(car, offs::VEH_DRIVER) == ped;
	const auto  model      = static_cast<uint16_t>(Field<int16_t>(car, offs::MODEL_INDEX));
	return CarCameraKeepsOffGun(automobile, model, drives) ? car : nullptr;
}

// CCam::Process's call for MODE_CAM_ON_A_STRING. __thiscall with four dwords
// is __fastcall with a spare edx, callee-cleaned like the callee's `ret 10h`.
// The dwords go on untouched: the three that are floats never pass through
// the FPU here.
void __fastcall CarCameraCall(void *cam, void * /*edx*/, const void *target,
                              uint32_t orientation, uint32_t a, uint32_t b) {
	void *const car = GunToKeep(cam);
	if (!car) {
		Func<CamFn>(g_next)(cam, target, orientation, a, b);
		return;
	}
	float &lr         = Field<float>(car, offs::AUTO_GUN_LR);
	float &ud         = Field<float>(car, offs::AUTO_GUN_UD);
	int32_t &audio    = Field<int32_t>(car, offs::PHYSICAL_AUDIO_ENTITY);
	const float   lr0 = lr, ud0 = ud;
	const int32_t audio0 = audio;
	audio                = -1;
	Func<CamFn>(g_next)(cam, target, orientation, a, b);
	audio = audio0;
	if (lr != lr0 || ud != ud0) {
		lr = lr0;
		ud = ud0;
		if (!g_saidKept) {
			g_saidKept = true;
			Log("carcam: the car camera turned the gun of a car we do not drive; put back, "
			    "and kept quiet, since that gun is its driver's (said once)");
		}
	}
}

} // namespace

bool InstallCarCamera() {
	const Reading onString = Read(CCam__Process_CamOnAStringCall, CCam__Process_Cam_On_A_String);
	const Reading boat     = Read(CCam__Process_BehindBoatCall, CCam__Process_BehindBoat);
	Say("car camera", CCam__Process_CamOnAStringCall, onString);
	Say("boat camera", CCam__Process_BehindBoatCall, boat);
	if (Global<uint8_t>(CCam__WellBufferMe) == 0xE9)
		Log("carcam: WellBufferMe (0x%08X) jumps elsewhere too, the way SACarCam replaces it",
		    static_cast<unsigned>(CCam__WellBufferMe));

	g_modFound = onString.owner == CarCamOwner::Mod || boat.owner == CarCamOwner::Mod;
	if (onString.owner != CarCamOwner::Mod) {
		if (onString.owner == CarCamOwner::Retail && boat.owner == CarCamOwner::Retail)
			Log("carcam: the car camera is the game's own");
		return onString.owner != CarCamOwner::Unknown;
	}
	if (g_taken)
		return true;
	g_next  = onString.target;
	g_taken = RedirectCall(CCam__Process_CamOnAStringCall, g_next,
	                       reinterpret_cast<uintptr_t>(&CarCameraCall));
	if (g_taken)
		Log("carcam: the car camera call comes to us and goes on to the mod; it keeps off "
		    "the gun of a tank or fire truck somebody else drives");
	else
		Log("carcam: FAILED to take the car camera call at 0x%08X; a rider in somebody's tank "
		    "hears its turret whenever he looks away from where it points",
		    static_cast<unsigned>(CCam__Process_CamOnAStringCall));
	return g_taken;
}

void RemoveCarCamera() {
	if (g_taken && RedirectCall(CCam__Process_CamOnAStringCall,
	                            reinterpret_cast<uintptr_t>(&CarCameraCall), g_next))
		g_taken = false;
}

bool CarCameraModInstalled() { return g_modFound; }

} // namespace coopiii::game
