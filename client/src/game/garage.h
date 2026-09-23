// Garages, doors and the Pay'n'Spray - the engine side.
//
// docs/protocol.md §1.16 is the design. What a reader of *this* file needs is
// four things.
//
// **1. The state transition travels, the door position does not.**
//
// A garage's state machine has two kinds of transition. Out of GS_OPENING or
// GS_CLOSING it is *derived*: ramp m_fDoorPos by the door's fixed speed times
// CTimer::ms_fTimeStep, call UpdateDoorsHeight, and when the ramp hits its
// limit take the resting state and play a sound. Every type's two moving arms
// are those same four statements. Out of a resting state it is *decided*, and
// decided from the local player - FindPlayerPed, FindPlayerVehicle,
// FindPlayerCoors, the player's money, the player's wanted level.
//
// Only the decided ones are news. Putting m_fDoorPos on the wire would be
// shipping a consequence at 25 Hz while its cause changes six times in a whole
// visit - the same mistake as sending a car's m_fHealth instead of the event
// that destroyed it (docs/protocol.md §1.11), and worse here, because
// m_fDoorPos means nothing without m_fDoorHeight and without the door CEntity,
// both of which are the map's and are resolved to a per-machine pool pointer
// by CGarage::RefreshDoorPointers. Sending a door's height would be sending a
// machine a float derived from its own map data.
//
// **2. Who is authoritative: nobody owns a garage, so it is a union.**
//
// A garage belongs to the map, not to a player. roadmap.md §5.8 settled the
// same shape for a parked car - "whoever saw it may say so, first report wins,
// and no transform travels because the map put it there on every machine" -
// and half of that carries over exactly: anybody may report, and no transform
// travels. The other half does not, and the difference is worth stating,
// because it is the difference between an event and a level. A car being
// destroyed is a fact that happens once, so first-report-wins is right and a
// second report is a duplicate. A door being open is a *level* that is true
// for as long as somebody is standing there, so first-report-wins would mean
// the first player to walk away shuts the door on the second. The rule is
// therefore a union: each machine reports whether its own state machine has a
// garage away from where that type of garage rests, and the garage is away
// from rest if anybody says so.
//
// That gives "whoever opens it, everybody sees it open" for a safehouse
// garage, and "whoever is inside it, everybody sees it shut" for a spray shop,
// out of one bit and one OR.
//
// **3. An observer must not run the arm that ends a visit.**
//
// This is the part that is not obvious and is the reason there is a detour
// here rather than a field write from PreFrame. The Pay'n'Spray's whole
// effect - the repair, the repaint, the money, and the wanted level - lives in
// the GS_FULLYCLOSED arm of *this machine's* CGarage::Update, and it acts on
// FindPlayerVehicle() and FindPlayerPed(): the local car and the local player.
// Hold that garage shut on an observer and let the engine run, and a couple of
// seconds later the observer's own car gets repainted and the observer's own
// stars get cleared because somebody across the city paid for a respray.
//
// So while a remote report is holding a serviced garage shut, this detour runs
// the ramp and then stops. The visit is the owner's; its ending is theirs to
// announce, and it arrives as C_Respray.
//
// **4. The wanted seam.**
//
// CWanted::Reset (0x004AD790) is in addresses.h and is deliberately never
// called from here. The wanted level is not on the wire at all yet
// (roadmap.md §5.1, designed and unbuilt, and owned by another agent right
// now), and the respray already clears the stars of the player who paid,
// through the engine's own code, on their own machine - that needs nothing
// from CoopIII. The only thing CoopIII does about it is *not* clear an
// observer's stars, which falls out of (3) for free.
//
// The single line to change if §5.1 ever lands a shared wanted level is marked
// `SEAM (wanted level)` in garage.cpp. It is one place, in
// ApplyRemoteRespray, and it is empty on purpose.
#pragma once

#include <cstdint>

#include "addresses.h"
#include "../client.h"

namespace coopiii::game {

// Installs the CGarage::Update detour. Not fatal if it fails: without it
// garages stay exactly as local as they are today, and the log says so.
bool InstallGarageHook();

// Adds the four garage entries to a bridge that has already been built.
void AddGaragesToBridge(WorldBridge &bridge);

// ---- the parts that do not need a running game ----------------------------
//
// These are here rather than static in the .cpp so tools/clienttest can drive
// them. They take a garage's type and state as plain bytes, which is what they
// are in memory, so a test can build a garage out of two numbers.

// What this machine should do to a garage it is being asked to hold away from
// rest, given where its door currently is. The engine's own OpenThisGarage /
// CloseThisGarage refuse to do anything to a garage that has already arrived,
// which is exactly the common case here - so the answer is not always "call
// one of them".
enum class GarageHold : uint8_t {
	// Leave it alone. Either nobody is asking, or the local engine already
	// agrees, or the door is already going the right way.
	None = 0,
	// Wind it the right way through the engine's own mutator. The door is
	// somewhere in between and the ramp has to run.
	Ramp = 1,
	// Write the arrived state straight in. The door is already at the end it
	// needs to be at, and putting it back through GS_OPENING/GS_CLOSING would
	// re-run the arrival every frame - which means the door-opened sound,
	// sixty times a second, forever.
	Latch = 2,
};

// The state a Latch writes. A hold-open lands on GS_OPENED and a hold-shut on
// GS_FULLYCLOSED, and neither is GS_OPENEDCONTAINSCAR or GS_CLOSEDCONTAINSCAR,
// because those two are claims about a car that this machine has not put in
// there.
inline uint8_t GarageLatchState(uint8_t type) {
	return GarageRestsOpen(type) ? GS_FULLYCLOSED : GS_OPENED;
}

// `held` is the union of what the other machines report for this garage.
// `doorAtEnd` is m_fDoorPos having reached the end the hold wants, which is
// m_fDoorHeight for a hold-open and 0 for a hold-shut.
inline GarageHold DecideGarageHold(uint8_t type, uint8_t state, bool held,
                                   bool doorAtEnd) {
	if (!held || type == GARAGE_NONE)
		return GarageHold::None;

	const bool holdOpen = !GarageRestsOpen(type);

	// Already going the right way, or already there: the engine's own ramp is
	// doing the work and there is nothing to add.
	if (GarageHeadingOpen(state) == holdOpen)
		return GarageHold::None;

	// Heading the wrong way. If the door has not moved off the end we want it
	// at, put the arrived state back without touching the door - the local
	// engine's resting arm decided to leave and the door has not got anywhere
	// yet. This is the ordinary case for a safehouse garage somebody else is
	// standing in: every frame the local arm says "close, the player is miles
	// away", and every frame this puts it back, silently and without moving
	// anything.
	if (doorAtEnd)
		return GarageHold::Latch;

	// The door really is somewhere in between - a hold that arrived
	// mid-close, or one that has just started. Wind it back through the
	// engine's own mutator so the ramp, the sound and UpdateDoorsHeight are
	// all the engine's.
	return GarageHold::Ramp;
}

// Whether a held garage's own Update may run at all this frame.
//
// It may while the door is moving - that arm is pure animation. It may not
// once a *serviced* garage (one that rests open: the spray shop, the three
// bomb shops, the crusher) has arrived shut, because that arm is the visit
// completing on somebody else's behalf. For a garage that rests shut there is
// nothing dangerous in its resting arm, so it keeps running and keeps the
// garage camera and the help messages working.
inline bool GarageUpdateMayRun(uint8_t type, uint8_t state, bool held) {
	if (!held)
		return true;
	// A moving arm is pure animation: ramp the door, push it to the door
	// entities, and on arrival take the resting state and play a sound.
	// Nothing in either arm decides anything about a player.
	if (state == GS_OPENING || state == GS_CLOSING)
		return true;
	// A garage that rests shut has a harmless resting arm. Its GS_OPENED arm
	// is "should I close?" and its GS_FULLYCLOSED arm is "should I open?",
	// and letting both keep running is what keeps the garage camera, the
	// "you cannot store any more cars" message and the hideout car store
	// working for the local player.
	if (!GarageRestsOpen(type))
		return true;
	// A garage that rests open has arrived shut, and that arm is the visit
	// completing: the repair, the repaint, the bomb, the crusher, the money
	// and the wanted level, all of them applied to FindPlayerVehicle() and
	// FindPlayerPed(). On this machine that is the wrong car and the wrong
	// player. The visit is somebody else's and so is its ending.
	return false;
}

} // namespace coopiii::game
