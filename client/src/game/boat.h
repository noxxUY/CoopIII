// Which class a synced vehicle is built as, and which writers may touch it
// once it exists. docs/roadmap.md M2, "Boats".
//
// A boat used to be built as a CAutomobile wearing a boat's model. Building a
// real CBoat fixes how it floats and moves, and it also means every
// CAutomobile-only offset that game/vehicle.cpp or game/ped.cpp writes lands in
// a CBoat's own members instead. Each decision about that is here, as
// arithmetic over numbers read out of the engine, so tools/clienttest can hold
// it to the disassembly without a game. addresses.h, "boats", has the
// disassembly.
#pragma once

#include <cstddef>
#include <cstdint>

#include "addresses.h"

namespace coopiii::game {

enum class VehicleBuild : uint8_t {
	None,         // not something CoopIII will construct
	Automobile,
	Boat,
};

// CREATE_CAR's own choice, from the model info rather than from a list of
// model ids: IsBoatModel first, then "not a bike" for the CAutomobile.
//
// Two differences from the engine, both refusals. IsBoatModel dereferences
// the model info without a null check, and CREATE_CAR's car arm never looks
// at m_type at all - the script only ever names vehicle models. A model id
// here came off a socket, so a missing model info or one that is not a
// vehicle's is refused instead of being read as one.
//
// Everything else that is a vehicle and not a boat or a bike is an
// Automobile, which is what CREATE_CAR does too. That includes the train,
// the heli and the plane types - no player drives those through this path,
// and what a synced train is belongs to the train work, not here.
constexpr VehicleBuild VehicleBuildFor(bool haveInfo, uint8_t infoType,
                                       int32_t vehicleType) {
	if (!haveInfo || infoType != MITYPE_VEHICLE)
		return VehicleBuild::None;
	if (vehicleType == VEHICLE_TYPE_BOAT)
		return VehicleBuild::Boat;
	if (vehicleType == VEHICLE_TYPE_BIKE)
		return VehicleBuild::None;   // CREATE_CAR builds nothing for one either
	return VehicleBuild::Automobile;
}

// What CREATE_CAR pushes before CVehicle::operator new. The pool ignores it
// and strides every slot at SIZEOF_AUTOMOBILE, which is why a boat fits.
constexpr size_t VehicleBuildSize(VehicleBuild b) {
	return b == VehicleBuild::Boat         ? offs::SIZEOF_BOAT
	       : b == VehicleBuild::Automobile ? offs::SIZEOF_AUTOMOBILE
	                                       : 0;
}

constexpr uintptr_t VehicleBuildCtor(VehicleBuild b) {
	return b == VehicleBuild::Boat         ? CBoat__ctor
	       : b == VehicleBuild::Automobile ? CAutomobile__ctor
	                                       : 0;
}

// What that constructor leaves at +0x284 - 1 from CBoat::CBoat at 0x0053E42A,
// 0 from CAutomobile::CAutomobile at 0x0052C766. -1 for None, which matches
// no eVehicleType.
constexpr int32_t VehicleBuildType(VehicleBuild b) {
	return b == VehicleBuild::Boat         ? VEHICLE_TYPE_BOAT
	       : b == VehicleBuild::Automobile ? VEHICLE_TYPE_CAR
	                                       : -1;
}

// CREATE_CAR's car arm joins the road system and its boat arm does not.
constexpr bool JoinsRoadSystemOnSpawn(VehicleBuild b) {
	return b == VehicleBuild::Automobile;
}

// 9 for a car, 20 for a boat, both out of CREATE_CAR.
constexpr float SpawnCruiseSpeed(VehicleBuild b) {
	return b == VehicleBuild::Boat ? BOAT_SPAWN_CRUISE_SPEED : 9.0f;
}

// ---- per-writer guards, all asked of the object's own m_vehType ----------

// Is there a CDamageManager at +0x288, doors and panels behind it and a
// CAutomobile::SetPanelDamage that can be called on this? Only for a car. In
// a CBoat, +0x288 is the first of the boat's own floats (the constructor
// writes 0.25f there at 0x0053E588), so reading it as a damage manager
// samples garbage and writing it corrupts the boat.
constexpr bool HasAutomobileBody(int32_t vehicleType) {
	return vehicleType == VEHICLE_TYPE_CAR;
}

// Where this vehicle keeps the five-second fire timer, or 0 if CoopIII does
// not know of one. The two are different members at different offsets and
// the car's is past the end of a boat.
constexpr size_t FireBlowUpTimerOffset(int32_t vehicleType) {
	return vehicleType == VEHICLE_TYPE_CAR    ? offs::AUTO_FIRE_BLOWUP_TIMER
	       : vehicleType == VEHICLE_TYPE_BOAT ? offs::BOAT_FIRE_BLOWUP_TIMER
	                                          : 0;
}

// Is this vehicle burning, as its own engine sees it? The health each fire
// block tests before it runs its timer - below 250 for a car (0x00534510), 150
// for a boat (0x0053F917) - or a CFire hanging off m_pCarFire, which takes
// 1.2 a step off it through InflictDamage until it goes out (0x004799FE).
// A wreck isn't burning; both blocks skip one.
constexpr bool VehicleOnFire(int32_t vehicleType, float health, bool wrecked,
                             bool carFire) {
	if (wrecked)
		return false;
	if (carFire)
		return true;
	return vehicleType == VEHICLE_TYPE_CAR    ? health < VEH_FIRE_HEALTH
	       : vehicleType == VEHICLE_TYPE_BOAT ? health < BOAT_FIRE_HEALTH
	                                          : false;
}

// Does CPed::SetObjective undo an ENTER_CAR objective on this vehicle for a
// ped that is not the player? 0x004D8519, and only for a boat.
constexpr bool SetObjectiveRefusesNonPlayer(int32_t vehicleType) {
	return vehicleType == VEHICLE_TYPE_BOAT;
}

// The vehicles CoopIII builds, by their vtable. The unseat path uses this as
// its "is this slot still what we think it is" test, the way ResolveRemote
// uses the ped vtable, so it has to name every class SpawnRemoteVehicle can
// make and nothing else.
constexpr bool IsBuiltVehicleVtable(uintptr_t vtable) {
	return vtable == CAutomobile__vtable || vtable == CBoat__vtable;
}

// Whether a replica is given the engine's animated way in. Not for a boat:
// the engine's own entry reaches SetEnterCar_AllClear's boat arm through the
// same objective SetObjective refuses a non-player for, so the replica is
// warped instead, with the objective put back by hand.
constexpr bool ReplicaMayAnimateEntry(int32_t vehicleType) {
	return vehicleType != VEHICLE_TYPE_BOAT;
}

static_assert(VehicleBuildSize(VehicleBuild::Boat) <= offs::SIZEOF_AUTOMOBILE,
              "a boat fits a vehicle pool slot");
static_assert(offs::AUTO_FIRE_BLOWUP_TIMER >= offs::SIZEOF_BOAT,
              "the car's fire timer is outside a boat, which is why it is guarded");
static_assert(offs::AUTO_DAMAGE_MANAGER == offs::SIZEOF_VEHICLE,
              "CAutomobile's first member is where CBoat's first member is too");

} // namespace coopiii::game
