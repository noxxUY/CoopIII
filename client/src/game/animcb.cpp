// game/animcb.h has the why; addresses.h, "the car chain's callbacks", the
// bytes.
#include "animcb.h"

#include "ped.h"
#include "../log.h"

#include <cstring>

#include <windows.h>

namespace coopiii::game {

namespace {

using CallbackFn = void(__cdecl *)(void *, void *);

bool g_saidRollingSkipped = false;
bool g_saidTrainSkipped   = false;

// What the engine's own callbacks do to their association first, and all that
// is left to do when the car is gone: fade it out now.
void FadeNow(void *assoc) {
	if (!assoc)
		return;
	Field<float>(assoc, ANIM_BLEND_DELTA) = -1000.0f;
	Field<int32_t>(assoc, ANIM_FLAGS) |= ASSOC_DELETEFADEDOUT;
}

bool CarOf(void *ped, bool &automobile) {
	void *const car = ped ? Field<void *>(ped, offs::PED_MY_VEHICLE) : nullptr;
	automobile      = car && Field<uintptr_t>(car, offs::VTABLE) == CAutomobile__vtable;
	return car != nullptr;
}

void __cdecl RollingDoorGuard(void *assoc, void *ped) {
	bool       automobile = false;
	const bool haveCar    = CarOf(ped, automobile);
	if (GuardedCarCallbackRuns(CPed__PedAnimDoorCloseRollingCB, haveCar, automobile)) {
		Func<CallbackFn>(CPed__PedAnimDoorCloseRollingCB)(assoc, ped);
		return;
	}
	FadeNow(assoc);
	if (!g_saidRollingSkipped) {
		g_saidRollingSkipped = true;
		Log("animcb: a rolling door close finished on a ped with %s; faded it out "
		    "instead of closing a door on nothing (said once)",
		    haveCar ? "a car that is not a CAutomobile" : "no car");
	}
}

void __cdecl OutTrainGuard(void *assoc, void *ped) {
	bool       automobile = false;
	const bool haveCar    = CarOf(ped, automobile);
	if (GuardedCarCallbackRuns(CPed__PedSetOutTrainCB, haveCar, automobile)) {
		Func<CallbackFn>(CPed__PedSetOutTrainCB)(assoc, ped);
		return;
	}
	FadeNow(assoc);
	if (!g_saidTrainSkipped) {
		g_saidTrainSkipped = true;
		Log("animcb: a train get-out finished on a ped with no train; faded it out "
		    "(said once)");
	}
}

uintptr_t GuardFor(uintptr_t original) {
	return original == CPed__PedAnimDoorCloseRollingCB
	           ? reinterpret_cast<uintptr_t>(&RollingDoorGuard)
	           : reinterpret_cast<uintptr_t>(&OutTrainGuard);
}

// Swaps one dword, only while it still holds `from`.
bool SwapDword(uintptr_t at, uint32_t from, uint32_t to) {
	if (ReadDword(Ptr<uint8_t>(at)) != from)
		return false;
	DWORD old = 0;
	if (!VirtualProtect(reinterpret_cast<void *>(at), 4, PAGE_EXECUTE_READWRITE, &old))
		return false;
	std::memcpy(reinterpret_cast<void *>(at), &to, sizeof to);
	VirtualProtect(reinterpret_cast<void *>(at), 4, old, &old);
	FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void *>(at), 4);
	return true;
}

uintptr_t SlotAt(int slot) {
	return CReplay__CBArray + static_cast<uintptr_t>(slot) * sizeof(uint32_t);
}

bool g_pushDone[sizeof CAR_CALLBACK_GUARD_SITES / sizeof CAR_CALLBACK_GUARD_SITES[0]] = {};
bool g_slotDone[sizeof CAR_CALLBACK_GUARD_SITES / sizeof CAR_CALLBACK_GUARD_SITES[0]] = {};

} // namespace

uintptr_t RollingDoorGuardAddress() { return reinterpret_cast<uintptr_t>(&RollingDoorGuard); }
uintptr_t OutTrainGuardAddress() { return reinterpret_cast<uintptr_t>(&OutTrainGuard); }

bool InstallCarCallbackGuards() {
	bool ok = true;
	for (size_t i = 0; i < sizeof CAR_CALLBACK_GUARD_SITES / sizeof CAR_CALLBACK_GUARD_SITES[0];
	     ++i) {
		const CarCallbackGuardSite &s     = CAR_CALLBACK_GUARD_SITES[i];
		const uint32_t              guard = static_cast<uint32_t>(GuardFor(s.original));
		if (!g_pushDone[i]) {
			// The push's operand, never the function: the callback's own code
			// is the engine's, and so is every other way of reaching it.
			g_pushDone[i] = PushesImm(Ptr<uint8_t>(s.push), s.original) &&
			                SwapDword(s.push + 1, static_cast<uint32_t>(s.original), guard);
			if (g_pushDone[i])
				Log("animcb: the callback pushed at 0x%08X (0x%08X) goes through a test for "
				    "a missing car now",
				    static_cast<unsigned>(s.push), static_cast<unsigned>(s.original));
			else
				Log("animcb: FAILED to guard the callback pushed at 0x%08X; it is not "
				    "`push 0x%08X` any more",
				    static_cast<unsigned>(s.push), static_cast<unsigned>(s.original));
		}
		// And its slot in the replay's table, so a replay that records the
		// animation names the wrapper and plays the wrapper back.
		if (!g_slotDone[i])
			g_slotDone[i] = SwapDword(SlotAt(s.replaySlot), static_cast<uint32_t>(s.original),
			                          guard);
		ok &= g_pushDone[i] && g_slotDone[i];
	}
	return ok;
}

void RemoveCarCallbackGuards() {
	// Whatever animation still names a wrapper goes back to the engine's own
	// function first: the wrappers leave with this module.
	RepointAnimCallbacks(RollingDoorGuardAddress(), CPed__PedAnimDoorCloseRollingCB);
	RepointAnimCallbacks(OutTrainGuardAddress(), CPed__PedSetOutTrainCB);
	for (size_t i = 0; i < sizeof CAR_CALLBACK_GUARD_SITES / sizeof CAR_CALLBACK_GUARD_SITES[0];
	     ++i) {
		const CarCallbackGuardSite &s     = CAR_CALLBACK_GUARD_SITES[i];
		const uint32_t              guard = static_cast<uint32_t>(GuardFor(s.original));
		if (g_pushDone[i] && SwapDword(s.push + 1, guard, static_cast<uint32_t>(s.original)))
			g_pushDone[i] = false;
		if (g_slotDone[i] &&
		    SwapDword(SlotAt(s.replaySlot), guard, static_cast<uint32_t>(s.original)))
			g_slotDone[i] = false;
	}
}

} // namespace coopiii::game
