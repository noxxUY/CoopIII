#include "carextras.h"

#include "addresses.h"
#include "boat.h"
#include "carextrasync.h"

namespace coopiii::game {
namespace {

using GetCarFn = void *(__cdecl *)(int32_t handle);

// CPools::GetVehicle, so a handle whose slot has been freed or reused
// resolves to nothing rather than to whatever the engine put there next.
void *CarAt(int32_t handle) {
	return handle < 0 ? nullptr : Func<GetCarFn>(CPools__GetVehicle)(handle);
}

int32_t TypeOf(void *vehicle) { return Field<int32_t>(vehicle, offs::VEH_TYPE); }

uint16_t ModelOf(void *vehicle) {
	return static_cast<uint16_t>(Field<int16_t>(vehicle, offs::MODEL_INDEX));
}

// A car with a gun: the automobile body and one of the two models whose
// control code aims one. A boat's +0x580 is past its end.
bool HasGun(void *vehicle) {
	return vehicle && HasAutomobileBody(TypeOf(vehicle)) && CarHasGun(ModelOf(vehicle));
}

bool ReadCarAlarm(int32_t handle, uint16_t &remainingMs) {
	void *const car = CarAt(handle);
	if (!car)
		return false;
	const int16_t state = Field<int16_t>(car, offs::VEH_ALARM_STATE);
	// The engine's own test, "not 0 and not -1" (0x0056C44F). Anything else
	// below zero is not a value any writer in the image produces.
	remainingMs = state > 0 ? static_cast<uint16_t>(state) : 0;
	return true;
}

void WriteCarAlarm(int32_t handle, uint16_t remainingMs) {
	void *const car = CarAt(handle);
	if (!car)
		return;
	int16_t &state = Field<int16_t>(car, offs::VEH_ALARM_STATE);
	if (remainingMs == 0) {
		// Stopping an alarm never disarms a car that has not gone off.
		if (state != VEHICLE_ALARM_ARMED)
			state = 0;
		return;
	}
	state = static_cast<int16_t>(remainingMs > VEHICLE_ALARM_MS ? VEHICLE_ALARM_MS
	                                                            : remainingMs);
}

bool ReadCarAim(int32_t handle, float &gunLR, float &gunUD) {
	void *const car = CarAt(handle);
	if (!HasGun(car))
		return false;
	gunLR = Field<float>(car, offs::AUTO_GUN_LR);
	gunUD = Field<float>(car, offs::AUTO_GUN_UD);
	return true;
}

void WriteCarAim(int32_t handle, float gunLR, float gunUD) {
	void *const car = CarAt(handle);
	if (!HasGun(car) || !AimFinite(gunLR, gunUD))
		return;
	Field<float>(car, offs::AUTO_GUN_LR) = gunLR;
	Field<float>(car, offs::AUTO_GUN_UD) = gunUD;
}

} // namespace

void AddCarExtrasToBridge(WorldBridge &bridge) {
	bridge.ReadCarAlarm  = &ReadCarAlarm;
	bridge.WriteCarAlarm = &WriteCarAlarm;
	bridge.ReadCarAim    = &ReadCarAim;
	bridge.WriteCarAim   = &WriteCarAim;
}

uint8_t SampleCarStateFlags(void *vehicle) {
	if (!vehicle)
		return 0;
	uint8_t flags = 0;
	if (Field<uint8_t>(vehicle, offs::VEH_FLAGS_A) & offs::VEH_HANDBRAKE_ON)
		flags |= VEH_HANDBRAKE;
	if (HasAutomobileBody(TypeOf(vehicle)) &&
	    (Field<uint8_t>(vehicle, offs::AUTOMOBILE_TAXI_LIGHT_BYTE) & offs::AUTOMOBILE_TAXI_LIGHT))
		flags |= VEH_TAXI_LIGHT;
	return flags;
}

void ApplyCarStateFlags(void *vehicle, uint8_t flags, bool driven) {
	if (!vehicle)
		return;
	uint8_t &a = Field<uint8_t>(vehicle, offs::VEH_FLAGS_A);
	if (driven && (flags & VEH_HANDBRAKE))
		a = static_cast<uint8_t>(a | offs::VEH_HANDBRAKE_ON);
	else
		a = static_cast<uint8_t>(a & ~offs::VEH_HANDBRAKE_ON);
	// Bit 3 alone: the bomb's three bits below it are somebody else's.
	if (HasAutomobileBody(TypeOf(vehicle))) {
		uint8_t &b = Field<uint8_t>(vehicle, offs::AUTOMOBILE_TAXI_LIGHT_BYTE);
		if (flags & VEH_TAXI_LIGHT)
			b = static_cast<uint8_t>(b | offs::AUTOMOBILE_TAXI_LIGHT);
		else
			b = static_cast<uint8_t>(b & ~offs::AUTOMOBILE_TAXI_LIGHT);
	}
}

} // namespace coopiii::game
