// The engine half of ramming a car another machine simulates: game/bump.h.
#include "bump.h"

#include "addresses.h"
#include "pedanim.h"
#include "population.h"
#include "../log.h"
#include "../quat.h"

#include <windows.h>

#include <cmath>

namespace coopiii::game {

namespace {

using ProcessControlFn = void(__thiscall *)(void *);

SpeedSamples g_samples;
uintptr_t    g_original  = 0;
bool         g_installed = false;

uint32_t Frame() { return Global<uint32_t>(CTimer__m_FrameCounter); }

void *CarFromRef(int32_t ref) {
	if (ref < 0)
		return nullptr;
	return Func<void *(__cdecl *)(int32_t)>(CPools__GetVehicle)(ref);
}

bool IsAutomobile(void *v) { return Field<int32_t>(v, offs::VEH_TYPE) == VEHICLE_TYPE_CAR; }

bool IsVehicleEntity(const void *e) {
	return (Field<uint8_t>(const_cast<void *>(e), offs::ENTITY_FLAGS) & 7) == ENTITY_TYPE_VEHICLE;
}

bool Wrecked(void *v) {
	return (Field<uint8_t>(v, offs::ENTITY_FLAGS) >> ENTITY_STATUS_SHIFT) == ENTITY_STATUS_WRECKED;
}

// CAutomobile's slot 8: the engine's own, then the note.
void __fastcall ProcessControlNoted(void *car, void * /*edx*/) {
	Func<ProcessControlFn>(g_original)(car);
	g_samples.Note(car, Frame(), ReadVec3(car, offs::MOVE_SPEED), ReadVec3(car, offs::TURN_SPEED));
}

bool WriteSlot(uintptr_t slot, uintptr_t value) {
	DWORD old = 0;
	if (!VirtualProtect(reinterpret_cast<void *>(slot), sizeof value, PAGE_READWRITE, &old))
		return false;
	*reinterpret_cast<uintptr_t *>(slot) = value;
	VirtualProtect(reinterpret_cast<void *>(slot), sizeof value, old, &old);
	return true;
}

} // namespace

bool InstallBumpWatch() {
	if (g_installed)
		return true;
	const uintptr_t slot = CAutomobile__vtable_ProcessControl;
	const uintptr_t was  = *reinterpret_cast<const uintptr_t *>(slot);
	if (was != CAutomobile__ProcessControl) {
		Log("bump: CAutomobile's ProcessControl slot holds 0x%08X, not 0x%08X; left alone, "
		    "so a car of somebody else's we ram stands like a wall as before",
		    static_cast<unsigned>(was), static_cast<unsigned>(CAutomobile__ProcessControl));
		return false;
	}
	g_original = was;
	if (!WriteSlot(slot, reinterpret_cast<uintptr_t>(&ProcessControlNoted))) {
		Log("bump: FAILED to take CAutomobile's ProcessControl slot at 0x%08X",
		    static_cast<unsigned>(slot));
		return false;
	}
	g_installed = true;
	Log("bump: took CAutomobile's ProcessControl slot; what our car's collisions do to "
	    "somebody else's car is seen and sent to its owner");
	return true;
}

void RemoveBumpWatch() {
	if (!g_installed)
		return;
	if (WriteSlot(CAutomobile__vtable_ProcessControl, g_original))
		g_installed = false;
}

bool ReadCopyContact(int32_t poolHandle, CopyContact &out) {
	out = CopyContact{};
	void *const v = CarFromRef(poolHandle);
	if (!v)
		return false;
	if (!g_installed || !IsAutomobile(v))
		return true;   // never fresh

	Vec3 move{}, turn{};
	if (!g_samples.Take(v, Frame(), move, turn))
		return true;
	out.fresh   = true;
	out.impulse = Field<float>(v, offs::VEH_DAMAGE_IMPULSE);
	out.piece   = static_cast<uint8_t>(Field<uint16_t>(v, offs::VEH_DAMAGE_PIECE_TYPE));
	out.move    = SpeedChange(ReadVec3(v, offs::MOVE_SPEED), move);
	out.turn    = SpeedChange(ReadVec3(v, offs::TURN_SPEED), turn);
	if (!std::isfinite(out.impulse))
		out.impulse = 0.0f;

	// What hit it, compared and never followed past its type byte: the engine
	// holds a reference on m_pDamageEntity, so it is live or null.
	void *const hit = Field<void *>(v, object::DAMAGE_ENTITY);
	if (!hit || hit == v || !IsVehicleEntity(hit))
		return true;
	void *const ped  = Func<void *(__cdecl *)()>(FindPlayerPed)();
	void *const ours = Func<void *(__cdecl *)()>(FindPlayerVehicle)();
	if (ped && hit == ours && Field<void *>(ours, offs::VEH_DRIVER) == ped) {
		out.byOurWheel = true;
		return true;
	}
	uint16_t hosted = INVALID_NETID;
	if (HostedCarNetIdFor(hit, hosted)) {
		out.byHosted = hosted;
		return true;
	}
	const int32_t ref = Func<int32_t(__cdecl *)(void *)>(CPools__GetVehicleRef)(hit);
	if (CarFromRef(ref) == hit)
		out.byHandle = ref;
	return true;
}

bool ReadVehiclePose(int32_t poolHandle, VehicleTransform &out) {
	void *const v = CarFromRef(poolHandle);
	if (!v)
		return false;
	out.pos = ReadVec3(v, offs::POSITION);
	out.rot = QuatFromAxes(ReadVec3(v, offs::MATRIX_RIGHT), ReadVec3(v, offs::MATRIX_FWD),
	                       ReadVec3(v, offs::MATRIX_UP));
	return true;
}

bool ApplyVehicleBump(int32_t target, int32_t by, const Vec3 &move, const Vec3 &turn,
                      float impulse, uint8_t piece) {
	void *const car    = CarFromRef(target);
	void *const rammer = CarFromRef(by);
	if (!car || !rammer || car == rammer || !IsAutomobile(car) || Wrecked(car))
		return false;
	const Vec3  a = ReadVec3(car, offs::POSITION), b = ReadVec3(rammer, offs::POSITION);
	const float dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
	if (!(dx * dx + dy * dy + dz * dz <= BUMP_APPLY_RANGE_M * BUMP_APPLY_RANGE_M))
		return false;

	// What the rammer's engine had the collision do, as ApplyMoveForce and
	// ApplyTurnForce would have done it: straight onto the speeds.
	const Vec3 m = ReadVec3(car, offs::MOVE_SPEED), t = ReadVec3(car, offs::TURN_SPEED);
	WriteVec3(car, offs::MOVE_SPEED, Vec3{m.x + move.x, m.y + move.y, m.z + move.z});
	WriteVec3(car, offs::TURN_SPEED, Vec3{t.x + turn.x, t.y + turn.y, t.z + turn.z});

	// And the dent, by the car's own VehicleDamage on its next ProcessControl,
	// which reads this record before anything clears it (addresses.h, "the one
	// door a dent comes through"). Only when it is the frame's biggest, as
	// SetDamagedPieceRecord keeps it. No culprit: our copy of the rammer is
	// nobody this engine could charge a crime or a fire to.
	if (impulse > Field<float>(car, offs::VEH_DAMAGE_IMPULSE)) {
		Field<float>(car, offs::VEH_DAMAGE_IMPULSE)       = impulse;
		Field<uint16_t>(car, offs::VEH_DAMAGE_PIECE_TYPE) = piece;
		Field<void *>(car, object::DAMAGE_ENTITY)         = nullptr;
	}
	return true;
}

} // namespace coopiii::game
