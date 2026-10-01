// A ped's car animations, when CoopIII changes his seat by hand.
//
// The engine gets a ped into and out of a car through a chain of animations,
// each one's finish callback starting the next, and every callback reads the
// ped's m_pMyVehicle when it fires (addresses.h, "the car chain's callbacks").
// CoopIII also seats and unseats peds directly - the seat key, the warp the
// door falls back to, a remote player's or a replica's seat, a jack handed
// over - and every one of those writes m_pMyVehicle, or nils it, while such a
// callback may still be on its way. Fading the animation does not stop it:
// UpdateBlend calls it as it deletes a faded association.
//
// So before any of those writes, the ped's own car animations with a
// callback still to come have it taken off (its kind set to CB_NONE) and are
// faded out at once. The one whose end the
// car still needs - the rolling door close, which gives the car its front
// left door back - is finished there and then instead, on the car it was
// started for. ped.h, DropCarChainCallbacks.
//
// And behind that, the two callbacks with no test of their own for a car
// that has gone are handed to SetFinishCallback as a wrapper that has one:
// the push of each and its slot in the replay's table are pointed at it. Their
// code is not touched.
//
// The rules are pure so tools/clienttest checks them without a game.
#pragma once

#include "addresses.h"

#include <cstddef>
#include <cstdint>

namespace coopiii::game {

constexpr uintptr_t CAR_CHAIN_CALLBACKS[] = {
    CPed__PedAnimAlignCB,
    CPed__PedAnimDoorOpenCB,
    CPed__PedAnimPullPedOutCB,
    CPed__PedAnimGetInCB,
    CPed__PedAnimDoorCloseCB,
    CPed__PedSetInCarCB,
    CPed__PedSetOutCarCB,
    CPed__PedAnimStepOutCarCB,
    CPed__PedSetDraggedOutCarCB,
    CPed__PedSetQuickDraggedOutCarPositionCB,
    CPed__PedSetDraggedOutCarPositionCB,
    CPed__PedSetInTrainCB,
    CPed__PedSetOutTrainCB,
    CPed__PedAnimDoorCloseRollingCB,
};

inline bool IsCarChainCallback(uintptr_t fn) {
	for (uintptr_t c : CAR_CHAIN_CALLBACKS)
		if (c == fn)
			return true;
	return false;
}

// The two that read the car with no test (addresses.h).
inline bool CarCallbackReadsCarUntested(uintptr_t fn) {
	return fn == CPed__PedAnimDoorCloseRollingCB || fn == CPed__PedSetOutTrainCB;
}

// A push of one of those two, and the replay slot that names it.
struct CarCallbackGuardSite {
	uintptr_t push;       // `68 imm32`
	uintptr_t original;   // the imm32 in the retail image
	int       replaySlot;
};

constexpr CarCallbackGuardSite CAR_CALLBACK_GUARD_SITES[] = {
    {CPlayerPed__ProcessControl_RollingDoorCBPush, CPed__PedAnimDoorCloseRollingCB,
     REPLAY_CB_ROLLING_DOOR},
    {CPed__SetExitTrain_OutTrainCBPush, CPed__PedSetOutTrainCB, REPLAY_CB_OUT_TRAIN},
};

inline uint32_t ReadDword(const uint8_t *p) {
	return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}

// `push imm32` of `value` at `insn`.
inline bool PushesImm(const uint8_t *insn, uintptr_t value) {
	return insn[0] == 0x68 && ReadDword(insn + 1) == static_cast<uint32_t>(value);
}

// The engine's callback a pointer on an association stands for: itself, or
// the one a guard wrapper stands in for. 0 when `fn` is neither.
inline uintptr_t EngineCarCallback(uintptr_t fn, uintptr_t rollingGuard, uintptr_t trainGuard) {
	if (fn != 0 && fn == rollingGuard)
		return CPed__PedAnimDoorCloseRollingCB;
	if (fn != 0 && fn == trainGuard)
		return CPed__PedSetOutTrainCB;
	return IsCarChainCallback(fn) ? fn : 0;
}

// Is this association one of the ped's car animations with its callback
// still to come? `engineFn` is EngineCarCallback's answer.
inline bool PendingCarCallback(int32_t type, uintptr_t engineFn, bool argIsThisPed) {
	return (type == ANIM_CB_FINISH || type == ANIM_CB_DELETE) && engineFn != 0 &&
	       argIsThisPed;
}

// What becomes of a pending one when the seat changes under it.
enum class CarCallbackEnd : uint8_t {
	Drop,        // taken off; the animation fades out at once
	FinishNow,   // run now, on the car it was started for, then as Drop
};

// Only the rolling door close is finished rather than dropped: it is what
// clears the front left door's bit in m_nGettingOutFlags, and a car left with
// that bit set has every ped who walks up to that door stand there
// (addresses.h, GettingOutFlagsAfterUnseat). It is a CAutomobile's callback -
// it swings the door through vtable 5Ch and writes its Damage - so only on
// one. Everything else in the chain is about the ped, and CoopIII is about to
// say where the ped is.
inline CarCallbackEnd EndPendingCarCallback(uintptr_t engineFn, int32_t type,
                                            bool carIsAutomobile) {
	return engineFn == CPed__PedAnimDoorCloseRollingCB && type == ANIM_CB_FINISH &&
	               carIsAutomobile
	           ? CarCallbackEnd::FinishNow
	           : CarCallbackEnd::Drop;
}

// The guard wrapper's test: run the engine's callback, or only fade the
// animation it was on. The rolling close needs a CAutomobile, the train
// get-out any car.
inline bool GuardedCarCallbackRuns(uintptr_t engineFn, bool haveCar, bool carIsAutomobile) {
	if (engineFn == CPed__PedAnimDoorCloseRollingCB)
		return haveCar && carIsAutomobile;
	return haveCar;
}

// The guards, pointed at from the two pushes and the two replay slots. Not
// fatal when they do not install: the seat changes still drop the callbacks.
bool InstallCarCallbackGuards();
void RemoveCarCallbackGuards();

// The two wrappers' addresses, for DropCarChainCallbacks to recognise.
uintptr_t RollingDoorGuardAddress();
uintptr_t OutTrainGuardAddress();

} // namespace coopiii::game
