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
// called from here. The respray clears the stars of the player who paid,
// through the engine's own code, on their own machine. Whose *other* stars it
// clears is the wanted rule's decision - everybody's under `shared`, the
// car's occupants under `perplayer` - and it is made in Client::OnRespray and
// carried out by Client::TickWanted (docs/wanted.md §4.9), never by letting an
// observer's own arm run, which (3) still refuses. `SEAM (wanted level)` in
// garage.cpp marks the place and stays empty on purpose.
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

// ---- whose stored cars a safehouse shows -----------------------------------
//
// Every machine keeps its own campaign and its own save (docs/campaign.md,
// "Changed since"), so each has its own CGarages::aCarsInSafeHouse. A hideout
// garage's GS_FULLYCLOSED arm restores that list when its own player walks up
// (CGarage::RestoreCarsForThisHideout), and each restored car is a
// RANDOM_VEHICLE the population hook hosts like traffic. So two players at one
// safehouse each restored their own cars into the same slots: each saw his own
// stored cars plus the other's hosted copies, on top of them.
//
// Now the first machine to open the garage restores and the rest do not. A
// machine that finds the garage already held open by another (the union the
// door sync keeps, garage.cpp) skips its own restore for that visit and tells
// the arm every car is out, which is what lets the door open; it sees the
// restorer's cars as that machine's hosted traffic. And when its door shuts on
// that visit, its own store is skipped too: the store starts by clearing the
// list, and the list still holds this player's cars, which were never taken
// out. They come back on the next visit this machine opens first.
//
// CGarage::RestoreCarsForThisHideout, __thiscall (CStoredCar *), `ret 4`,
// al = every stored car is out. Its one caller is the hideout arm:
//
//   00424BEB  mov ecx,ebp
//   00424BED  call 00427A40
//   00424BF2  mov [esp+14h],al / cmp ... / je
//   00424C01  mov byte [ebp+1],3            GS_OPENING
constexpr uintptr_t CGarage__RestoreCarsForThisHideout = 0x00427A40;
constexpr uintptr_t HIDEOUT_RESTORE_CALL               = 0x00424BED;

// Called in place of the restore: true when this machine is to leave its
// stored cars where they are this time (and it then remembers that it did).
bool SkipHideoutRestore(void *garage);
// Called by the store (game/carremoval.cpp): true, once, when this visit's
// restore was skipped here and the store must be skipped with it.
bool TakeSkippedHideoutRestore(void *garage);

// The decision, without the engine: restore here unless somebody else already
// has the garage open.
inline bool RestoreStoredCarsHere(bool heldOpenByAnother) {
	return !heldOpenByAnother;
}

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

// ---- neither machine waits for ever at a lock-up ------------------------------
//
// A garage that rests shut closes with the local player's controls taken:
// the mission lock-up's GS_OPENED arm, with the target car inside and the
// player walked out, calls SetDisablePlayerControls(PLAYERCONTROL_GARAGE) and
// goes to GS_CLOSING, and only the arrival at the bottom gives the controls
// back. A hold-open from another machine used to catch it on the way down
// (Latch or Ramp back to open), the arm ran again, took the controls again
// and closed again, every frame: the owner stood outside Van Heist's lock-up
// with no controls and no end to it.
//
// Two rules close it.
//
// 1. A close this machine's own engine started runs to the bottom. From the
//    frame its arm turns an open garage to GS_CLOSING until it is no longer
//    closing, nobody else's hold applies to it.
inline bool LocalCloseStarted(uint8_t type, uint8_t before, uint8_t after, bool ran) {
	return ran && type != GARAGE_NONE && !GarageRestsOpen(type) && GarageHeadingOpen(before) &&
	       after == GS_CLOSING;
}
inline bool LocalCloseGoesOn(bool closing, uint8_t after) {
	return closing && after == GS_CLOSING;
}
inline bool HoldApplies(bool remoteHeld, bool localReports, bool localClosing) {
	return remoteHeld && !localReports && !localClosing;
}

// 2. A garage another machine held away from rest, and left there when the
//    hold came off, is not this machine's opinion. It used to be reported the
//    frame after the release as if this engine had opened it, which held the
//    other machine's door open in turn: the guest's lock-up, opened for the
//    owner and left open, kept the owner's from ever shutting. So it is
//    reported only once this machine's own engine moves the state, or never,
//    if the engine lets it come back to rest.
inline bool ReportLocalDeviation(uint8_t type, uint8_t before, uint8_t after, bool inherited) {
	if (!GarageDeviates(type, after))
		return false;
	return !inherited || before != after;
}
inline bool StillInherited(bool inherited, uint8_t type, uint8_t before, uint8_t after) {
	return inherited && before == after && GarageDeviates(type, after);
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

// A Pay'n'Spray with our player riding beside another player (mission.h's
// R4 passengers, and any ride). The arm acts on FindPlayerVehicle(), which is
// the car a passenger sits in as much as the car a driver drives, so both
// machines resprayed the one car, each with its own pick of colour off its
// own ChooseVehicleColour cursor, and the passenger's C_Respray named no car
// (Client::SendLocalResprays sends the car we drive). Two screens, two
// colours. The paint is the driver's: his machine holds the car and his
// C_Respray paints ours. So a passenger's own arm keeps the colours the car
// had going in, which are the driver's already when his packet came first,
// and his packet paints it when it comes after. The repair and the rest of
// the arm stay as they were.
inline bool PassengerKeepsPaint(bool weDrive, bool driverIsAnotherPlayer) {
	return !weDrive && driverIsAnotherPlayer;
}

} // namespace coopiii::game
