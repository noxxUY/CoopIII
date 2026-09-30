// Turning a car's CDamageManager into six bytes and back, separated out from
// the code that touches game memory.
//
// Everything here is pure arithmetic over the constants in addresses.h, so it
// compiles and runs outside GTA III and is covered by tools/clienttest. That
// separation is not tidiness: the two dangerous things in this feature are a
// *decision* and a *bound*, and neither needs an engine to get wrong.
//
// docs/cardamage.md is the design. The three things a reader has to know
// before changing anything in this file:
//
// 1. **The door status byte is not the damage.** Two of its four values are a
//    door somebody opened, not a door somebody broke, and the engine writes
//    DOOR_STATUS_SWINGING over DOOR_STATUS_MISSING unconditionally
//    (0x004DE71E). So the byte can go down while the car still has a hole in
//    it, because SetDoorDamage has no arm that puts an atomic back. What
//    travels is a damage *level*, and DoorLevel below is that mapping.
//
// 2. **Damage only climbs.** ProgressPanelDamage refuses at 3,
//    ProgressDoorDamage refuses at 3, and nothing except CAutomobile::Fix ever
//    lowers either. So the wire word is a monotone join: MergeDamage is
//    commutative, associative and idempotent, and the order packets arrive in
//    cannot change where the session ends up. That is what lets §4 of the
//    design reuse roadmap.md §5.8's "anybody may report it" without inventing
//    a fourth ownership model.
//
// 3. **Nothing in the engine bounds a door or wheel index.**
//    CDamageManager::SetDoorStatus is `mov byte [ecx+edx+9],al` and
//    SetWheelStatus is `mov byte [ecx+edx+5],al`, neither with a compare in
//    front of it. A door index off a socket writes one byte wherever the index
//    reaches - door 24 lands in m_panelStatus, door 0x100 lands in a CDoor.
//    SetPanelStatus and SetLightStatus are gentler only by accident: they
//    compute a shift and x86 `shl` masks the count to five bits, so an
//    out-of-range panel silently aliases a valid one. Same missing check,
//    different failure. Six doors and seven panels is the bound and it is ours
//    to enforce, which is why the wire format has no room to express anything
//    else.
#pragma once

#include "addresses.h"

#include <coopiii/protocol.h>

#include <cstdint>

namespace coopiii::game {

// The wire's idea of how many doors and panels there are, against the
// engine's. protocol.h has to state the counts because the server has no
// addresses.h and must not grow one; these two lines are what stop the sdk
// from only ever agreeing with itself. Same trick as padtest --contract
// diffing its layout against the PowerShell driver's.
static_assert(VEH_DAMAGE_DOORS == NUM_DOORS,
              "the wire's door count is CDamageManager::m_doorStatus[6]");
static_assert(VEH_DAMAGE_PANELS == NUM_PANELS,
              "the wire's panel count is the seven ApplyDamage ever reaches");
static_assert(VEH_PANEL_LEVEL_MAX == PANEL_STATUS_MISSING,
              "a panel level is ePanelStatus verbatim, and it stops at 3 "
              "because ProgressPanelDamage refuses there");

// ---- doors -----------------------------------------------------------------

// What a door's raw m_doorStatus byte means for the wire.
//
// The enum order is the engine's (OK, SMASHED, SWINGING, MISSING) and it is
// not a damage ordering - SWINGING sits above SMASHED and means less. This
// collapses it to the three states that describe the car rather than the last
// thing that happened to it.
enum DoorDamageLevel : uint8_t {
	DOORDMG_NONE    = 0,   // OK, or open: nothing is broken
	DOORDMG_SMASHED = 1,   // the damaged atomic is showing
	DOORDMG_GONE    = 2,   // the door is not there
};

constexpr uint8_t DOORDMG_MAX = DOORDMG_GONE;

// One door's raw status byte -> what has to travel.
//
// DOOR_STATUS_SWINGING is deliberately level 0. A passenger getting in sets it
// and getting out clears it, twice per stop, and neither is damage. Sending it
// would put a flickering field on a packet designed to be sent a handful of
// times a session, and would occasionally ask an observer to un-lose a door -
// which SetDoorDamage could not act on anyway.
inline uint8_t DoorLevel(uint8_t rawStatus) {
	switch (rawStatus) {
	case DOOR_STATUS_SMASHED: return DOORDMG_SMASHED;
	case DOOR_STATUS_MISSING: return DOORDMG_GONE;
	case DOOR_STATUS_OK:
	case DOOR_STATUS_SWINGING:
	default:                  return DOORDMG_NONE;
	}
}

// ...and back, for the observer that has to write a byte into the car.
//
// Level 0 maps to DOOR_STATUS_OK rather than to "leave it alone" because the
// caller only ever applies a level it has already decided is an increase; see
// ApplyVehicleDamage, which skips every door whose level did not go up.
inline uint8_t DoorStatusForLevel(uint8_t level) {
	switch (level) {
	case DOORDMG_SMASHED: return DOOR_STATUS_SMASHED;
	case DOORDMG_GONE:    return DOOR_STATUS_MISSING;
	default:              return DOOR_STATUS_OK;
	}
}

// ---- lights ----------------------------------------------------------------
//
// Not on the wire, and this is the measurement that keeps it off.
//
// SetLightStatus has exactly one caller in the image - ApplyDamage's panel arm
// at 0x00545B25 - it is always passed the literal 1, and it sits two
// instructions from the ProgressPanelDamage call that moves the same panel. So
// for panels 0..4, `GetLightStatus(i) == 1` if and only if
// `GetPanelStatus(i) != 0`, both ways, for the whole life of the car.
//
// And a light is the one part of a car's damage where writing the number is
// enough: all 46 reads of GetLightStatus live in the render-preparation pass
// and every one of them is fresh, every frame. No atomic is swapped, no flag
// is cached. Contrast SetPanelDamage and SetDoorDamage, which exist precisely
// because everything else *is* cached in the clump.
inline uint32_t LightWordFromPanels(uint32_t panels) {
	uint32_t lights = 0;
	// Panels 0..4 are the four wings and the windscreen, which is the range
	// ApplyDamage's `subComp` covers. The two bumpers (5, 6) are not lights
	// and ApplyDamage never passes them to SetLightStatus.
	for (unsigned i = 0; i <= VEHPANEL_WINDSCREEN; ++i)
		if (GetPanelLevel(panels, i) != PANEL_STATUS_OK)
			lights |= (1u << (i * 2));
	return lights;
}

// ---- which engine component each index belongs to --------------------------
//
// SetPanelDamage, SetBumperDamage and SetDoorDamage all take an m_aCarNodes
// subscript as well as the panel or door index, and getting the pairing wrong
// is a silent no-op rather than a crash: each applier opens with
// `if(m_aCarNodes[component] == nil) return`. Both tables are transcribed from
// the engine's own `push` pairs - BlowUpCar for the doors and the bumpers,
// SetupDamageAfterLoad for the wings.

// ePanels -> eCarNodes. The windscreen is included even though
// SetupDamageAfterLoad skips it, because VehicleDamage's CAR_PIECE_WINDSCREEN
// arm does apply it (CAR_WINDSCREEN, VEHPANEL_WINDSCREEN).
inline uint8_t CarNodeForPanel(unsigned panel) {
	switch (panel) {
	case VEHPANEL_FRONT_LEFT:  return CAR_WING_LF;     // 0x0D, [ebx+3B0h]
	case VEHPANEL_FRONT_RIGHT: return CAR_WING_RF;     // 0x09, [ebx+3A0h]
	case VEHPANEL_REAR_LEFT:   return CAR_WING_LR;     // 0x0E, [ebx+3B4h]
	case VEHPANEL_REAR_RIGHT:  return CAR_WING_RR;     // 0x0A, [ebx+3A4h]
	case VEHPANEL_WINDSCREEN:  return CAR_WINDSCREEN;  // 0x13
	case VEHBUMPER_FRONT:      return CAR_BUMP_FRONT;  // 0x07, [ebx+398h]
	case VEHBUMPER_REAR:       return CAR_BUMP_REAR;   // 0x08, [ebx+39Ch]
	default:                   return 0;               // CAR_NODE_NONE
	}
}

// The two bumpers go through SetBumperDamage and the five panels through
// SetPanelDamage. The two functions are nearly identical - the difference is
// which COMPGROUP_ they hand SpawnFlyingComponent, which decides how the part
// tumbles - so calling the wrong one is another silent near-miss.
inline bool PanelIsBumper(unsigned panel) {
	return panel == VEHBUMPER_FRONT || panel == VEHBUMPER_REAR;
}

// eDoors -> eCarNodes, from BlowUpCar's six SetDoorDamage calls in order:
// (11h,0) (12h,1) (0Fh,2) (0Bh,3) (10h,4) (0Ch,5).
inline uint8_t CarNodeForDoor(unsigned door) {
	switch (door) {
	case DOOR_BONNET:      return CAR_BONNET;    // 0x11
	case DOOR_BOOT:        return CAR_BOOT;      // 0x12
	case DOOR_FRONT_LEFT:  return CAR_DOOR_LF;   // 0x0F
	case DOOR_FRONT_RIGHT: return CAR_DOOR_RF;   // 0x0B
	case DOOR_REAR_LEFT:   return CAR_DOOR_LR;   // 0x10
	case DOOR_REAR_RIGHT:  return CAR_DOOR_RR;   // 0x0C
	default:               return 0;             // CAR_NODE_NONE
	}
}

// ---- what a wreck looks like ------------------------------------------------
//
// CDamageManager::FuckCarCompletely, in the shape this file speaks, so a
// receiver can recognise a wreck's damage without asking the engine and a test
// can pin it. Every six doors MISSING, every panel zeroed.
//
// It is here to be *compared against*, not to be applied: a wreck's damage
// comes from the observer's own BlowUpCar (protocol.md §1.11), which is why
// wheels never need to travel. Sending a damage packet for a car that is
// already a wreck is redundant rather than wrong, and DamageGrew makes it
// cheap to notice.
constexpr uint16_t WRECK_DOOR_WORD = static_cast<uint16_t>(
    (DOORDMG_GONE << 0) | (DOORDMG_GONE << 2) | (DOORDMG_GONE << 4) |
    (DOORDMG_GONE << 6) | (DOORDMG_GONE << 8) | (DOORDMG_GONE << 10));

constexpr uint32_t WRECK_PANEL_WORD = 0;

} // namespace coopiii::game
